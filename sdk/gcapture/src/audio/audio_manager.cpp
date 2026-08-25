#include "audio_manager.h"
#include "dshow_audio_monitor.h"
#include "wasapi_renderer.h"

#include "../core/logging.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
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

        std::string out(static_cast<size_t>(len), '\0');
        if (WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), len, nullptr, nullptr) <= 0)
            return {};
        out.pop_back();
        return out;
    }

    std::wstring utf8_to_wide(const char *s)
    {
        if (!s || !*s)
            return {};
        const int len = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
        if (len <= 0)
            return {};
        std::wstring out(static_cast<size_t>(len), L'\0');
        if (MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), len) <= 0)
            return {};
        out.pop_back();
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

    class WasapiMonitor
    {
    public:
        ~WasapiMonitor() { stop(); }

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
                sprintf_s(buf, "[AudioMonitoring] %s hr=0x%08X", message, static_cast<unsigned>(hr));
            else
                sprintf_s(buf, "[AudioMonitoring] %s", message);
            lastError_ = buf;
            gcap_log_warn(buf);
            failed_.store(true);
            running_.store(false);
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

            ComPtr<IAudioClient> captureClient;
            hr = captureDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(captureClient.GetAddressOf()));
            if (FAILED(hr) || !captureClient)
            {
                fail("capture IAudioClient activate failed", hr);
                return;
            }
            CoTaskMemWaveFormat captureMix;
            hr = captureClient->GetMixFormat(captureMix.put());
            if (FAILED(hr) || !captureMix.get() || !is_supported_pcm_like(captureMix.get()))
            {
                fail("capture mix format unsupported", hr);
                return;
            }
            const REFERENCE_TIME bufferDuration = 1000000; // 100 ms
            hr = captureClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, bufferDuration, 0, captureMix.get(), nullptr);
            if (FAILED(hr))
            {
                fail("capture IAudioClient Initialize failed", hr);
                return;
            }
            ComPtr<IAudioCaptureClient> capture;
            hr = captureClient->GetService(IID_PPV_ARGS(&capture));
            if (FAILED(hr) || !capture)
            {
                fail("IAudioCaptureClient unavailable", hr);
                return;
            }
            gcap::audio::wasapi_renderer renderer;
            std::string rendererError;
            if (!renderer.start(captureMix.get(), &rendererError))
            {
                fail(rendererError.empty() ? "WASAPI renderer start failed" : rendererError.c_str());
                return;
            }

            hr = captureClient->Start();
            if (FAILED(hr))
            {
                renderer.stop();
                fail("capture Start failed", hr);
                return;
            }

            gcap::log_printf(GCAP_LOG_INFO,
                             "[AudioMonitoring] started capture %u ch %u Hz -> speaker %u ch %u Hz",
                             captureMix.get()->nChannels,
                             captureMix.get()->nSamplesPerSec,
                             renderer.channels(),
                             renderer.sample_rate());

            ready_.store(true);

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

                const bool rendered = renderer.write(src, packetFrames, flags, running_, &rendererError);
                capture->ReleaseBuffer(packetFrames);
                if (!rendered && running_.load())
                {
                    fail(rendererError.empty() ? "WASAPI renderer write failed" : rendererError.c_str());
                    break;
                }
            }

            captureClient->Stop();
            renderer.stop();
            ready_.store(false);
            gcap_log_info("[AudioMonitoring] stopped");
        }

        std::atomic<bool> running_{false};
        std::atomic<bool> ready_{false};
        std::atomic<bool> failed_{false};
        std::thread worker_;
        std::string deviceId_;
        std::string lastError_;
    };

    std::mutex g_previewMutex;
    std::unique_ptr<WasapiMonitor> g_monitor;
    std::unique_ptr<gcap::audio::dshow_monitor> g_dshowMonitor;
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

    bool start_monitoring(const char *deviceIdUtf8, std::string *error)
    {
        std::lock_guard<std::mutex> lock(g_previewMutex);
        auto next = std::make_unique<WasapiMonitor>();
        if (!next->start(deviceIdUtf8, error))
            return false;
        g_monitor = std::move(next);
        return true;
    }

    bool start_dshow_monitoring(const char *videoDeviceNameUtf8, device &source, std::string *error)
    {
        std::lock_guard<std::mutex> lock(g_previewMutex);
        auto next = std::make_unique<dshow_monitor>();
        if (!next->start(videoDeviceNameUtf8, source, error))
            return false;
        g_dshowMonitor = std::move(next);
        return true;
    }

    void stop_monitoring()
    {
        std::unique_ptr<WasapiMonitor> old;
        std::unique_ptr<dshow_monitor> oldDshow;
        {
            std::lock_guard<std::mutex> lock(g_previewMutex);
            old = std::move(g_monitor);
            oldDshow = std::move(g_dshowMonitor);
        }
        if (old)
            old->stop();
        if (oldDshow)
            oldDshow->stop();
    }
}
