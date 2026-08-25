#include "wasapi_renderer.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
    struct CoTaskMemWaveFormat
    {
        WAVEFORMATEX *ptr = nullptr;
        ~CoTaskMemWaveFormat() { if (ptr) CoTaskMemFree(ptr); }
        WAVEFORMATEX **put() { return &ptr; }
        WAVEFORMATEX *get() const { return ptr; }
    };

    bool is_float_format(const WAVEFORMATEX *fmt)
    {
        if (!fmt)
            return false;
        if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
            return true;
        if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
            fmt->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
        {
            const auto *ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(fmt);
            return ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        }
        return false;
    }

    int valid_bits_per_sample(const WAVEFORMATEX *fmt)
    {
        if (!fmt)
            return 0;
        if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
            fmt->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
        {
            const auto *ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(fmt);
            if (ext->Samples.wValidBitsPerSample > 0)
                return ext->Samples.wValidBitsPerSample;
        }
        return fmt->wBitsPerSample;
    }

    bool is_supported_pcm_like(const WAVEFORMATEX *fmt)
    {
        if (!fmt || fmt->nChannels == 0 || fmt->nSamplesPerSec == 0 || fmt->nBlockAlign == 0)
            return false;
        if (fmt->wFormatTag == WAVE_FORMAT_PCM || fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
            return true;
        if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
            fmt->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
        {
            const auto *ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(fmt);
            return ext->SubFormat == KSDATAFORMAT_SUBTYPE_PCM ||
                   ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        }
        return false;
    }

    int bytes_per_sample(const WAVEFORMATEX *fmt)
    {
        if (!fmt || fmt->nChannels == 0)
            return 0;
        const int fromAlign = fmt->nBlockAlign / fmt->nChannels;
        return fromAlign > 0 ? fromAlign : (fmt->wBitsPerSample + 7) / 8;
    }

    float clamp_unit(float value)
    {
        return (std::max)(-1.0f, (std::min)(1.0f, value));
    }

    float read_sample(const uint8_t *frame, const WAVEFORMATEX *fmt, int channel)
    {
        if (!frame || !fmt)
            return 0.0f;
        const int bytes = bytes_per_sample(fmt);
        const uint8_t *src = frame + static_cast<size_t>(channel) * static_cast<size_t>(bytes);
        if (is_float_format(fmt))
        {
            if (bytes == 4)
            {
                float value = 0.0f;
                std::memcpy(&value, src, sizeof(value));
                return std::isfinite(value) ? clamp_unit(value) : 0.0f;
            }
            return 0.0f;
        }
        const int bits = valid_bits_per_sample(fmt);
        if (bytes == 1 || bits == 8)
            return (static_cast<int>(*src) - 128) / 128.0f;
        if (bytes == 2)
        {
            int16_t value = 0;
            std::memcpy(&value, src, sizeof(value));
            return static_cast<float>(value) / 32768.0f;
        }
        if (bytes == 3)
        {
            int32_t value = static_cast<int32_t>(src[0]) |
                            (static_cast<int32_t>(src[1]) << 8) |
                            (static_cast<int32_t>(src[2]) << 16);
            if (value & 0x00800000)
                value |= static_cast<int32_t>(0xFF000000);
            return static_cast<float>(value) / 8388608.0f;
        }
        if (bytes >= 4)
        {
            int32_t value = 0;
            std::memcpy(&value, src, sizeof(value));
            return static_cast<float>(value) / 2147483648.0f;
        }
        return 0.0f;
    }

    void write_sample(uint8_t *frame, const WAVEFORMATEX *fmt, int channel, float value)
    {
        if (!frame || !fmt)
            return;
        value = clamp_unit(value);
        const int bytes = bytes_per_sample(fmt);
        uint8_t *dst = frame + static_cast<size_t>(channel) * static_cast<size_t>(bytes);
        if (is_float_format(fmt))
        {
            if (bytes == 4)
                std::memcpy(dst, &value, sizeof(value));
            return;
        }
        const int bits = valid_bits_per_sample(fmt);
        if (bytes == 1 || bits == 8)
        {
            *dst = static_cast<uint8_t>((std::max)(0, (std::min)(255, static_cast<int>(std::lrint(value * 127.0f + 128.0f)))));
        }
        else if (bytes == 2)
        {
            const int16_t sample = static_cast<int16_t>((std::max)(-32768, (std::min)(32767, static_cast<int>(std::lrint(value * 32767.0f)))));
            std::memcpy(dst, &sample, sizeof(sample));
        }
        else if (bytes == 3)
        {
            const int sample = (std::max)(-8388608, (std::min)(8388607, static_cast<int>(std::lrint(value * 8388607.0f))));
            dst[0] = static_cast<uint8_t>(sample & 0xFF);
            dst[1] = static_cast<uint8_t>((sample >> 8) & 0xFF);
            dst[2] = static_cast<uint8_t>((sample >> 16) & 0xFF);
        }
        else if (bytes >= 4)
        {
            const int64_t scaled = static_cast<int64_t>(std::llround(value * 2147483647.0f));
            const int32_t sample = static_cast<int32_t>((std::max)(static_cast<int64_t>(INT32_MIN),
                                                                   (std::min)(static_cast<int64_t>(INT32_MAX), scaled)));
            std::memcpy(dst, &sample, sizeof(sample));
        }
    }

    std::vector<uint8_t> clone_wave_format(const WAVEFORMATEX *fmt)
    {
        std::vector<uint8_t> result;
        if (!fmt)
            return result;
        result.resize(sizeof(WAVEFORMATEX) + fmt->cbSize);
        std::memcpy(result.data(), fmt, result.size());
        return result;
    }

    void convert_audio(const uint8_t *src, uint32_t srcFrames, uint32_t srcFlags,
                       const WAVEFORMATEX *srcFmt, const WAVEFORMATEX *dstFmt,
                       std::vector<uint8_t> &out, uint32_t &outFrames)
    {
        out.clear();
        outFrames = 0;
        if (!srcFmt || !dstFmt || srcFrames == 0 || srcFmt->nSamplesPerSec == 0 || dstFmt->nSamplesPerSec == 0)
            return;
        outFrames = static_cast<uint32_t>((std::max)(uint64_t{1},
            (static_cast<uint64_t>(srcFrames) * dstFmt->nSamplesPerSec + srcFmt->nSamplesPerSec - 1) /
                srcFmt->nSamplesPerSec));
        out.resize(static_cast<size_t>(outFrames) * dstFmt->nBlockAlign);
        const bool silent = (srcFlags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || !src;
        for (uint32_t outFrame = 0; outFrame < outFrames; ++outFrame)
        {
            const uint32_t sourceFrame = (std::min)(srcFrames - 1,
                static_cast<uint32_t>((static_cast<uint64_t>(outFrame) * srcFmt->nSamplesPerSec) /
                                      dstFmt->nSamplesPerSec));
            const uint8_t *source = silent ? nullptr : src + static_cast<size_t>(sourceFrame) * srcFmt->nBlockAlign;
            uint8_t *destination = out.data() + static_cast<size_t>(outFrame) * dstFmt->nBlockAlign;
            for (int channel = 0; channel < dstFmt->nChannels; ++channel)
            {
                float sample = 0.0f;
                if (!silent)
                {
                    if (dstFmt->nChannels == 1 && srcFmt->nChannels > 1)
                    {
                        for (int sourceChannel = 0; sourceChannel < srcFmt->nChannels; ++sourceChannel)
                            sample += read_sample(source, srcFmt, sourceChannel);
                        sample /= static_cast<float>(srcFmt->nChannels);
                    }
                    else
                    {
                        const int sourceChannel = srcFmt->nChannels == 1 ? 0 : (std::min)(channel, static_cast<int>(srcFmt->nChannels) - 1);
                        sample = read_sample(source, srcFmt, sourceChannel);
                    }
                }
                write_sample(destination, dstFmt, channel, sample);
            }
        }
    }
}

namespace gcap::audio
{
    struct wasapi_renderer::impl
    {
        ComPtr<IAudioClient> client;
        ComPtr<IAudioRenderClient> render;
        std::vector<uint8_t> sourceFormatBytes;
        std::vector<uint8_t> renderFormatBytes;
        uint32_t bufferFrames = 0;
        bool started = false;
    };

    wasapi_renderer::wasapi_renderer() : impl_(std::make_unique<impl>()) {}
    wasapi_renderer::~wasapi_renderer() { stop(); }

    bool wasapi_renderer::start(const WAVEFORMATEX *sourceFormat, std::string *error)
    {
        stop();
        if (!is_supported_pcm_like(sourceFormat))
        {
            if (error) *error = "source PCM format unsupported";
            return false;
        }

        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> device;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        if (SUCCEEDED(hr))
            hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (SUCCEEDED(hr))
            hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void **>(impl_->client.GetAddressOf()));
        if (FAILED(hr) || !impl_->client)
        {
            if (error) *error = "default render endpoint open failed";
            stop();
            return false;
        }

        CoTaskMemWaveFormat mixFormat;
        hr = impl_->client->GetMixFormat(mixFormat.put());
        if (FAILED(hr) || !is_supported_pcm_like(mixFormat.get()))
        {
            if (error) *error = "render mix format unavailable";
            stop();
            return false;
        }

        WAVEFORMATEX *closest = nullptr;
        const HRESULT sourceSupport = impl_->client->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, sourceFormat, &closest);
        if (closest)
            CoTaskMemFree(closest);
        impl_->sourceFormatBytes = clone_wave_format(sourceFormat);
        impl_->renderFormatBytes = clone_wave_format(sourceSupport == S_OK ? sourceFormat : mixFormat.get());
        auto *renderFormat = reinterpret_cast<WAVEFORMATEX *>(impl_->renderFormatBytes.data());

        constexpr REFERENCE_TIME kBufferDuration = 1000000; // 100 ms
        hr = impl_->client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, kBufferDuration, 0, renderFormat, nullptr);
        if (SUCCEEDED(hr))
            hr = impl_->client->GetBufferSize(&impl_->bufferFrames);
        if (SUCCEEDED(hr) && impl_->bufferFrames == 0)
            hr = E_FAIL;
        if (SUCCEEDED(hr))
            hr = impl_->client->GetService(IID_PPV_ARGS(&impl_->render));
        if (SUCCEEDED(hr))
            hr = impl_->client->Start();
        if (FAILED(hr) || !impl_->render)
        {
            if (error) *error = "WASAPI renderer initialization failed";
            stop();
            return false;
        }
        impl_->started = true;
        return true;
    }

    bool wasapi_renderer::write(const uint8_t *sourceData, uint32_t sourceFrames, uint32_t sourceFlags,
                                const std::atomic<bool> &keepRunning, std::string *error)
    {
        if (!impl_->started || !impl_->client || !impl_->render)
        {
            if (error) *error = "WASAPI renderer is not started";
            return false;
        }
        const auto *sourceFormat = reinterpret_cast<const WAVEFORMATEX *>(impl_->sourceFormatBytes.data());
        const auto *renderFormat = reinterpret_cast<const WAVEFORMATEX *>(impl_->renderFormatBytes.data());
        std::vector<uint8_t> converted;
        uint32_t convertedFrames = 0;
        convert_audio(sourceData, sourceFrames, sourceFlags, sourceFormat, renderFormat, converted, convertedFrames);

        uint32_t written = 0;
        while (keepRunning.load() && written < convertedFrames)
        {
            uint32_t padding = 0;
            HRESULT hr = impl_->client->GetCurrentPadding(&padding);
            if (FAILED(hr))
            {
                if (error) *error = "GetCurrentPadding failed";
                return false;
            }
            const uint32_t available = padding < impl_->bufferFrames ? impl_->bufferFrames - padding : 0;
            if (available == 0)
            {
                Sleep(3);
                continue;
            }
            const uint32_t framesNow = (std::min)(available, convertedFrames - written);
            BYTE *destination = nullptr;
            hr = impl_->render->GetBuffer(framesNow, &destination);
            if (FAILED(hr) || !destination)
            {
                if (error) *error = "IAudioRenderClient::GetBuffer failed";
                return false;
            }
            std::memcpy(destination,
                        converted.data() + static_cast<size_t>(written) * renderFormat->nBlockAlign,
                        static_cast<size_t>(framesNow) * renderFormat->nBlockAlign);
            hr = impl_->render->ReleaseBuffer(framesNow, 0);
            if (FAILED(hr))
            {
                if (error) *error = "IAudioRenderClient::ReleaseBuffer failed";
                return false;
            }
            written += framesNow;
        }
        return keepRunning.load();
    }

    void wasapi_renderer::stop()
    {
        if (!impl_)
            return;
        if (impl_->started && impl_->client)
            impl_->client->Stop();
        impl_->started = false;
        impl_->render.Reset();
        impl_->client.Reset();
        impl_->sourceFormatBytes.clear();
        impl_->renderFormatBytes.clear();
        impl_->bufferFrames = 0;
    }

    uint32_t wasapi_renderer::sample_rate() const
    {
        if (!impl_ || impl_->renderFormatBytes.empty())
            return 0;
        return reinterpret_cast<const WAVEFORMATEX *>(impl_->renderFormatBytes.data())->nSamplesPerSec;
    }

    uint16_t wasapi_renderer::channels() const
    {
        if (!impl_ || impl_->renderFormatBytes.empty())
            return 0;
        return reinterpret_cast<const WAVEFORMATEX *>(impl_->renderFormatBytes.data())->nChannels;
    }
}
