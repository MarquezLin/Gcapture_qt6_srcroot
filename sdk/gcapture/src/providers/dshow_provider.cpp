
#include <windows.h>
#include <dshow.h>
#include <d3d9.h>
#include <vmr9.h>

#include "dshow_provider.h"
#include "dshow_custom_sink.h"
#include <objbase.h>
#include <dvdmedia.h>
#include <stdio.h>
#include <chrono>
#include <cstring>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace
{
    std::once_flag g_comOnce;

    void dshow_log(const char *msg)
    {
        OutputDebugStringA(msg);
    }

    void dshow_log_hr(const char *prefix, HRESULT hr)
    {
        char sys[512] = {};
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, static_cast<DWORD>(hr), 0,
                       sys, static_cast<DWORD>(sizeof(sys)), nullptr);

        char buf[1024] = {};
        sprintf_s(buf, "[DShow] %s hr=0x%08X (%s)", prefix, static_cast<unsigned>(hr), sys[0] ? sys : "n/a");
        OutputDebugStringA(buf);
    }

    static bool mediaTypeToVideoInfo(const AM_MEDIA_TYPE *pmt, int &w, int &h, int &fpsNum, int &fpsDen)
    {
        w = h = fpsNum = fpsDen = 0;
        if (!pmt || !pmt->pbFormat)
            return false;

        if (pmt->formattype == FORMAT_VideoInfo && pmt->cbFormat >= sizeof(VIDEOINFOHEADER))
        {
            auto *vih = reinterpret_cast<const VIDEOINFOHEADER *>(pmt->pbFormat);
            w = vih->bmiHeader.biWidth;
            h = abs(vih->bmiHeader.biHeight);
            if (vih->AvgTimePerFrame > 0)
            {
                fpsNum = 10000000;
                fpsDen = static_cast<int>(vih->AvgTimePerFrame);
            }
            return true;
        }
        if (pmt->formattype == FORMAT_VideoInfo2 && pmt->cbFormat >= sizeof(VIDEOINFOHEADER2))
        {
            auto *vih = reinterpret_cast<const VIDEOINFOHEADER2 *>(pmt->pbFormat);
            w = vih->bmiHeader.biWidth;
            h = abs(vih->bmiHeader.biHeight);
            if (vih->AvgTimePerFrame > 0)
            {
                fpsNum = 10000000;
                fpsDen = static_cast<int>(vih->AvgTimePerFrame);
            }
            return true;
        }
        return false;
    }

    static void freeMediaType(AM_MEDIA_TYPE *pmt)
    {
        if (!pmt)
            return;
        if (pmt->cbFormat && pmt->pbFormat)
            CoTaskMemFree(pmt->pbFormat);
        if (pmt->pUnk)
            pmt->pUnk->Release();
        CoTaskMemFree(pmt);
    }

    static const char *subtypeName(const GUID &g)
    {
        if (g == MEDIASUBTYPE_NV12)
            return "NV12";
        if (g == MEDIASUBTYPE_YUY2)
            return "YUY2";
        if (g == MEDIASUBTYPE_MJPG)
            return "MJPG";
        if (g == MEDIASUBTYPE_RGB24)
            return "RGB24";
        if (g == MEDIASUBTYPE_RGB32)
            return "RGB32";
        if (g == MEDIASUBTYPE_ARGB32)
            return "ARGB32";
        return "UNKNOWN";
    }

    static IPin *findUnconnectedPin(IBaseFilter *filter, PIN_DIRECTION dir)
    {
        if (!filter)
            return nullptr;
        IEnumPins *enumPins = nullptr;
        if (FAILED(filter->EnumPins(&enumPins)) || !enumPins)
            return nullptr;
        IPin *pin = nullptr;
        while (enumPins->Next(1, &pin, nullptr) == S_OK)
        {
            PIN_DIRECTION pd = PINDIR_INPUT;
            if (SUCCEEDED(pin->QueryDirection(&pd)) && pd == dir)
            {
                IPin *tmp = nullptr;
                HRESULT hr = pin->ConnectedTo(&tmp);
                if (tmp)
                    tmp->Release();
                if (FAILED(hr) || hr == VFW_E_NOT_CONNECTED)
                {
                    enumPins->Release();
                    return pin;
                }
            }
            pin->Release();
            pin = nullptr;
        }
        enumPins->Release();
        return nullptr;
    }

    static HRESULT connectFilters(IGraphBuilder *graph, IBaseFilter *upstream, IBaseFilter *downstream)
    {
        if (!graph || !upstream || !downstream)
            return E_POINTER;
        IPin *outPin = findUnconnectedPin(upstream, PINDIR_OUTPUT);
        if (!outPin)
            return VFW_E_NOT_FOUND;
        IPin *inPin = findUnconnectedPin(downstream, PINDIR_INPUT);
        if (!inPin)
        {
            outPin->Release();
            return VFW_E_NOT_FOUND;
        }
        HRESULT hr = graph->Connect(outPin, inPin);
        outPin->Release();
        inPin->Release();
        return hr;
    }

    static IPin *findPinByName(IBaseFilter *filter, PIN_DIRECTION dir, const wchar_t *name)
    {
        if (!filter || !name)
            return nullptr;
        IEnumPins *enumPins = nullptr;
        if (FAILED(filter->EnumPins(&enumPins)) || !enumPins)
            return nullptr;
        IPin *pin = nullptr;
        while (enumPins->Next(1, &pin, nullptr) == S_OK)
        {
            PIN_DIRECTION pd = PINDIR_INPUT;
            PIN_INFO info{};
            const bool dirOk = SUCCEEDED(pin->QueryDirection(&pd)) && pd == dir;
            const bool infoOk = SUCCEEDED(pin->QueryPinInfo(&info));
            bool nameOk = false;
            if (infoOk)
            {
                nameOk = (_wcsicmp(info.achName, name) == 0);
                if (info.pFilter)
                    info.pFilter->Release();
            }
            if (dirOk && nameOk)
            {
                enumPins->Release();
                return pin;
            }
            pin->Release();
            pin = nullptr;
        }
        enumPins->Release();
        return nullptr;
    }

    static HRESULT connectPinToFilter(IGraphBuilder *graph, IPin *outPin, IBaseFilter *downstream)
    {
        if (!graph || !outPin || !downstream)
            return E_POINTER;
        IPin *inPin = findUnconnectedPin(downstream, PINDIR_INPUT);
        if (!inPin)
            return VFW_E_NOT_FOUND;
        HRESULT hr = graph->Connect(outPin, inPin);
        inPin->Release();
        return hr;
    }

    static void disconnectAllPins(IBaseFilter *filter)
    {
        if (!filter)
            return;
        IEnumPins *enumPins = nullptr;
        if (FAILED(filter->EnumPins(&enumPins)) || !enumPins)
            return;
        IPin *pin = nullptr;
        while (enumPins->Next(1, &pin, nullptr) == S_OK)
        {
            IPin *peer = nullptr;
            if (SUCCEEDED(pin->ConnectedTo(&peer)) && peer)
            {
                peer->Disconnect();
                peer->Release();
            }
            pin->Disconnect();
            pin->Release();
            pin = nullptr;
        }
        enumPins->Release();
    }

    static void resizeArgbNearest(const std::vector<uint8_t> &src, int srcW, int srcH, int srcStride,
                                  std::vector<uint8_t> &dst, int dstW, int dstH, int &dstStride)
    {
        dstStride = dstW * 4;
        dst.resize(static_cast<size_t>(dstStride) * static_cast<size_t>(dstH));
        if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0)
            return;

        for (int y = 0; y < dstH; ++y)
        {
            const int sy = (y * srcH) / dstH;
            const uint8_t *srcRow = src.data() + static_cast<size_t>(sy) * srcStride;
            uint8_t *dstRow = dst.data() + static_cast<size_t>(y) * dstStride;
            for (int x = 0; x < dstW; ++x)
            {
                const int sx = (x * srcW) / dstW;
                const uint8_t *sp = srcRow + static_cast<size_t>(sx) * 4;
                uint8_t *dp = dstRow + static_cast<size_t>(x) * 4;
                dp[0] = sp[0];
                dp[1] = sp[1];
                dp[2] = sp[2];
                dp[3] = sp[3];
            }
        }
    }
