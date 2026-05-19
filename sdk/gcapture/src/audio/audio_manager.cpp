#include "audio_manager.h"

#include "../core/logging.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

namespace
{
    std::string wide_to_utf8(const wchar_t *w)
    {
        if (!w)
            return {};

        int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        if (len <= 0)
            return {};

        std::string out(static_cast<size_t>(len - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), len, nullptr, nullptr);
        return out;
    }

    std::wstring utf8_to_wide(const char *s)
    {
        if (!s || !*s)
            return {};
        const int len = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
        if (len <= 0)
            return {};
        std::wstring out(static_cast<size_t>(len - 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), len);
        return out;
    }

    class ComInit
    {
    public:
        ComInit() : hr_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
        ~ComInit()
        {
            if (SUCCEEDED(hr_))
                CoUninitialize();
        }
        HRESULT hr() const { return hr_; }
        bool ok() const { return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE; }

    private:
        HRESULT hr_ = E_FAIL;
    };

    struct CoTaskMemWaveFormat
    {
        WAVEFORMATEX *ptr = nullptr;
        ~CoTaskMemWaveFormat()
        {
            if (ptr)
                CoTaskMemFree(ptr);
        }
        WAVEFORMATEX **put() { return &ptr; }
        WAVEFORMATEX *get() const { return ptr; }
    };

    bool is_pcm_subformat(const GUID &g)
    {
        return g == KSDATAFORMAT_SUBTYPE_PCM;
    }

    bool is_float_subformat(const GUID &g)
    {
        return g == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    }

    bool is_float_format(const WAVEFORMATEX *fmt)
    {
        if (!fmt)
            return false;
        if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
            return true;
        if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
        {
            const auto *ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(fmt);
            return is_float_subformat(ext->SubFormat);
        }
        return false;
    }

    int bits_per_sample(const WAVEFORMATEX *fmt)
    {
        if (!fmt)
            return 0;
        if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
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
        if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
        {
            const auto *ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(fmt);
            return is_pcm_subformat(ext->SubFormat) || is_float_subformat(ext->SubFormat);
        }
        return false;
    }

    int bytes_per_sample(const WAVEFORMATEX *fmt)
    {
        if (!fmt || fmt->nChannels == 0)
            return 0;
        const int fromAlign = fmt->nBlockAlign / fmt->nChannels;
        if (fromAlign > 0)
            return fromAlign;
        return (fmt->wBitsPerSample + 7) / 8;
    }

    float clamp_unit(float v)
    {
        if (v > 1.0f)
            return 1.0f;
        if (v < -1.0f)
            return -1.0f;
        return v;
    }

    float read_sample(const uint8_t *frame, const WAVEFORMATEX *fmt, int channel)
    {
        if (!frame || !fmt)
            return 0.0f;

        const int bps = bytes_per_sample(fmt);
        const uint8_t *p = frame + static_cast<size_t>(channel) * static_cast<size_t>(bps);

        if (is_float_format(fmt))
        {
            if (bps == 4)
            {
                float v = 0.0f;
                std::memcpy(&v, p, sizeof(v));
                return std::isfinite(v) ? clamp_unit(v) : 0.0f;
            }
            return 0.0f;
        }

        const int bits = bits_per_sample(fmt);
        if (bps == 1 || bits == 8)
            return (static_cast<int>(*p) - 128) / 128.0f;
        if (bps == 2)
        {
            int16_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            return static_cast<float>(v) / 32768.0f;
        }
        if (bps == 3)
        {
            int32_t v = static_cast<int32_t>(p[0]) |
                        (static_cast<int32_t>(p[1]) << 8) |
                        (static_cast<int32_t>(p[2]) << 16);
            if (v & 0x00800000)
                v |= static_cast<int32_t>(0xFF000000);
            return static_cast<float>(v) / 8388608.0f;
        }
        if (bps >= 4)
        {
            int32_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            return static_cast<float>(v) / 2147483648.0f;
        }
        return 0.0f;
    }

