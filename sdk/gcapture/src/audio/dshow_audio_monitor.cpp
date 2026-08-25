#include "dshow_audio_monitor.h"

#include "../core/logging.h"

#include <windows.h>
#include <mmreg.h>
#include <dshow.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <string>

using Microsoft::WRL::ComPtr;

namespace
{
    std::string wide_to_utf8(const wchar_t *text)
    {
        if (!text)
            return {};
        const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
        if (size <= 0)
            return {};
        std::string result(static_cast<size_t>(size), '\0');
        if (WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr) <= 0)
            return {};
        result.pop_back();
        return result;
    }

    std::string lower_ascii(std::string text)
    {
        for (char &ch : text)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return text;
    }

    std::string normalized_device_name(std::string text)
    {
        text = lower_ascii(std::move(text));
        for (char &ch : text)
            if (!std::isalnum(static_cast<unsigned char>(ch)))
                ch = ' ';

        std::string result;
        std::string token;
        for (size_t i = 0; i <= text.size(); ++i)
        {
            const char ch = i < text.size() ? text[i] : ' ';
            if (ch != ' ')
            {
                token.push_back(ch);
                continue;
            }
            if (!token.empty() && token != "video" && token != "audio" && token != "capture")
            {
                if (!result.empty())
                    result.push_back(' ');
                result += token;
            }
            token.clear();
        }
        return result;
    }

    int match_score(const std::string &videoName, const std::string &audioName)
    {
        const std::string videoKey = normalized_device_name(videoName);
        const std::string audioKey = normalized_device_name(audioName);
        if (videoKey.empty() || audioKey.empty())
            return 0;
        if (videoKey == audioKey)
            return 100;
        if (videoKey.find(audioKey) != std::string::npos || audioKey.find(videoKey) != std::string::npos)
            return 70;

        int score = 0;
        size_t begin = 0;
        while (begin < videoKey.size())
        {
            const size_t end = videoKey.find(' ', begin);
            const std::string token = videoKey.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
            if (token.size() >= 3 && audioKey.find(token) != std::string::npos)
                score += 20;
            if (end == std::string::npos)
                break;
            begin = end + 1;
        }
        return score;
    }

    std::string moniker_name(IMoniker *moniker)
    {
        if (!moniker)
            return {};
        ComPtr<IPropertyBag> properties;
        if (FAILED(moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&properties))) || !properties)
            return {};
        VARIANT value;
        VariantInit(&value);
        std::string name;
        if (SUCCEEDED(properties->Read(L"FriendlyName", &value, nullptr)) && value.vt == VT_BSTR)
            name = wide_to_utf8(value.bstrVal);
        VariantClear(&value);
        return name;
    }

    bool read_connected_audio_format(IBaseFilter *source, gcap::audio::device &info)
    {
        if (!source)
            return false;
        ComPtr<IEnumPins> pins;
        if (FAILED(source->EnumPins(&pins)) || !pins)
            return false;

        ComPtr<IPin> pin;
        while (pins->Next(1, pin.ReleaseAndGetAddressOf(), nullptr) == S_OK)
        {
            PIN_DIRECTION direction = PINDIR_INPUT;
            if (FAILED(pin->QueryDirection(&direction)) || direction != PINDIR_OUTPUT)
                continue;
            AM_MEDIA_TYPE media{};
            if (FAILED(pin->ConnectionMediaType(&media)))
                continue;
            const bool audio = media.majortype == MEDIATYPE_Audio &&
                               media.formattype == FORMAT_WaveFormatEx &&
                               media.pbFormat && media.cbFormat >= sizeof(WAVEFORMATEX);
            if (audio)
            {
                const auto *wave = reinterpret_cast<const WAVEFORMATEX *>(media.pbFormat);
                info.channels = wave->nChannels;
                info.sample_rate = static_cast<int>(wave->nSamplesPerSec);
                info.bits_per_sample = wave->wBitsPerSample;
                info.is_float = wave->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
            }
            if (media.cbFormat && media.pbFormat)
                CoTaskMemFree(media.pbFormat);
            if (media.pUnk)
                media.pUnk->Release();
            if (audio)
                return true;
        }
        return false;
    }
}