#define HR_CHECK_LOG(x)                              \
    do                                               \
    {                                                \
        HRESULT _hr = (x);                           \
        if (FAILED(_hr))                             \
        {                                            \
            dshow_log_hr(#x, _hr);                   \
            return false;                            \
        }                                            \
        else                                         \
        {                                            \
            char _buf[512] = {};                     \
            sprintf_s(_buf, "[DShow] OK: %s\n", #x); \
            OutputDebugStringA(_buf);                \
        }                                            \
    } while (0)
}

DShowProvider::DShowProvider()
{
    ensure_com();
}

DShowProvider::~DShowProvider()
{
    stop();
    close();
    uninit_com();
}

void DShowProvider::ensure_com()
{
    std::call_once(g_comOnce, []
                   {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        dshow_log_hr("COM initialized", hr); });
}

void DShowProvider::uninit_com()
{
}

void DShowProvider::setCallbacks(gcap_on_video_cb vcb, gcap_on_error_cb ecb, void *user)
{
    std::lock_guard<std::mutex> lock(mtx_);
    vcb_ = vcb;
    ecb_ = ecb;
    user_ = user;
}

bool DShowProvider::getSignalStatus(gcap_signal_status_t &out)
{
    std::lock_guard<std::mutex> lock(mtx_);
    memset(&out, 0, sizeof(out));
    out.width = width_;
    out.height = height_;
    out.fps_num = negotiatedFpsNum_ > 0 ? negotiatedFpsNum_ : profile_.fps_num;
    out.fps_den = negotiatedFpsDen_ > 0 ? negotiatedFpsDen_ : profile_.fps_den;
    out.range = GCAP_RANGE_UNKNOWN;
    out.csp = GCAP_CSP_UNKNOWN;
    out.hdr = 0;

    if (subtype_ == MEDIASUBTYPE_NV12)
    {
        out.pixfmt = GCAP_FMT_NV12;
        out.bit_depth = 8;
    }
    else if (subtype_ == MEDIASUBTYPE_YUY2)
    {
        out.pixfmt = GCAP_FMT_YUY2;
        out.bit_depth = 8;
    }
    else
    {
        out.pixfmt = GCAP_FMT_ARGB;
        out.bit_depth = 8;
    }

    return (out.width > 0 && out.height > 0);
}

bool DShowProvider::getRuntimeInfo(gcap_runtime_info_t &out)
{
    memset(&out, 0, sizeof(out));
    if (!getSignalStatus(out.signal))
        return false;

    out.active_backend = GCAP_BACKEND_DSHOW;
    strcpy_s(out.backend_name, rawOnlyActive_ ? "DShow Raw" : "DShow");
    strcpy_s(out.path_name, rawOnlyActive_ ? "DShow Raw Preview" : "DShow VMR9 Preview");
    strcpy_s(out.frame_source, callbackSourceName(lastCallbackSource_.load()));
    if (out.frame_source[0] == '\0' || strcmp(out.frame_source, "Unknown") == 0)
        strcpy_s(out.frame_source, rawOnlyActive_ ? "RawSink" : "RendererImage");
    return true;
}

bool DShowProvider::setPreview(const gcap_preview_desc_t &desc)
{
    previewHwnd_ = reinterpret_cast<HWND>(desc.hwnd);
    char buf[128];
    sprintf_s(buf, "[DShow] setPreview hwnd=%p", previewHwnd_);
    OutputDebugStringA(buf);
    if (vmrWindowless_)
        updatePreviewRect();
    return true;
}

bool DShowProvider::enumerate(std::vector<gcap_device_info_t> &list)
{
    ensure_com();
    list.clear();

    ComPtr<ICreateDevEnum> devEnum;
    HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&devEnum));
    if (FAILED(hr))
        return false;

    ComPtr<IEnumMoniker> enumMoniker;
    hr = devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &enumMoniker, 0);
    if (hr != S_OK)
        return false;

    ComPtr<IMoniker> moniker;
    ULONG fetched = 0;
    int index = 0;

    while (enumMoniker->Next(1, &moniker, &fetched) == S_OK)
    {
        ComPtr<IPropertyBag> propBag;
        hr = moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&propBag));
        if (SUCCEEDED(hr))
        {
            VARIANT varName;
            VariantInit(&varName);

            hr = propBag->Read(L"FriendlyName", &varName, nullptr);
            if (SUCCEEDED(hr))
            {
                gcap_device_info_t di{};
                di.index = index;
                char nameBuf[128] = {};
                WideCharToMultiByte(CP_UTF8, 0, varName.bstrVal, -1,
                                    nameBuf, sizeof(nameBuf), nullptr, nullptr);
                strncpy_s(di.name, nameBuf, sizeof(di.name) - 1);
                di.caps = 0;
                di.symbolic_link[0] = 'NULL';
                list.push_back(di);
                ++index;
            }
            VariantClear(&varName);
        }
        moniker.Reset();
    }

    return !list.empty();
}