    void write_sample(uint8_t *frame, const WAVEFORMATEX *fmt, int channel, float value)
    {
        if (!frame || !fmt)
            return;

        value = clamp_unit(value);
        const int bps = bytes_per_sample(fmt);
        uint8_t *p = frame + static_cast<size_t>(channel) * static_cast<size_t>(bps);

        if (is_float_format(fmt))
        {
            if (bps == 4)
                std::memcpy(p, &value, sizeof(value));
            return;
        }

        const int bits = bits_per_sample(fmt);
        if (bps == 1 || bits == 8)
        {
            const int v = (std::max)(0, (std::min)(255, static_cast<int>(std::lrint(value * 127.0f + 128.0f))));
            *p = static_cast<uint8_t>(v);
        }
        else if (bps == 2)
        {
            const int v = (std::max)(-32768, (std::min)(32767, static_cast<int>(std::lrint(value * 32767.0f))));
            const int16_t s = static_cast<int16_t>(v);
            std::memcpy(p, &s, sizeof(s));
        }
        else if (bps == 3)
        {
            const int v = (std::max)(-8388608, (std::min)(8388607, static_cast<int>(std::lrint(value * 8388607.0f))));
            p[0] = static_cast<uint8_t>(v & 0xFF);
            p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
            p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
        }
        else if (bps >= 4)
        {
            const int64_t scaled = static_cast<int64_t>(std::llround(value * 2147483647.0f));
            const int32_t v = static_cast<int32_t>((std::max)(static_cast<int64_t>(INT32_MIN), (std::min)(static_cast<int64_t>(INT32_MAX), scaled)));
            std::memcpy(p, &v, sizeof(v));
        }
    }