namespace gcap::audio
{
    struct dshow_monitor::impl
    {
        ComPtr<IGraphBuilder> graph;
        ComPtr<ICaptureGraphBuilder2> builder;
        ComPtr<IMediaControl> control;
        ComPtr<IBaseFilter> source;
        bool comInitialized = false;
    };

    dshow_monitor::dshow_monitor() : impl_(std::make_unique<impl>()) {}
    dshow_monitor::~dshow_monitor() { stop(); }

    bool dshow_monitor::start(const char *videoDeviceNameUtf8, device &sourceInfo, std::string *error)
    {
        stop();
        if (!videoDeviceNameUtf8 || !*videoDeviceNameUtf8)
        {
            if (error)
                *error = "video device name is empty";
            return false;
        }

        const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        impl_->comInitialized = SUCCEEDED(comHr);
        if (FAILED(comHr) && comHr != RPC_E_CHANGED_MODE)
        {
            if (error)
                *error = "CoInitializeEx failed";
            return false;
        }

        ComPtr<ICreateDevEnum> deviceEnum;
        HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&deviceEnum));
        if (FAILED(hr) || !deviceEnum)
        {
            if (error)
                *error = "DirectShow device enumerator create failed";
            stop();
            return false;
        }

        ComPtr<IEnumMoniker> monikers;
        hr = deviceEnum->CreateClassEnumerator(CLSID_AudioInputDeviceCategory, &monikers, 0);
        if (hr != S_OK || !monikers)
        {
            if (error)
                *error = "no DirectShow audio input devices";
            stop();
            return false;
        }

        ComPtr<IMoniker> bestMoniker;
        std::string bestName;
        int bestScore = 0;
        ComPtr<IMoniker> moniker;
        while (monikers->Next(1, moniker.ReleaseAndGetAddressOf(), nullptr) == S_OK)
        {
            const std::string name = moniker_name(moniker.Get());
            const int score = match_score(videoDeviceNameUtf8, name);
            if (score > bestScore)
            {
                bestScore = score;
                bestName = name;
                bestMoniker = moniker;
            }
        }
        if (!bestMoniker || bestScore < 40)
        {
            if (error)
                *error = "no matching DirectShow audio filter";
            stop();
            return false;
        }

        hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&impl_->graph));
        if (SUCCEEDED(hr))
            hr = CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&impl_->builder));
        if (SUCCEEDED(hr))
            hr = impl_->builder->SetFiltergraph(impl_->graph.Get());
        if (SUCCEEDED(hr))
            hr = bestMoniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&impl_->source));
        if (SUCCEEDED(hr))
            hr = impl_->graph->AddFilter(impl_->source.Get(), L"AudioCapture");
        if (SUCCEEDED(hr))
            hr = impl_->builder->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Audio,
                                              impl_->source.Get(), nullptr, nullptr);
        if (SUCCEEDED(hr))
            hr = impl_->graph.As(&impl_->control);
        if (SUCCEEDED(hr))
            hr = impl_->control->Run();
        if (FAILED(hr))
        {
            if (error)
            {
                char message[160] = {};
                sprintf_s(message, "DirectShow audio graph start failed hr=0x%08X", static_cast<unsigned>(hr));
                *error = message;
            }
            stop();
            return false;
        }

        sourceInfo = {};
        sourceInfo.id = "dshow:" + bestName;
        sourceInfo.name = bestName;
        read_connected_audio_format(impl_->source.Get(), sourceInfo);
        gcap::log_printf(GCAP_LOG_INFO,
                         "[AudioMonitoring][DShow] started filter=%s format=%d Hz/%d ch/%d bit",
                         sourceInfo.name.c_str(), sourceInfo.sample_rate, sourceInfo.channels,
                         sourceInfo.bits_per_sample);
        return true;
    }

    void dshow_monitor::stop()
    {
        if (!impl_)
            return;
        if (impl_->control)
            impl_->control->Stop();
        impl_->control.Reset();
        impl_->source.Reset();
        impl_->builder.Reset();
        impl_->graph.Reset();
        if (impl_->comInitialized)
        {
            CoUninitialize();
            impl_->comInitialized = false;
        }
    }
}