void DShowProvider::logCaptureCapabilities(IAMStreamConfig *streamConfig)
{
    if (!streamConfig)
        return;

    int capCount = 0, capSize = 0;
    if (FAILED(streamConfig->GetNumberOfCapabilities(&capCount, &capSize)) || capCount <= 0 || capSize <= 0)
        return;

    std::vector<unsigned char> caps(static_cast<size_t>(capSize));
    const int maxLog = capCount < 8 ? capCount : 8;
    for (int i = 0; i < maxLog; ++i)
    {
        AM_MEDIA_TYPE *pmt = nullptr;
        if (FAILED(streamConfig->GetStreamCaps(i, &pmt, caps.data())) || !pmt)
            continue;

        int w = 0, h = 0, fpsNum = 0, fpsDen = 0;
        mediaTypeToVideoInfo(pmt, w, h, fpsNum, fpsDen);
        double fps = (fpsNum > 0 && fpsDen > 0) ? ((double)fpsNum / (double)fpsDen) : 0.0;

        char buf[256] = {};
        sprintf_s(buf, "[DShow] cap[%d] -> %s %dx%d %.2ffps", i, subtypeName(pmt->subtype), w, h, fps);
        OutputDebugStringA(buf);

        freeMediaType(pmt);
    }
}

bool DShowProvider::configureCaptureFormat(IAMStreamConfig *streamConfig)
{
    if (!streamConfig)
        return false;

    int wantW = profile_.width;
    int wantH = profile_.height;
    int wantFpsNum = profile_.fps_num;
    int wantFpsDen = profile_.fps_den > 0 ? profile_.fps_den : 1;
    const GUID wantSubtype = (profile_.format == GCAP_FMT_NV12) ? MEDIASUBTYPE_NV12 : (profile_.format == GCAP_FMT_YUY2) ? MEDIASUBTYPE_YUY2
                                                                                                                         : GUID{};

    if (wantW <= 0 && wantH <= 0 && wantFpsNum <= 0 && wantSubtype == GUID{})
        return false;

    int capCount = 0, capSize = 0;
    if (FAILED(streamConfig->GetNumberOfCapabilities(&capCount, &capSize)) || capCount <= 0 || capSize <= 0)
        return false;

    std::vector<unsigned char> caps(static_cast<size_t>(capSize));
    AM_MEDIA_TYPE *best = nullptr;
    long long bestScore = (1LL << 60);

    for (int i = 0; i < capCount; ++i)
    {
        AM_MEDIA_TYPE *pmt = nullptr;
        if (FAILED(streamConfig->GetStreamCaps(i, &pmt, caps.data())) || !pmt)
            continue;

        int w = 0, h = 0, fpsNum = 0, fpsDen = 0;
        if (!mediaTypeToVideoInfo(pmt, w, h, fpsNum, fpsDen))
        {
            freeMediaType(pmt);
            continue;
        }

        long long score = 0;
        if (wantSubtype != GUID{})
            score += (pmt->subtype == wantSubtype) ? 0 : 1000000000LL;

        if (wantW > 0)
            score += 1000LL * llabs((long long)w - wantW);
        if (wantH > 0)
            score += 1000LL * llabs((long long)h - wantH);
        if (wantFpsNum > 0 && fpsNum > 0 && fpsDen > 0)
        {
            double fps = (double)fpsNum / (double)fpsDen;
            double wantFps = (double)wantFpsNum / (double)wantFpsDen;
            score += (long long)(100.0 * fabs(fps - wantFps));
        }

        if (score < bestScore)
        {
            freeMediaType(best);
            best = pmt;
            bestScore = score;
        }
        else
        {
            freeMediaType(pmt);
        }
    }

    if (!best)
        return false;

    HRESULT hr = streamConfig->SetFormat(best);
    if (FAILED(hr))
    {
        dshow_log_hr("IAMStreamConfig::SetFormat(best)", hr);
        freeMediaType(best);
        return false;
    }

    int w = 0, h = 0, fpsNum = 0, fpsDen = 0;
    mediaTypeToVideoInfo(best, w, h, fpsNum, fpsDen);
    char buf[256] = {};
    sprintf_s(buf, "[DShow] apply profile -> width=%d height=%d subtype={%08lX-0000-0010-...}",
              w, h, best->subtype.Data1);
    OutputDebugStringA(buf);
    freeMediaType(best);
    return true;
}