    void convert_audio(const uint8_t *src,
                       UINT32 srcFrames,
                       DWORD srcFlags,
                       const WAVEFORMATEX *srcFmt,
                       const WAVEFORMATEX *dstFmt,
                       std::vector<uint8_t> &out,
                       UINT32 &outFrames)
    {
        out.clear();
        outFrames = 0;
        if (!srcFmt || !dstFmt || srcFrames == 0 || srcFmt->nSamplesPerSec == 0 || dstFmt->nSamplesPerSec == 0)
            return;

        const UINT32 dstFrames64 = static_cast<UINT32>(
            (std::max)(uint64_t{1},
                       (static_cast<uint64_t>(srcFrames) * dstFmt->nSamplesPerSec + srcFmt->nSamplesPerSec - 1) /
                           srcFmt->nSamplesPerSec));
        outFrames = dstFrames64;
        out.resize(static_cast<size_t>(outFrames) * dstFmt->nBlockAlign);

        const bool silent = (srcFlags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || !src;
        const int srcChannels = srcFmt->nChannels;
        const int dstChannels = dstFmt->nChannels;

        for (UINT32 of = 0; of < outFrames; ++of)
        {
            const UINT32 sf = (std::min)(srcFrames - 1,
                                         static_cast<UINT32>((static_cast<uint64_t>(of) * srcFmt->nSamplesPerSec) /
                                                             dstFmt->nSamplesPerSec));
            const uint8_t *srcFrame = silent ? nullptr : src + static_cast<size_t>(sf) * srcFmt->nBlockAlign;
            uint8_t *dstFrame = out.data() + static_cast<size_t>(of) * dstFmt->nBlockAlign;

            for (int dc = 0; dc < dstChannels; ++dc)
            {
                float sample = 0.0f;
                if (!silent)
                {
                    if (dstChannels == 1 && srcChannels > 1)
                    {
                        for (int sc = 0; sc < srcChannels; ++sc)
                            sample += read_sample(srcFrame, srcFmt, sc);
                        sample /= static_cast<float>(srcChannels);
                    }
                    else
                    {
                        const int sc = (srcChannels == 1) ? 0 : (std::min)(dc, srcChannels - 1);
                        sample = read_sample(srcFrame, srcFmt, sc);
                    }
                }
                write_sample(dstFrame, dstFmt, dc, sample);
            }
        }
    }

    size_t wave_format_size(const WAVEFORMATEX *fmt)
    {
        if (!fmt)
            return 0;
        return sizeof(WAVEFORMATEX) + fmt->cbSize;
    }

    std::vector<uint8_t> clone_wave_format(const WAVEFORMATEX *fmt)
    {
        std::vector<uint8_t> out;
        const size_t size = wave_format_size(fmt);
        if (size > 0)
        {
            out.resize(size);
            std::memcpy(out.data(), fmt, size);
        }
        return out;
    }

    std::string lower_ascii(std::string s)
    {
        for (char &c : s)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        return s;
    }

    std::vector<std::string> tokenize_name(const std::string &name)
    {
        std::vector<std::string> out;
        std::string cur;
        for (char ch : lower_ascii(name))
        {
            const bool alphaNum = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
            if (alphaNum)
            {
                cur.push_back(ch);
                continue;
            }
            if (cur.size() >= 3)
                out.push_back(cur);
            cur.clear();
        }
        if (cur.size() >= 3)
            out.push_back(cur);

        out.erase(std::remove_if(out.begin(), out.end(), [](const std::string &t) {
                      return t == "usb" || t == "hdmi" || t == "video" || t == "audio" ||
                             t == "capture" || t == "device" || t == "interface" || t == "digital";
                  }),
                  out.end());
        return out;
    }

    int match_score(const std::string &captureName, const gcap::audio::device &audioDevice)
    {
        const std::string cap = lower_ascii(captureName);
        const std::string aud = lower_ascii(audioDevice.name);
        if (cap.empty() || aud.empty())
            return 0;

        int score = 0;
        if (aud.find(cap) != std::string::npos || cap.find(aud) != std::string::npos)
            score += 100;
        if (aud.find("capture") != std::string::npos)
            score += 10;
        if (aud.find("hdmi") != std::string::npos)
            score += 6;
        if (cap.find("capture") != std::string::npos &&
            (aud.find("digital audio interface") != std::string::npos ||
             aud.find("line") != std::string::npos ||
             aud.find("usb audio") != std::string::npos))
            score += 22;
        if (aud.find("microphone") != std::string::npos || aud.find("mic") != std::string::npos)
            score -= 20;

        const auto capTokens = tokenize_name(captureName);
        const auto audTokens = tokenize_name(audioDevice.name);
        for (const auto &ct : capTokens)
            for (const auto &at : audTokens)
                if (ct == at)
                    score += 25;
                else if (ct.size() >= 5 && at.find(ct) != std::string::npos)
                    score += 12;

        return score;
    }

    class WasapiPreview
    {
    public:
        ~WasapiPreview() { stop(); }

        bool start(const char *deviceIdUtf8, std::string *error)
        {
            stop();
            deviceId_ = deviceIdUtf8 ? deviceIdUtf8 : "";
            running_.store(true);
            ready_.store(false);
            failed_.store(false);
            worker_ = std::thread([this]() { run(); });

            for (int i = 0; i < 100; ++i)
            {
                if (ready_.load())
                    return true;
                if (failed_.load())
                    break;
                Sleep(10);
            }

            if (!ready_.load())
            {
                if (error)
                    *error = lastError_.empty() ? "WASAPI preview did not start" : lastError_;
                stop();
                return false;
            }
            return true;
        }

        void stop()
        {
            running_.store(false);
            if (worker_.joinable())
                worker_.join();
            ready_.store(false);
        }

    private:
        void fail(const char *message, HRESULT hr = S_OK)
        {
            char buf[512] = {};
            if (FAILED(hr))
                sprintf_s(buf, "[AudioPreview] %s hr=0x%08X", message, static_cast<unsigned>(hr));
            else
                sprintf_s(buf, "[AudioPreview] %s", message);
            lastError_ = buf;
            gcap_log_warn(buf);
            failed_.store(true);
            running_.store(false);
        }

        bool choose_render_format(IAudioClient *renderClient,
                                  const WAVEFORMATEX *captureFmt,
                                  const WAVEFORMATEX *renderMixFmt,
                                  std::vector<uint8_t> &formatBytes)
        {
            if (renderClient && captureFmt && is_supported_pcm_like(captureFmt))
            {
                WAVEFORMATEX *closest = nullptr;
                const HRESULT hr = renderClient->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, captureFmt, &closest);
                if (closest)
                    CoTaskMemFree(closest);
                if (hr == S_OK)
                {
                    formatBytes = clone_wave_format(captureFmt);
                    return !formatBytes.empty();
                }
            }
            if (renderMixFmt && is_supported_pcm_like(renderMixFmt))
            {
                formatBytes = clone_wave_format(renderMixFmt);
                return !formatBytes.empty();
            }
            return false;
        }

        void run()
        {
            ComInit com;
            if (!com.ok())
            {
                fail("CoInitializeEx failed", com.hr());
                return;
            }

            ComPtr<IMMDeviceEnumerator> enumerator;
            HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
            if (FAILED(hr) || !enumerator)
            {
                fail("MMDeviceEnumerator create failed", hr);
                return;
            }

            ComPtr<IMMDevice> captureDevice;
            if (!deviceId_.empty())
            {
                const std::wstring wid = utf8_to_wide(deviceId_.c_str());
                hr = enumerator->GetDevice(wid.c_str(), &captureDevice);
            }
            else
            {
                hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &captureDevice);
            }
            if (FAILED(hr) || !captureDevice)
            {
                fail("capture endpoint open failed", hr);
                return;
            }

            ComPtr<IMMDevice> renderDevice;
            hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &renderDevice);
            if (FAILED(hr) || !renderDevice)
            {
                fail("default render endpoint open failed", hr);
                return;
            }