bool DShowProvider::isRawCandidate() const
{
    return (subtype_ == MEDIASUBTYPE_NV12 || subtype_ == MEDIASUBTYPE_YUY2);
}

const char *DShowProvider::callbackSourceName(CallbackSource src) const
{
    switch (src)
    {
    case CallbackSource::RawSink:
        return "RawSink";
    case CallbackSource::RendererImage:
        return "RendererImage";
    case CallbackSource::PreviewBitBlt:
        return "PreviewBitBlt";
    default:
        return "Unknown";
    }
}

bool DShowProvider::rawSinkPlanned() const
{
    return rawOnlyActive_ && rawRenderer_.isSupportedSubtype();
}

void DShowProvider::updatePreviewRect()
{
    if (!vmrWindowless_ || !previewHwnd_)
        return;
    RECT rc{};
    GetClientRect(previewHwnd_, &rc);
    vmrWindowless_->SetVideoPosition(nullptr, &rc);
}

bool DShowProvider::isRawOnlyMode() const
{
    return (previewHwnd_ == nullptr);
}

bool DShowProvider::buildPreviewGraph(ICaptureGraphBuilder2 *capBuilder)
{
    if (!capBuilder || !graph_ || !sourceFilter_)
        return false;

    previewRenderer_.Reset();
    vmrWindowless_.Reset();

    HRESULT hr = CoCreateInstance(CLSID_VideoMixingRenderer9, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&previewRenderer_));
    if (FAILED(hr))
    {
        dshow_log_hr("CoCreateInstance(CLSID_VideoMixingRenderer9)", hr);
        return false;
    }
    dshow_log("[DShow] OK: CoCreateInstance(CLSID_VideoMixingRenderer9, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&previewRenderer_))");

    hr = graph_->AddFilter(previewRenderer_.Get(), L"VMR9");
    if (FAILED(hr))
    {
        dshow_log_hr("graph_->AddFilter(previewRenderer_.Get(), L'VMR9')", hr);
        return false;
    }
    dshow_log("[DShow] OK: graph_->AddFilter(previewRenderer_.Get(), L'VMR9')");

    ComPtr<IVMRFilterConfig9> vmrConfig;
    hr = previewRenderer_.As(&vmrConfig);
    if (FAILED(hr))
    {
        dshow_log_hr("previewRenderer_.As(&vmrConfig)", hr);
        return false;
    }
    dshow_log("[DShow] OK: previewRenderer_.As(&vmrConfig)");

    hr = vmrConfig->SetNumberOfStreams(1);
    if (FAILED(hr))
    {
        dshow_log_hr("vmrConfig->SetNumberOfStreams(1)", hr);
        return false;
    }
    dshow_log("[DShow] OK: vmrConfig->SetNumberOfStreams(1)");

    hr = vmrConfig->SetRenderingMode(VMR9Mode_Windowless);
    if (FAILED(hr))
    {
        dshow_log_hr("vmrConfig->SetRenderingMode(VMR9Mode_Windowless)", hr);
        return false;
    }
    dshow_log("[DShow] OK: vmrConfig->SetRenderingMode(VMR9Mode_Windowless)");

    hr = previewRenderer_.As(&vmrWindowless_);
    if (FAILED(hr))
    {
        dshow_log_hr("previewRenderer_.As(&vmrWindowless_)", hr);
        return false;
    }
    dshow_log("[DShow] OK: previewRenderer_.As(&vmrWindowless_)");

    if (!previewHwnd_)
    {
        dshow_log("[DShow] preview hwnd is null");
        return false;
    }

    hr = vmrWindowless_->SetVideoClippingWindow(previewHwnd_);
    if (FAILED(hr))
    {
        dshow_log_hr("vmrWindowless_->SetVideoClippingWindow(previewHwnd_)", hr);
        return false;
    }
    dshow_log("[DShow] OK: vmrWindowless_->SetVideoClippingWindow(previewHwnd_)");

    hr = vmrWindowless_->SetAspectRatioMode(VMR9ARMode_LetterBox);
    if (FAILED(hr))
    {
        dshow_log_hr("vmrWindowless_->SetAspectRatioMode(VMR9ARMode_LetterBox)", hr);
        return false;
    }
    dshow_log("[DShow] OK: vmrWindowless_->SetAspectRatioMode(VMR9ARMode_LetterBox)");
    updatePreviewRect();

    hr = capBuilder->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video,
                                  sourceFilter_.Get(), nullptr, previewRenderer_.Get());
    if (FAILED(hr))
    {
        dshow_log_hr("capBuilder->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, sourceFilter_.Get(), nullptr, previewRenderer_.Get())", hr);
        return false;
    }
    dshow_log("[DShow] OK: capBuilder->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, sourceFilter_.Get(), nullptr, previewRenderer_.Get())");
    return true;
}

bool DShowProvider::buildGraphForDevice(int index)
{
    dshow_log(isRawOnlyMode() ? "[DShow] === buildGraphForDevice begin (raw-only path) ===" : "[DShow] === buildGraphForDevice begin (VMR9 preview path) ===");

    graph_.Reset();
    mediaControl_.Reset();
    mediaEvent_.Reset();
    sourceFilter_.Reset();
    previewRenderer_.Reset();
    smartTee_.Reset();
    if (rawSinkFilter_)
    {
        rawSinkFilter_->Release();
        rawSinkFilter_ = nullptr;
    }
    vmrWindowless_.Reset();
    rawOnlyActive_ = false;
    width_ = 0;
    height_ = 0;
    subtype_ = MEDIASUBTYPE_NULL;
    rawRenderer_.reset();

    HR_CHECK_LOG(CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&graph_)));
    HR_CHECK_LOG(graph_.As(&mediaControl_));
    HR_CHECK_LOG(graph_.As(&mediaEvent_));

    ComPtr<ICreateDevEnum> devEnum;
    HR_CHECK_LOG(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&devEnum)));

    ComPtr<IEnumMoniker> enumMoniker;
    HRESULT hr = devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &enumMoniker, 0);
    if (hr != S_OK)
    {
        dshow_log_hr("CreateClassEnumerator(CLSID_VideoInputDeviceCategory)", hr);
        return false;
    }
    dshow_log("[DShow] OK: CreateClassEnumerator(CLSID_VideoInputDeviceCategory)");

    ComPtr<IMoniker> moniker;
    ULONG fetched = 0;
    int cur = 0;
    while (enumMoniker->Next(1, &moniker, &fetched) == S_OK)
    {
        if (cur == index)
            break;
        moniker.Reset();
        ++cur;
    }
    if (!moniker)
    {
        dshow_log("[DShow] device moniker not found");
        return false;
    }

    {
        ComPtr<IPropertyBag> propBag;
        if (SUCCEEDED(moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&propBag))))
        {
            VARIANT varName;
            VariantInit(&varName);
            if (SUCCEEDED(propBag->Read(L"FriendlyName", &varName, nullptr)))
            {
                char nameBuf[256] = {};
                WideCharToMultiByte(CP_UTF8, 0, varName.bstrVal, -1,
                                    nameBuf, sizeof(nameBuf), nullptr, nullptr);
                char buf[320] = {};
                sprintf_s(buf, "[DShow] selected device[%d] = %s", index, nameBuf);
                OutputDebugStringA(buf);
            }
            VariantClear(&varName);
        }
    }

    HR_CHECK_LOG(moniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&sourceFilter_)));
    HR_CHECK_LOG(graph_->AddFilter(sourceFilter_.Get(), L"VideoCapture"));

    ComPtr<ICaptureGraphBuilder2> capBuilder;
    HR_CHECK_LOG(CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&capBuilder)));
    HR_CHECK_LOG(capBuilder->SetFiltergraph(graph_.Get()));

    ComPtr<IAMStreamConfig> streamConfig;
    hr = capBuilder->FindInterface(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video,
                                   sourceFilter_.Get(), IID_PPV_ARGS(&streamConfig));
    if (SUCCEEDED(hr) && streamConfig)
    {
        logCaptureCapabilities(streamConfig.Get());
        configureCaptureFormat(streamConfig.Get());
    }

    const bool rawOnly = isRawOnlyMode();
    if (rawOnly)
    {
        rawOnlyActive_ = buildRawOnlyGraph(capBuilder.Get());
        if (!rawOnlyActive_)
        {
            dshow_log("[DShow] raw-only graph build failed");
            return false;
        }
        dshow_log("[DShow] raw-only path ON: source -> CustomRawSink");
    }
    else
    {
        if (!buildPreviewGraph(capBuilder.Get()))
        {
            dshow_log("[DShow] preview graph build failed");
            return false;
        }
    }

    hr = capBuilder->FindInterface(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video,
                                   sourceFilter_.Get(), IID_PPV_ARGS(&streamConfig));
    if (SUCCEEDED(hr) && streamConfig)
    {
        AM_MEDIA_TYPE *pmt = nullptr;
        hr = streamConfig->GetFormat(&pmt);
        if (SUCCEEDED(hr) && pmt)
        {
            dshow_log("[DShow] OK: IAMStreamConfig::GetFormat()");
            if ((pmt->formattype == FORMAT_VideoInfo || pmt->formattype == FORMAT_VideoInfo2) &&
                pmt->cbFormat >= sizeof(VIDEOINFOHEADER) && pmt->pbFormat)
            {
                VIDEOINFOHEADER *vih = reinterpret_cast<VIDEOINFOHEADER *>(pmt->pbFormat);
                width_ = vih->bmiHeader.biWidth;
                height_ = abs(vih->bmiHeader.biHeight);
                subtype_ = pmt->subtype;
                negotiatedFpsNum_ = 0;
                negotiatedFpsDen_ = 0;
                mediaTypeToVideoInfo(pmt, width_, height_, negotiatedFpsNum_, negotiatedFpsDen_);

                double fps = (negotiatedFpsNum_ > 0 && negotiatedFpsDen_ > 0) ? ((double)negotiatedFpsNum_ / (double)negotiatedFpsDen_) : 0.0;
                rawRenderer_.setNegotiated(subtype_, width_, height_, negotiatedFpsNum_, negotiatedFpsDen_);
                char buf[256] = {};
                sprintf_s(buf, "[DShow] stream format: %s %dx%d %.2ffps",
                          subtypeName(subtype_), width_, height_, fps);
                OutputDebugStringA(buf);
            }
            if (pmt->cbFormat && pmt->pbFormat)
                CoTaskMemFree(pmt->pbFormat);
            if (pmt->pUnk)
                pmt->pUnk->Release();
            CoTaskMemFree(pmt);
        }
    }

    dshow_log(rawOnly ? "[DShow] === buildGraphForDevice success (raw-only path) ===" : "[DShow] === buildGraphForDevice success (VMR9 preview path) ===");
    return true;
}