            ComPtr<IAudioClient> captureClient;
            ComPtr<IAudioClient> renderClient;
            hr = captureDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(captureClient.GetAddressOf()));
            if (FAILED(hr) || !captureClient)
            {
                fail("capture IAudioClient activate failed", hr);
                return;
            }
            hr = renderDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(renderClient.GetAddressOf()));
            if (FAILED(hr) || !renderClient)
            {
                fail("render IAudioClient activate failed", hr);
                return;
            }

            CoTaskMemWaveFormat captureMix;
            CoTaskMemWaveFormat renderMix;
            hr = captureClient->GetMixFormat(captureMix.put());
            if (FAILED(hr) || !captureMix.get() || !is_supported_pcm_like(captureMix.get()))
            {
                fail("capture mix format unsupported", hr);
                return;
            }
            hr = renderClient->GetMixFormat(renderMix.put());
            if (FAILED(hr) || !renderMix.get())
            {
                fail("render mix format unavailable", hr);
                return;
            }

            std::vector<uint8_t> renderFormatBytes;
            if (!choose_render_format(renderClient.Get(), captureMix.get(), renderMix.get(), renderFormatBytes))
            {
                fail("render format unsupported");
                return;
            }
            WAVEFORMATEX *renderFmt = reinterpret_cast<WAVEFORMATEX *>(renderFormatBytes.data());

            const REFERENCE_TIME bufferDuration = 1000000; // 100 ms
            hr = captureClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, bufferDuration, 0, captureMix.get(), nullptr);
            if (FAILED(hr))
            {
                fail("capture IAudioClient Initialize failed", hr);
                return;
            }
            hr = renderClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, bufferDuration, 0, renderFmt, nullptr);
            if (FAILED(hr))
            {
                fail("render IAudioClient Initialize failed", hr);
                return;
            }

            UINT32 renderBufferFrames = 0;
            renderClient->GetBufferSize(&renderBufferFrames);
            if (renderBufferFrames == 0)
            {
                fail("render buffer size is zero");
                return;
            }

            ComPtr<IAudioCaptureClient> capture;
            ComPtr<IAudioRenderClient> render;
            hr = captureClient->GetService(IID_PPV_ARGS(&capture));
            if (FAILED(hr) || !capture)
            {
                fail("IAudioCaptureClient unavailable", hr);
                return;
            }
            hr = renderClient->GetService(IID_PPV_ARGS(&render));
            if (FAILED(hr) || !render)
            {
                fail("IAudioRenderClient unavailable", hr);
                return;
            }

            hr = renderClient->Start();
            if (FAILED(hr))
            {
                fail("render Start failed", hr);
                return;
            }
            hr = captureClient->Start();
            if (FAILED(hr))
            {
                renderClient->Stop();
                fail("capture Start failed", hr);
                return;
            }

            gcap::log_printf(GCAP_LOG_INFO,
                             "[AudioPreview] started capture %u ch %u Hz -> speaker %u ch %u Hz",
                             captureMix.get()->nChannels,
                             captureMix.get()->nSamplesPerSec,
                             renderFmt->nChannels,
                             renderFmt->nSamplesPerSec);

            ready_.store(true);

            std::vector<uint8_t> converted;
            while (running_.load())
            {
                UINT32 packetFrames = 0;
                hr = capture->GetNextPacketSize(&packetFrames);
                if (FAILED(hr))
                    break;
                if (packetFrames == 0)
                {
                    Sleep(5);
                    continue;
                }

                BYTE *src = nullptr;
                DWORD flags = 0;
                UINT64 devicePosition = 0;
                UINT64 qpcPosition = 0;
                hr = capture->GetBuffer(&src, &packetFrames, &flags, &devicePosition, &qpcPosition);
                if (FAILED(hr))
                    break;

                UINT32 convertedFrames = 0;
                convert_audio(src, packetFrames, flags, captureMix.get(), renderFmt, converted, convertedFrames);

                UINT32 written = 0;
                while (running_.load() && written < convertedFrames)
                {
                    UINT32 padding = 0;
                    if (FAILED(renderClient->GetCurrentPadding(&padding)))
                        break;
                    const UINT32 available = (padding < renderBufferFrames) ? (renderBufferFrames - padding) : 0;
                    if (available == 0)
                    {
                        Sleep(3);
                        continue;
                    }

                    const UINT32 framesNow = (std::min)(available, convertedFrames - written);
                    BYTE *dst = nullptr;
                    hr = render->GetBuffer(framesNow, &dst);
                    if (FAILED(hr) || !dst)
                        break;
                    std::memcpy(dst,
                                converted.data() + static_cast<size_t>(written) * renderFmt->nBlockAlign,
                                static_cast<size_t>(framesNow) * renderFmt->nBlockAlign);
                    render->ReleaseBuffer(framesNow, 0);
                    written += framesNow;
                }

                capture->ReleaseBuffer(packetFrames);
            }

            captureClient->Stop();
            renderClient->Stop();
            ready_.store(false);
            gcap_log_info("[AudioPreview] stopped");
        }

        std::atomic<bool> running_{false};
        std::atomic<bool> ready_{false};
        std::atomic<bool> failed_{false};
        std::thread worker_;
        std::string deviceId_;
        std::string lastError_;
    };

    std::mutex g_previewMutex;
    std::unique_ptr<WasapiPreview> g_preview;
}