bool DShowProvider::buildRawOnlyGraph(ICaptureGraphBuilder2 *capBuilder)
{
    if (!capBuilder || !graph_ || !sourceFilter_)
        return false;

    rawRenderer_.reset();

    auto cleanupPartialGraph = [this]()
    {
        disconnectAllPins(rawSinkFilter_);
        disconnectAllPins(sourceFilter_.Get());
        if (rawSinkFilter_)
        {
            graph_->RemoveFilter(rawSinkFilter_);
            rawSinkFilter_->Release();
            rawSinkFilter_ = nullptr;
        }
    };

    rawSinkFilter_ = new DShowCustomSinkFilter(&rawRenderer_);
    if (!rawSinkFilter_)
    {
        dshow_log("[DShow] raw sink allocation failed");
        return false;
    }

    HRESULT hr = graph_->AddFilter(rawSinkFilter_, L"GCapCustomRawSink");
    if (FAILED(hr))
    {
        dshow_log_hr("graph_->AddFilter(CustomRawSink)", hr);
        cleanupPartialGraph();
        return false;
    }

    hr = capBuilder->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video,
                                  sourceFilter_.Get(), nullptr, rawSinkFilter_);
    if (FAILED(hr))
    {
        dshow_log_hr("capBuilder->RenderStream(source->CustomRawSink)", hr);
        cleanupPartialGraph();
        return false;
    }

    dshow_log("[DShow] raw-only graph connected");
    return true;
}

bool DShowProvider::open(int index)
{
    char buf[128];
    sprintf_s(buf, "[DShow] open() current preview_hwnd_=%p", previewHwnd_);
    OutputDebugStringA(buf);
    ensure_com();
    dshow_log("[DShow] open() begin");
    close();
    std::lock_guard<std::mutex> lock(mtx_);

    if (!buildGraphForDevice(index))
    {
        dshow_log("[DShow] open() failed");
        return false;
    }

    currentIndex_ = index;
    updatePreviewRect();
    dshow_log("[DShow] open() success");
    {
        char msg[256] = {};
        sprintf_s(msg, "[DShow] raw-path prep active: negotiated=%s %dx%d %.2ffps raw-candidate=%s raw-sink-plan=%s",
                  subtypeName(subtype_), width_, height_,
                  (negotiatedFpsNum_ > 0 && negotiatedFpsDen_ > 0) ? ((double)negotiatedFpsNum_ / (double)negotiatedFpsDen_) : 0.0,
                  isRawCandidate() ? "YES" : "NO",
                  rawSinkPlanned() ? "CUSTOM_V4_RAW_PREVIEW" : "NO");
        OutputDebugStringA(msg);
    }
    return true;
}

bool DShowProvider::setProfile(const gcap_profile_t &p)
{
    std::lock_guard<std::mutex> lock(mtx_);
    profile_ = p;
    return true;
}

bool DShowProvider::setBuffers(int, size_t)
{
    return true;
}

bool DShowProvider::captureRawFrameToArgb(std::vector<uint8_t> &out, int &w, int &h, int &stride)
{
    if (!rawRenderer_.copyLatestFrameToArgb(out, w, h, stride))
    {
        static bool once = false;
        if (!once)
        {
            once = true;
            char msg[256] = {};
            sprintf_s(msg, "[DShow] rawRenderer copyLatestFrameToArgb = NO hasFrame=%s sampleCount=%llu bytes=%llu",
                      rawRenderer_.hasFrame() ? "YES" : "NO",
                      static_cast<unsigned long long>(rawRenderer_.sampleCount()),
                      static_cast<unsigned long long>(rawRenderer_.lastSampleBytes()));
            dshow_log(msg);
        }
        return false;
    }
    static bool onceOk = false;
    if (!onceOk)
    {
        onceOk = true;
        char msg[256] = {};
        sprintf_s(msg, "[DShow] rawRenderer copyLatestFrameToArgb = YES sampleCount=%llu bytes=%llu",
                  static_cast<unsigned long long>(rawRenderer_.sampleCount()),
                  static_cast<unsigned long long>(rawRenderer_.lastSampleBytes()));
        dshow_log(msg);
    }
    lastCallbackSource_ = CallbackSource::RawSink;
    return true;
}

bool DShowProvider::captureRendererFrameToArgb(std::vector<uint8_t> &out, int &w, int &h, int &stride)
{
    if (!vmrWindowless_)
        return false;

    BYTE *dib = nullptr;
    if (FAILED(vmrWindowless_->GetCurrentImage(&dib)) || !dib)
        return false;

    auto *bih = reinterpret_cast<BITMAPINFOHEADER *>(dib);
    if (bih->biSize < sizeof(BITMAPINFOHEADER) || bih->biBitCount != 32 || bih->biCompression != BI_RGB)
    {
        CoTaskMemFree(dib);
        return false;
    }

    w = bih->biWidth;
    h = std::abs(bih->biHeight);
    if (w <= 0 || h <= 0)
    {
        CoTaskMemFree(dib);
        return false;
    }

    const int srcStride = w * 4;
    const uint8_t *srcBits = reinterpret_cast<const uint8_t *>(dib + bih->biSize + bih->biClrUsed * sizeof(RGBQUAD));
    stride = srcStride;
    out.resize(static_cast<size_t>(stride) * static_cast<size_t>(h));

    if (bih->biHeight > 0)
    {
        for (int y = 0; y < h; ++y)
        {
            const uint8_t *srcRow = srcBits + static_cast<size_t>(h - 1 - y) * srcStride;
            uint8_t *dstRow = out.data() + static_cast<size_t>(y) * stride;
            memcpy(dstRow, srcRow, static_cast<size_t>(stride));
        }
    }
    else
    {
        memcpy(out.data(), srcBits, out.size());
    }

    CoTaskMemFree(dib);
    return true;
}

bool DShowProvider::capturePreviewFrameToArgb(std::vector<uint8_t> &out, int &w, int &h, int &stride)
{
    if (captureRendererFrameToArgb(out, w, h, stride))
    {
        lastCallbackSource_ = CallbackSource::RendererImage;
        return true;
    }

    if (!previewHwnd_)
        return false;

    RECT rc{};
    if (!GetClientRect(previewHwnd_, &rc))
        return false;
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0)
        return false;

    HDC wndDc = GetDC(previewHwnd_);
    if (!wndDc)
        return false;
    HDC memDc = CreateCompatibleDC(wndDc);
    if (!memDc)
    {
        ReleaseDC(previewHwnd_, wndDc);
        return false;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void *bits = nullptr;
    HBITMAP hbmp = CreateDIBSection(wndDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbmp || !bits)
    {
        if (hbmp)
            DeleteObject(hbmp);
        DeleteDC(memDc);
        ReleaseDC(previewHwnd_, wndDc);
        return false;
    }

    HGDIOBJ old = SelectObject(memDc, hbmp);
    BOOL bltOk = BitBlt(memDc, 0, 0, w, h, wndDc, 0, 0, SRCCOPY | CAPTUREBLT);

    stride = w * 4;
    if (bltOk)
    {
        out.resize(static_cast<size_t>(stride) * static_cast<size_t>(h));
        memcpy(out.data(), bits, out.size());
    }

    SelectObject(memDc, old);
    DeleteObject(hbmp);
    DeleteDC(memDc);
    ReleaseDC(previewHwnd_, wndDc);
    if (bltOk == TRUE)
        lastCallbackSource_ = CallbackSource::PreviewBitBlt;
    return bltOk == TRUE;
}

bool DShowProvider::captureCallbackFrameToArgb(std::vector<uint8_t> &out, int &w, int &h, int &stride)
{
    std::vector<uint8_t> tmp;
    int cw = 0, ch = 0, cstride = 0;

    if (rawOnlyActive_)
    {
        if (!captureRawFrameToArgb(tmp, cw, ch, cstride))
            return false;
    }
    else
    {
        if (!captureRawFrameToArgb(tmp, cw, ch, cstride) && !capturePreviewFrameToArgb(tmp, cw, ch, cstride))
            return false;
    }

    const int targetW = width_ > 0 ? width_ : cw;
    const int targetH = height_ > 0 ? height_ : ch;
    if (cw == targetW && ch == targetH)
    {
        out.swap(tmp);
        w = cw;
        h = ch;
        stride = cstride;
        return true;
    }

    resizeArgbNearest(tmp, cw, ch, cstride, out, targetW, targetH, stride);
    w = targetW;
    h = targetH;
    return true;
}