namespace gcap::audio
{
    std::vector<device> enumerate_devices()
    {
        std::vector<device> out;

        ComInit com;
        if (!com.ok())
            return out;

        ComPtr<IMMDeviceEnumerator> enumerator;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) || !enumerator)
            return out;

        std::wstring defaultId;
        {
            ComPtr<IMMDevice> def;
            if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &def)) && def)
            {
                LPWSTR wid = nullptr;
                if (SUCCEEDED(def->GetId(&wid)) && wid)
                {
                    defaultId = wid;
                    CoTaskMemFree(wid);
                }
            }
        }

        ComPtr<IMMDeviceCollection> collection;
        if (FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection)) || !collection)
            return out;

        UINT count = 0;
        collection->GetCount(&count);

        for (UINT i = 0; i < count; ++i)
        {
            ComPtr<IMMDevice> dev;
            if (FAILED(collection->Item(i, &dev)) || !dev)
                continue;

            device info;
            std::wstring idW;

            LPWSTR wid = nullptr;
            if (SUCCEEDED(dev->GetId(&wid)) && wid)
            {
                idW = wid;
                info.id = wide_to_utf8(wid);
                CoTaskMemFree(wid);
            }
            info.is_default = (!defaultId.empty() && idW == defaultId);

            ComPtr<IPropertyStore> props;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && props)
            {
                PROPVARIANT v;
                PropVariantInit(&v);
                if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR)
                    info.name = wide_to_utf8(v.pwszVal);
                PropVariantClear(&v);
            }

            ComPtr<IAudioClient> client;
            if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(client.GetAddressOf()))) && client)
            {
                CoTaskMemWaveFormat wfx;
                if (SUCCEEDED(client->GetMixFormat(wfx.put())) && wfx.get())
                {
                    info.channels = wfx.get()->nChannels;
                    info.sample_rate = wfx.get()->nSamplesPerSec;
                    info.bits_per_sample = bits_per_sample(wfx.get());
                    info.is_float = is_float_format(wfx.get());
                }
            }

            out.push_back(info);
        }

        return out;
    }

    bool find_device_for_capture_name(const std::string &captureNameUtf8, device &out)
    {
        const auto devices = enumerate_devices();
        int bestScore = 0;
        bool found = false;
        for (const auto &d : devices)
        {
            const int score = match_score(captureNameUtf8, d);
            if (score > bestScore)
            {
                bestScore = score;
                out = d;
                found = true;
            }
        }
        return found && bestScore >= 20;
    }

    bool start_preview(const char *deviceIdUtf8, std::string *error)
    {
        std::lock_guard<std::mutex> lock(g_previewMutex);
        auto next = std::make_unique<WasapiPreview>();
        if (!next->start(deviceIdUtf8, error))
            return false;
        g_preview = std::move(next);
        return true;
    }

    void stop_preview()
    {
        std::unique_ptr<WasapiPreview> old;
        {
            std::lock_guard<std::mutex> lock(g_previewMutex);
            old = std::move(g_preview);
        }
        if (old)
            old->stop();
    }
}