void DShowProvider::mirrorLoop()
{
    dshow_log("[DShow] mirrorLoop begin");
    while (mirrorThreadRunning_)
    {
        std::vector<uint8_t> buf;
        int w = 0, h = 0, stride = 0;
        if (captureCallbackFrameToArgb(buf, w, h, stride))
        {
            gcap_on_video_cb vcb = nullptr;
            void *user = nullptr;
            {
                std::lock_guard<std::mutex> lock(mtx_);
                vcb = vcb_;
                user = user_;
            }
            if (vcb && !buf.empty())
            {
                gcap_frame_t f{};
                f.data[0] = buf.data();
                f.stride[0] = stride;
                f.plane_count = 1;
                f.width = w;
                f.height = h;
                f.format = GCAP_FMT_ARGB;
                if (f.frame_id == 0)
                {
                }
                auto now = std::chrono::steady_clock::now().time_since_epoch();
                f.pts_ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
                f.frame_id = ++frameCounter_;
                if (f.frame_id == 1)
                {
                    char msg[256] = {};
                    double fps = (negotiatedFpsNum_ > 0 && negotiatedFpsDen_ > 0) ? ((double)negotiatedFpsNum_ / (double)negotiatedFpsDen_) : 0.0;
                    sprintf_s(msg, "[DShow] callback frame -> %d x %d fmt=ARGB negotiated=%s %d x %d %.2ffps source=%s raw-candidate=%s raw-sink-plan=%s",
                              w, h, subtypeName(subtype_), width_, height_, fps,
                              callbackSourceName(lastCallbackSource_.load()),
                              isRawCandidate() ? "YES" : "NO",
                              rawSinkPlanned() ? "CUSTOM_V4_RAW_PREVIEW" : "NO");
                    OutputDebugStringA(msg);
                    if (isRawCandidate())
                    {
                        char rawMsg[256] = {};
                        sprintf_s(rawMsg, "[DShow] raw state: hasFrame=%s sampleCount=%llu bytes=%llu",
                                  rawRenderer_.hasFrame() ? "YES" : "NO",
                                  static_cast<unsigned long long>(rawRenderer_.sampleCount()),
                                  static_cast<unsigned long long>(rawRenderer_.lastSampleBytes()));
                        OutputDebugStringA(rawMsg);
                    }
                }
                vcb(&f, user);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(mirrorSleepMs()));
    }
    dshow_log("[DShow] mirrorLoop end");
}

int DShowProvider::mirrorSleepMs() const
{
    int fpsNum = negotiatedFpsNum_ > 0 ? negotiatedFpsNum_ : profile_.fps_num;
    int fpsDen = negotiatedFpsDen_ > 0 ? negotiatedFpsDen_ : profile_.fps_den;
    if (fpsNum > 0)
    {
        double fps = fpsDen > 0 ? (double)fpsNum / (double)fpsDen : (double)fpsNum;
        if (fps >= 1.0)
        {
            int ms = (int)(1000.0 / fps + 0.5);
            if (ms < 5)
                ms = 5;
            if (ms > 1000)
                ms = 1000;
            return ms;
        }
    }
    return 33;
}

void DShowProvider::startMirrorThread()
{
    stopMirrorThread();
    mirrorThreadRunning_ = true;
    mirrorThread_ = std::thread([this]
                                { mirrorLoop(); });
}

void DShowProvider::stopMirrorThread()
{
    mirrorThreadRunning_ = false;
    if (mirrorThread_.joinable())
        mirrorThread_.join();
}

bool DShowProvider::start()
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (!mediaControl_)
        return false;

    HRESULT hr = mediaControl_->Run();
    if (FAILED(hr))
    {
        dshow_log_hr("mediaControl_->Run()", hr);
        if (ecb_)
            ecb_(GCAP_EIO, "DShow: Run() failed", user_);
        return false;
    }

    dshow_log("[DShow] start() OK: mediaControl_->Run()");
    if (vmrWindowless_ && previewHwnd_)
    {
        updatePreviewRect();
        vmrWindowless_->RepaintVideo(previewHwnd_, nullptr);
    }
    running_ = true;
    if (vcb_)
        startMirrorThread();
    return true;
}

void DShowProvider::stop()
{
    stopMirrorThread();
    std::lock_guard<std::mutex> lock(mtx_);
    if (mediaControl_ && running_)
    {
        mediaControl_->Stop();
        dshow_log("[DShow] stop()");
        running_ = false;
    }
}

void DShowProvider::close()
{
    dshow_log("[DShow] close()");
    stop();

    std::lock_guard<std::mutex> lock(mtx_);
    vmrWindowless_.Reset();
    previewRenderer_.Reset();
    smartTee_.Reset();
    if (rawSinkFilter_)
    {
        rawSinkFilter_->Release();
        rawSinkFilter_ = nullptr;
    }
    sourceFilter_.Reset();
    mediaEvent_.Reset();
    mediaControl_.Reset();
    graph_.Reset();
    width_ = 0;
    height_ = 0;
    subtype_ = MEDIASUBTYPE_NULL;
    currentIndex_ = -1;
}
