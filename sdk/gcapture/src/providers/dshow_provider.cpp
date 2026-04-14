
#include <windows.h>
#include <dshow.h>
#include <d3d9.h>
#include <vmr9.h>

#include "dshow_provider.h"
#include "dshow_custom_sink.h"
#include "dshow_signal_probe.h"
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

    static int readEnvIntClamp(const char *name, int defaultValue, int minValue, int maxValue)
    {
        char buf[64] = {};
        DWORD n = GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf)));
        if (n == 0 || n >= sizeof(buf))
            return defaultValue;
        char *end = nullptr;
        long v = strtol(buf, &end, 10);
        if (end == buf)
            return defaultValue;
        if (v < minValue)
            v = minValue;
        if (v > maxValue)
            v = maxValue;
        return static_cast<int>(v);
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
        if (g == MEDIASUBTYPE_Y210)
            return "Y210";
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

    static GUID subtypeFromProfileFmt(gcap_pixfmt_t fmt)
    {
        return (fmt == GCAP_FMT_Y210) ? MEDIASUBTYPE_Y210
             : (fmt == GCAP_FMT_NV12) ? MEDIASUBTYPE_NV12
             : (fmt == GCAP_FMT_YUY2) ? MEDIASUBTYPE_YUY2
             : (fmt == GCAP_FMT_ARGB) ? MEDIASUBTYPE_RGB24
                                       : GUID{};
    }

    static int dshowQualityRank(const GUID &g)
    {
        if (g == MEDIASUBTYPE_Y210)
            return 5;
        if (g == MEDIASUBTYPE_YUY2)
            return 4;
        if (g == MEDIASUBTYPE_NV12)
            return 3;
        if (g == MEDIASUBTYPE_RGB24)
            return 2;
        if (g == MEDIASUBTYPE_RGB32 || g == MEDIASUBTYPE_ARGB32)
            return 1;
        return 0;
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


static void fill_vendor_rgb_inferred_input_signal(gcap_signal_status_t &out, bool hasCustomPage, const GUID &activeSubtype)
{
    if (!hasCustomPage)
        return;

    const bool rgbPath = (activeSubtype == MEDIASUBTYPE_RGB24) || (activeSubtype == MEDIASUBTYPE_RGB32) || (activeSubtype == MEDIASUBTYPE_ARGB32);
    if (!rgbPath)
        return;

    out.pixfmt = GCAP_FMT_ARGB;
    out.bit_depth = 8;
    out.csp = GCAP_CSP_BT709;
    out.range = GCAP_RANGE_UNKNOWN;
    out.hdr = 0;
}

static void fill_runtime_signal_text(gcap_runtime_info_t &out)
{
    out.input_signal_desc[0] = 0;
    out.input_signal_note[0] = 0;
    out.negotiated_desc[0] = 0;

    if (out.signal.width > 0 && out.signal.height > 0)
    {
        if (out.signal.pixfmt == GCAP_FMT_ARGB && out.signal.bit_depth == 8 && out.signal.csp == GCAP_CSP_BT709)
        {
            strcpy_s(out.input_signal_desc, "RGB444 / BT.709 / 8-bit");
            strcpy_s(out.input_signal_note, "Inferred");
        }
        else if (out.signal.pixfmt == GCAP_FMT_NV12 || out.signal.pixfmt == GCAP_FMT_YUY2 || out.signal.pixfmt == GCAP_FMT_ARGB || out.signal.pixfmt == GCAP_FMT_P010 || out.signal.pixfmt == GCAP_FMT_Y210 || out.signal.pixfmt == GCAP_FMT_V210 || out.signal.pixfmt == GCAP_FMT_R210)
        {
            strcpy_s(out.input_signal_desc, gcap_pixfmt_name(out.signal.pixfmt));
            strcpy_s(out.input_signal_note, "BestEffort");
        }
    }

    const char *negName = gcap_pixfmt_name(out.negotiated.pixfmt);
    if (negName && negName[0])
    {
        if (strcmp(negName, "ARGB") == 0)
            strcpy_s(out.negotiated_desc, "ARGB32");
        else
            strcpy_s(out.negotiated_desc, negName);
    }
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

void DShowProvider::setFramePacketCallback(gcap_on_frame_packet_cb pcb, void *user)
{
    std::lock_guard<std::mutex> lock(mtx_);
    pcb_ = pcb;
    user_ = user;
}

bool DShowProvider::refreshSignalProbe(bool force)
{
    if (currentIndex_ < 0)
        return false;

    const auto nowMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 std::chrono::steady_clock::now().time_since_epoch())
                                                 .count());
    // Preview-only test mode: while capture is actively running, avoid re-probing
    // the DShow signal on every runtime UI refresh. Re-probe only on force or when
    // we do not yet have a valid cached signal.
    if (!force)
    {
        if (signalValid_ && (nowMs - lastSignalProbeMs_ < 5000))
            return true;
        if (framePumpThreadRunning_ && signalValid_)
            return true;
    }

    DShowSignalProbeResult probe{};
    if (dshow_probe_current_signal_by_index(currentIndex_, probe) && probe.ok)
    {
        signalValid_ = true;
        signalW_ = probe.width;
        signalH_ = probe.height;
        signalFpsNum_ = probe.fps_num;
        signalFpsDen_ = (probe.fps_den > 0) ? probe.fps_den : 1;
        signalSubtype_ = probe.subtype;
        signalHasVendorCustomPage_ = probe.has_vendor_custom_page;
        if (probe.vendor_property_module[0])
            wcsncpy_s(signalVendorModule_, probe.vendor_property_module, _TRUNCATE);
        else
            signalVendorModule_[0] = 0;
        lastSignalProbeMs_ = nowMs;
        return true;
    }

    signalValid_ = false;
    lastSignalProbeMs_ = nowMs;
    return false;
}

bool DShowProvider::getSignalStatus(gcap_signal_status_t &out)
{
    if (!running_)
    {
        std::lock_guard<std::mutex> lock(mtx_);
        refreshSignalProbe(false);
    }

    std::lock_guard<std::mutex> lock(mtx_);
    memset(&out, 0, sizeof(out));

    // "Input Signal" shown to UI is a best-effort live status.
    // While DShow is actively running, prefer the currently negotiated/active format
    // so UI reflects what the graph is truly delivering now.
    // Keep the separate DShow GetFormat/GetStreamCaps result in runtime_info.signal_probe.
    const bool haveActive = (width_ > 0 && height_ > 0);
    const bool haveProbe = signalValid_ && signalW_ > 0 && signalH_ > 0;

    out.width = haveActive ? width_ : (haveProbe ? signalW_ : 0);
    out.height = haveActive ? height_ : (haveProbe ? signalH_ : 0);
    out.fps_num = haveActive ? (negotiatedFpsNum_ > 0 ? negotiatedFpsNum_ : profile_.fps_num)
                          : (haveProbe ? signalFpsNum_ : 0);
    out.fps_den = haveActive ? (negotiatedFpsDen_ > 0 ? negotiatedFpsDen_ : profile_.fps_den)
                          : (haveProbe ? signalFpsDen_ : 1);
    out.range = GCAP_RANGE_UNKNOWN;
    out.csp = GCAP_CSP_UNKNOWN;
    out.hdr = 0;
    out.pixfmt = gcap_subtype_to_pixfmt(haveActive ? subtype_ : (haveProbe ? signalSubtype_ : subtype_));
    out.bit_depth = gcap_pixfmt_bitdepth(out.pixfmt);
    if (out.pixfmt == GCAP_FMT_Y210)
        out.csp = GCAP_CSP_BT709;
    fill_vendor_rgb_inferred_input_signal(out, signalHasVendorCustomPage_, haveActive ? subtype_ : signalSubtype_);
    return (out.width > 0 && out.height > 0);
}

bool DShowProvider::getRuntimeInfo(gcap_runtime_info_t &out)
{
    memset(&out, 0, sizeof(out));

    if (!running_)
    {
        std::lock_guard<std::mutex> lock(mtx_);
        refreshSignalProbe(false);
    }

    {
        std::lock_guard<std::mutex> lock(mtx_);

        memset(&out.signal, 0, sizeof(out.signal));

        const bool haveActive = (width_ > 0 && height_ > 0);
        const bool haveProbe = signalValid_ && signalW_ > 0 && signalH_ > 0;

        out.signal.width = haveActive ? width_ : (haveProbe ? signalW_ : 0);
        out.signal.height = haveActive ? height_ : (haveProbe ? signalH_ : 0);
        out.signal.fps_num = haveActive ? (negotiatedFpsNum_ > 0 ? negotiatedFpsNum_ : profile_.fps_num)
                                         : (haveProbe ? signalFpsNum_ : 0);
        out.signal.fps_den = haveActive ? (negotiatedFpsDen_ > 0 ? negotiatedFpsDen_ : profile_.fps_den)
                                         : (haveProbe ? signalFpsDen_ : 1);
        out.signal.range = GCAP_RANGE_UNKNOWN;
        out.signal.csp = GCAP_CSP_UNKNOWN;
        out.signal.hdr = 0;
        out.signal.pixfmt = gcap_subtype_to_pixfmt(haveActive ? subtype_ : (haveProbe ? signalSubtype_ : subtype_));
        out.signal.bit_depth = gcap_pixfmt_bitdepth(out.signal.pixfmt);
        if (out.signal.pixfmt == GCAP_FMT_Y210)
            out.signal.csp = GCAP_CSP_BT709;
        fill_vendor_rgb_inferred_input_signal(out.signal, signalHasVendorCustomPage_, haveActive ? subtype_ : signalSubtype_);

        if (out.signal.width <= 0 || out.signal.height <= 0)
            return false;

        memset(&out.signal_probe, 0, sizeof(out.signal_probe));
        if (signalValid_ && signalW_ > 0 && signalH_ > 0)
        {
            out.signal_probe.width = signalW_;
            out.signal_probe.height = signalH_;
            out.signal_probe.fps_num = signalFpsNum_;
            out.signal_probe.fps_den = signalFpsDen_;
            out.signal_probe.pixfmt = gcap_subtype_to_pixfmt(signalSubtype_);
            out.signal_probe.bit_depth = gcap_pixfmt_bitdepth(out.signal_probe.pixfmt);
            out.signal_probe.csp = (out.signal_probe.pixfmt == GCAP_FMT_Y210) ? GCAP_CSP_BT709 : GCAP_CSP_UNKNOWN;
            out.signal_probe.range = GCAP_RANGE_UNKNOWN;
            out.signal_probe.hdr = 0;
        }

        memset(&out.negotiated, 0, sizeof(out.negotiated));
        out.negotiated.width = width_;
        out.negotiated.height = height_;
        out.negotiated.fps_num = (negotiatedFpsNum_ > 0) ? negotiatedFpsNum_ : profile_.fps_num;
        out.negotiated.fps_den = (negotiatedFpsDen_ > 0) ? negotiatedFpsDen_ : profile_.fps_den;
        out.negotiated.pixfmt = gcap_subtype_to_pixfmt(subtype_);
        out.negotiated.bit_depth = gcap_pixfmt_bitdepth(out.negotiated.pixfmt);
        out.negotiated.csp = GCAP_CSP_UNKNOWN;
        out.negotiated.range = GCAP_RANGE_UNKNOWN;
        out.negotiated.hdr = 0;
    }

    out.runtime_fps = rawRenderer_.runtimeFpsAvg();
    out.active_backend = GCAP_BACKEND_DSHOW;
    strcpy_s(out.backend_name, rawOnlyActive_ ? "DShow Raw" : "DShow");
    strcpy_s(out.path_name, rawOnlyActive_ ? "DShow Raw Preview" : "DShow VMR9 Preview");
    strcpy_s(out.frame_source, callbackSourceName(lastCallbackSource_.load()));
    if (previewHwnd_ && rawOnlyActive_)
        strcpy_s(out.frame_source, "RawSink Preview");
    else if (out.frame_source[0] == 0 || strcmp(out.frame_source, "Unknown") == 0)
        strcpy_s(out.frame_source, rawOnlyActive_ ? "RawSink" : "RendererImage");
    strcpy_s(out.source_format, gcap_subtype_name(subtype_));
    if (rawOnlyActive_)
        strcpy_s(out.render_format, pipeline_ ? (pipeline_->preview_swapchain_10bit() ? "FP16 Scene -> 10bit Swapchain" : "FP16 Scene -> 8bit Swapchain") : "FP16 Scene");
    else
        strcpy_s(out.render_format, "Renderer Native");
    fill_runtime_signal_text(out);
    return true;
}

bool DShowProvider::setPreview(const gcap_preview_desc_t &desc)
{
    previewDesc_ = desc;
    previewHwnd_ = reinterpret_cast<HWND>(desc.hwnd);
    char buf[128];
    sprintf_s(buf, "[DShow] setPreview hwnd=%p", previewHwnd_);
    OutputDebugStringA(buf);
    if (vmrWindowless_)
        updatePreviewRect();
    if (pipeline_)
    {
        pipeline_->configurePreview(desc);
        if (width_ > 0 && height_ > 0)
            pipeline_->ensure_preview_swapchain(width_, height_);
    }
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

    refreshSignalProbe(false);

    int wantW = signalValid_ && signalW_ > 0 ? signalW_ : profile_.width;
    int wantH = signalValid_ && signalH_ > 0 ? signalH_ : profile_.height;
    int wantFpsNum = signalValid_ && signalFpsNum_ > 0 ? signalFpsNum_ : profile_.fps_num;
    int wantFpsDen = signalValid_ && signalFpsDen_ > 0 ? signalFpsDen_ : (profile_.fps_den > 0 ? profile_.fps_den : 1);

    const bool profileAuto = (profile_.width <= 0 && profile_.height <= 0 && profile_.fps_num <= 0 && profile_.fps_den <= 0 && profile_.format == GCAP_FMT_NV12);
    const GUID explicitSubtype = profileAuto ? GUID{} : subtypeFromProfileFmt(profile_.format);
    GUID preferredSubtype = GUID{};

    int capCount = 0, capSize = 0;
    if (FAILED(streamConfig->GetNumberOfCapabilities(&capCount, &capSize)) || capCount <= 0 || capSize <= 0)
        return false;

    std::vector<unsigned char> caps(static_cast<size_t>(capSize));
    bool explicitSubtypeAvailable = false;
    bool y210Available = false;
    bool nv12Available = false;
    bool yuy2Available = false;
    bool rgb24Available = false;
    for (int i = 0; i < capCount; ++i)
    {
        AM_MEDIA_TYPE *scan = nullptr;
        if (FAILED(streamConfig->GetStreamCaps(i, &scan, caps.data())) || !scan)
            continue;
        if (explicitSubtype != GUID{} && scan->subtype == explicitSubtype)
            explicitSubtypeAvailable = true;
        if (scan->subtype == MEDIASUBTYPE_Y210)
            y210Available = true;
        else if (scan->subtype == MEDIASUBTYPE_NV12)
            nv12Available = true;
        else if (scan->subtype == MEDIASUBTYPE_YUY2)
            yuy2Available = true;
        else if (scan->subtype == MEDIASUBTYPE_RGB24)
            rgb24Available = true;
        freeMediaType(scan);
    }

    if (explicitSubtype != GUID{} && explicitSubtypeAvailable)
    {
        preferredSubtype = explicitSubtype;
        dshow_log("[DShow] capture format policy: explicit profile format is available; use it as first preference");
    }
    else
    {
        if (y210Available)
            preferredSubtype = MEDIASUBTYPE_Y210;
        else if (yuy2Available)
            preferredSubtype = MEDIASUBTYPE_YUY2;
        else if (nv12Available)
            preferredSubtype = MEDIASUBTYPE_NV12;
        else if (rgb24Available)
            preferredSubtype = MEDIASUBTYPE_RGB24;

        dshow_log("[DShow] capture format policy: high-quality preferred order Y210 > YUY2 > NV12 > RGB24");
    }

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
        if (preferredSubtype != GUID{})
            score += (pmt->subtype == preferredSubtype) ? 0 : 1000000000LL;

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

        const int pref = dshowQualityRank(pmt->subtype);
        score = score * 100 + (100 - pref);

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
    char buf[640] = {};
    sprintf_s(buf, "[DShow] negotiated by HQ policy -> explicit=%s available=%d preferred=%s negotiated=%s %dx%d %.2ffps",
              subtypeName(explicitSubtype), explicitSubtypeAvailable ? 1 : 0,
              subtypeName(preferredSubtype),
              subtypeName(best->subtype), w, h, (fpsNum > 0 && fpsDen > 0) ? ((double)fpsNum / (double)fpsDen) : 0.0);
    OutputDebugStringA(buf);
    freeMediaType(best);
    return true;
}

bool DShowProvider::isRawCandidate() const
{
    return (subtype_ == MEDIASUBTYPE_NV12 || subtype_ == MEDIASUBTYPE_YUY2 || subtype_ == MEDIASUBTYPE_Y210 ||
            subtype_ == MEDIASUBTYPE_RGB24 || subtype_ == MEDIASUBTYPE_RGB32 || subtype_ == MEDIASUBTYPE_ARGB32);
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

bool DShowProvider::createRenderPipeline()
{
    if (!previewHwnd_ || width_ <= 0 || height_ <= 0)
        return false;

    if (!pipeline_)
        pipeline_ = std::make_unique<SharedScenePipeline>();

    if (!d3d_)
    {
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL got{};
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, fls, _countof(fls), D3D11_SDK_VERSION, &d3d_, &got, &ctx_);
#ifdef _DEBUG
        if (FAILED(hr))
        {
            flags &= ~D3D11_CREATE_DEVICE_DEBUG;
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, fls, _countof(fls), D3D11_SDK_VERSION, &d3d_, &got, &ctx_);
        }
#endif
        if (FAILED(hr) || !d3d_ || !ctx_)
        {
            dshow_log_hr("D3D11CreateDevice(DShow pipeline)", hr);
            return false;
        }

        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_factory_.ReleaseAndGetAddressOf())) || !d2d_factory_)
            return false;

        ComPtr<IDXGIDevice> dxgiDev;
        if (FAILED(d3d_.As(&dxgiDev)) || !dxgiDev)
            return false;
        if (FAILED(d2d_factory_->CreateDevice(dxgiDev.Get(), &d2d_device_)) || !d2d_device_)
            return false;
        if (FAILED(d2d_device_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2d_ctx_)) || !d2d_ctx_)
            return false;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), &dwrite_)) || !dwrite_)
            return false;
        d2d_ctx_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &d2d_white_);
        d2d_ctx_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.55f), &d2d_black_);
    }

    if (!pipeline_->initialize(d3d_.Get(), ctx_.Get(), d2d_ctx_.Get(), dwrite_.Get(), d2d_white_.Get(), d2d_black_.Get()))
        return false;

    gcap_preview_desc_t desc = previewDesc_;
    desc.hwnd = previewHwnd_;
    desc.enable_preview = 1;
    if (desc.use_fp16_pipeline == 0)
        desc.use_fp16_pipeline = 1;
    if (desc.swapchain_10bit == 0)
        desc.swapchain_10bit = 1;

    char previewBuf[256] = {};
    sprintf_s(previewBuf, "[DShow] createRenderPipeline preview=%p use_fp16=%d swapchain_10bit=%d",
              previewHwnd_, desc.use_fp16_pipeline, desc.swapchain_10bit);
    OutputDebugStringA(previewBuf);

    pipeline_->configurePreview(desc);

    return pipeline_->ensure_rt_and_pipeline(width_, height_) &&
           pipeline_->ensure_preview_swapchain(width_, height_);
}

void DShowProvider::releaseRenderPipeline()
{
    if (pipeline_)
    {
        pipeline_->release_preview_swapchain();
        pipeline_.reset();
    }
    d2d_black_.Reset();
    d2d_white_.Reset();
    dwrite_.Reset();
    d2d_ctx_.Reset();
    d2d_device_.Reset();
    d2d_factory_.Reset();
    ctx_.Reset();
    d3d_.Reset();
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
    return true;
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
    dshow_dump_signal_diagnostics_by_index(index);
    refreshSignalProbe(true);
    updatePreviewRect();
    if (previewHwnd_)
        createRenderPipeline();
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

void DShowProvider::resetPreviewProbeStats()
{
    previewProbeStats_ = {};
}

void DShowProvider::logPreviewProbeStats(uint64_t frameId, int frameW, int frameH, bool directRaw, const char *presentTag)
{
    auto avgMs = [](uint64_t ns, uint64_t frames) -> double
    {
        return (frames > 0) ? (double)ns / (double)frames / 1000000.0 : 0.0;
    };
    auto lastMs = [](uint64_t ns) -> double
    {
        return (double)ns / 1000000.0;
    };

    const uint64_t frames = previewProbeStats_.frames;
    if (frames == 0)
        return;

    const uint64_t ensureRtCalls = pipeline_ ? pipeline_->ensure_rt_calls() : 0;
    const double ensureRtLastMs = pipeline_ ? lastMs(pipeline_->last_ensure_rt_ns()) : 0.0;
    const double ensureRtAvgMs = (pipeline_ && ensureRtCalls > 0) ? avgMs(pipeline_->total_ensure_rt_ns(), ensureRtCalls) : 0.0;
    const int ensureRtRebuild = (pipeline_ && pipeline_->last_ensure_rt_rebuilt()) ? 1 : 0;
    const char *ensureRtReason = pipeline_ ? pipeline_->last_ensure_rt_rebuild_reason() : "n/a";

    char msg[960] = {};
    sprintf_s(msg,
              "[DShow][Probe] preview-path frame=%llu size=%dx%d path=%s present=%s avg_ms{copyRaw=%.3f ensureSwap=%.3f upload=%.3f renderYuv=%.3f copyScene=%.3f blit=%.3f present=%.3f readback=%.3f} ensureRt{last=%.3f avg=%.3f rebuild=%d reason=%s calls=%llu} frames=%llu readbacks=%llu callbacks=%llu",
              static_cast<unsigned long long>(frameId),
              frameW,
              frameH,
              directRaw ? "raw-direct" : "argb-bridge",
              presentTag ? presentTag : "n/a",
              avgMs(previewProbeStats_.copyLatestRawNs, frames),
              avgMs(previewProbeStats_.ensureSwapNs, frames),
              avgMs(previewProbeStats_.uploadNs, frames),
              avgMs(previewProbeStats_.renderYuvNs, frames),
              avgMs(previewProbeStats_.copySceneNs, frames),
              avgMs(previewProbeStats_.blitNs, frames),
              avgMs(previewProbeStats_.presentNs, frames),
              avgMs(previewProbeStats_.readbackNs, previewProbeStats_.readbackFrames),
              ensureRtLastMs,
              ensureRtAvgMs,
              ensureRtRebuild,
              ensureRtReason ? ensureRtReason : "n/a",
              static_cast<unsigned long long>(ensureRtCalls),
              static_cast<unsigned long long>(frames),
              static_cast<unsigned long long>(previewProbeStats_.readbackFrames),
              static_cast<unsigned long long>(previewProbeStats_.callbackFrames));
    dshow_log(msg);
}

void DShowProvider::framePumpLoop()
{
    dshow_log("[DShow] framePumpLoop begin");
    resetPreviewProbeStats();
    uint64_t lastProcessedSampleCount = 0;
    uint64_t lastReadbackPtsNs = 0;
    const auto logPumpExit = [&](const char *reason, DWORD wr = 0)
    {
        char msg[384] = {};
        const uint64_t sc = rawRenderer_.sampleCount();
        std::snprintf(msg, sizeof(msg),
                      "[DShow] framePumpLoop exit: reason=%s running=%d preview=%p pipeline=%p rawOnly=%d sampleCount=%llu lastProcessed=%llu wait=0x%lx",
                      reason ? reason : "(null)",
                      framePumpThreadRunning_ ? 1 : 0,
                      previewHwnd_,
                      pipeline_.get(),
                      rawOnlyActive_ ? 1 : 0,
                      static_cast<unsigned long long>(sc),
                      static_cast<unsigned long long>(lastProcessedSampleCount),
                      static_cast<unsigned long>(wr));
        dshow_log(msg);
    };
    while (framePumpThreadRunning_)
    {
        HANDLE frameEvt = rawRenderer_.frameReadyEvent();
        if (frameEvt)
        {
            const DWORD wr = WaitForSingleObject(frameEvt, INFINITE);
            if (!framePumpThreadRunning_)
            {
                logPumpExit("running-cleared-after-wait", wr);
                break;
            }
            if (wr != WAIT_OBJECT_0 && wr != WAIT_TIMEOUT)
            {
                char msg[192] = {};
                std::snprintf(msg, sizeof(msg),
                              "[DShow] framePumpLoop wait abnormal wr=0x%lx gle=%lu, fallback continue",
                              static_cast<unsigned long>(wr),
                              static_cast<unsigned long>(GetLastError()));
                dshow_log(msg);
            }
        }
        else
        {
            static bool s_loggedNoFrameEvent = false;
            if (!s_loggedNoFrameEvent)
            {
                dshow_log("[DShow] framePumpLoop no frame event, using sleep fallback");
                s_loggedNoFrameEvent = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(framePumpSleepMs()));
            if (!framePumpThreadRunning_)
            {
                logPumpExit("running-cleared-after-sleep");
                break;
            }
        }

        gcap_on_video_cb vcb = nullptr;
        gcap_on_frame_packet_cb pcb = nullptr;
        void *user = nullptr;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            vcb = vcb_;
            pcb = pcb_;
            user = user_;
        }

        const uint64_t curSampleCount = rawRenderer_.sampleCount();
        if (rawOnlyActive_ && curSampleCount != 0 && curSampleCount == lastProcessedSampleCount)
            continue;
        std::vector<uint8_t> raw;
        int rw = 0, rh = 0, rstride = 0;
        GUID rawSubtype = MEDIASUBTYPE_NULL;
        const auto tCopyRaw0 = std::chrono::steady_clock::now();
        const bool haveRaw = rawRenderer_.copyLatestRaw(raw, rw, rh, rstride, rawSubtype) && !raw.empty();
        const auto tCopyRaw1 = std::chrono::steady_clock::now();

        std::vector<uint8_t> buf;
        int w = 0, h = 0, stride = 0;
        const bool previewOnlyActive = (previewHwnd_ != nullptr);
        const bool canUseSharedRaw = pipeline_ && haveRaw && rawOnlyActive_ &&
                                     (rawSubtype == MEDIASUBTYPE_NV12 || rawSubtype == MEDIASUBTYPE_YUY2 || rawSubtype == MEDIASUBTYPE_Y210);
        // DS preview + low-frequency ARGB callback coexist mode:
        //   - preview path still owns render/present timing when a preview window is active
        //   - frame-packet callback can still run from raw data
        //   - ARGB video callback is allowed during preview, but only at a low frequency
        //     so it does not drag preview smoothness back down.
        const bool allowVideoCallbackPath = (vcb != nullptr);
        const bool needArgb = !canUseSharedRaw && (allowVideoCallbackPath || previewOnlyActive || rawSubtype == MEDIASUBTYPE_RGB24 || rawSubtype == MEDIASUBTYPE_RGB32 || rawSubtype == MEDIASUBTYPE_ARGB32);
        const bool haveArgb = needArgb ? captureRawFrameToArgb(buf, w, h, stride) : false;

        if (haveRaw || haveArgb)
        {
            if (haveRaw && curSampleCount != 0)
                lastProcessedSampleCount = curSampleCount;
            auto now = std::chrono::steady_clock::now().time_since_epoch();
            const uint64_t ptsNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
            const uint64_t frameId = ++frameCounter_;

            if (pcb && haveRaw && rawOnlyActive_)
            {
                gcap_frame_packet_t pkt{};
                pkt.width = rw;
                pkt.height = rh;
                pkt.pts_ns = ptsNs;
                pkt.frame_id = frameId;
                pkt.backend = GCAP_BACKEND_DSHOW;
                pkt.source_kind = GCAP_SOURCE_DSHOW_RAWSINK;
                pkt.gpu_backed = 0;

                if (rawSubtype == MEDIASUBTYPE_NV12)
                {
                    pkt.format = GCAP_FMT_NV12;
                    pkt.plane_count = 2;
                    pkt.data[0] = raw.data();
                    pkt.data[1] = raw.data() + (size_t)rstride * (size_t)rh;
                    pkt.stride[0] = rstride;
                    pkt.stride[1] = rstride;
                }
                else if (rawSubtype == MEDIASUBTYPE_YUY2)
                {
                    pkt.format = GCAP_FMT_YUY2;
                    pkt.plane_count = 1;
                    pkt.data[0] = raw.data();
                    pkt.stride[0] = rstride;
                }
                else if (rawSubtype == MEDIASUBTYPE_Y210)
                {
                    pkt.format = GCAP_FMT_Y210;
                    pkt.plane_count = 1;
                    pkt.data[0] = raw.data();
                    pkt.stride[0] = rstride;
                }
                else
                {
                    pkt.format = GCAP_FMT_ARGB;
                    pkt.plane_count = 1;
                    pkt.data[0] = haveArgb ? buf.data() : nullptr;
                    pkt.stride[0] = stride;
                }
                if (frameId <= 5 || (frameId % 60) == 0)
                {
                    char dbg[256];
                    std::snprintf(dbg, sizeof(dbg),
                                  "[DShow] dispatch frame packet frame=%llu fmt=%d %dx%d planes=%d user=%p\n",
                                  static_cast<unsigned long long>(frameId),
                                  pkt.format,
                                  pkt.width,
                                  pkt.height,
                                  pkt.plane_count,
                                  user);
                    dshow_log(dbg);
                }
                pcb(&pkt, user);
            }

            bool sharedReady = false;
            int sharedW = 0, sharedH = 0;
            uint64_t probeEnsureRtNs = 0;
            uint64_t probeEnsureSwapNs = 0;
            uint64_t probeUploadNs = 0;
            uint64_t probeRenderYuvNs = 0;
            uint64_t probeCopySceneNs = 0;
            uint64_t probeBlitNs = 0;
            uint64_t probePresentNs = 0;
            const char *probePresentTag = "skip";
            if (pipeline_ && canUseSharedRaw)
            {
                sharedW = rw;
                sharedH = rh;
                bool ensuredRt = false;
                bool ensuredSwap = false;
                {
                    const auto t0 = std::chrono::steady_clock::now();
                    ensuredRt = pipeline_->ensure_rt_and_pipeline(rw, rh);
                    const auto t1 = std::chrono::steady_clock::now();
                    probeEnsureRtNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                }
                if (pipeline_ && pipeline_->last_ensure_rt_rebuilt() && (frameId <= 5 || (frameId % 120) == 0))
                {
                    char rtmsg[320] = {};
                    sprintf_s(rtmsg,
                              "[DShow][Probe] ensureRt rebuild frame=%llu size=%dx%d reason=%s last_ms=%.3f",
                              static_cast<unsigned long long>(frameId),
                              rw,
                              rh,
                              pipeline_->last_ensure_rt_rebuild_reason(),
                              (double)pipeline_->last_ensure_rt_ns() / 1000000.0);
                    dshow_log(rtmsg);
                }
                if (ensuredRt)
                {
                    const auto t0 = std::chrono::steady_clock::now();
                    ensuredSwap = pipeline_->ensure_preview_swapchain(rw, rh);
                    const auto t1 = std::chrono::steady_clock::now();
                    probeEnsureSwapNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                }
                if (ensuredRt && ensuredSwap)
                {
                    bool uploaded = false;
                    if (rawSubtype == MEDIASUBTYPE_NV12)
                    {
                        const uint8_t *y = raw.data();
                        const uint8_t *uv = raw.data() + (size_t)rstride * (size_t)rh;
                        {
                            const auto t0 = std::chrono::steady_clock::now();
                            uploaded = pipeline_->upload_nv12_frame(y, rstride, uv, rstride, rw, rh);
                            const auto t1 = std::chrono::steady_clock::now();
                            probeUploadNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                        }
                        if (uploaded)
                        {
                            const auto t0 = std::chrono::steady_clock::now();
                            uploaded = pipeline_->render_uploaded_yuv_to_fp16(GCAP_FMT_NV12, rw, rh);
                            const auto t1 = std::chrono::steady_clock::now();
                            probeRenderYuvNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                        }
                        if (uploaded)
                        {
                            const auto t0 = std::chrono::steady_clock::now();
                            uploaded = pipeline_->copy_fp16_to_scene();
                            const auto t1 = std::chrono::steady_clock::now();
                            probeCopySceneNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                        }
                    }
                    else if (rawSubtype == MEDIASUBTYPE_YUY2)
                    {
                        {
                            const auto t0 = std::chrono::steady_clock::now();
                            uploaded = pipeline_->upload_yuy2_frame(raw.data(), rstride, rw, rh);
                            const auto t1 = std::chrono::steady_clock::now();
                            probeUploadNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                        }
                        if (uploaded)
                        {
                            const auto t0 = std::chrono::steady_clock::now();
                            uploaded = pipeline_->render_uploaded_yuv_to_fp16(GCAP_FMT_YUY2, rw, rh);
                            const auto t1 = std::chrono::steady_clock::now();
                            probeRenderYuvNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                        }
                        if (uploaded)
                        {
                            const auto t0 = std::chrono::steady_clock::now();
                            uploaded = pipeline_->copy_fp16_to_scene();
                            const auto t1 = std::chrono::steady_clock::now();
                            probeCopySceneNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                        }
                    }
                    else if (rawSubtype == MEDIASUBTYPE_Y210)
                    {
                        {
                            const auto t0 = std::chrono::steady_clock::now();
                            uploaded = pipeline_->upload_y210_frame(raw.data(), rstride, rw, rh);
                            const auto t1 = std::chrono::steady_clock::now();
                            probeUploadNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                        }
                        if (uploaded)
                        {
                            const auto t0 = std::chrono::steady_clock::now();
                            uploaded = pipeline_->render_uploaded_yuv_to_fp16(GCAP_FMT_Y210, rw, rh);
                            const auto t1 = std::chrono::steady_clock::now();
                            probeRenderYuvNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                        }
                        if (uploaded)
                        {
                            const auto t0 = std::chrono::steady_clock::now();
                            uploaded = pipeline_->copy_fp16_to_scene();
                            const auto t1 = std::chrono::steady_clock::now();
                            probeCopySceneNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                        }
                    }
                    if (uploaded)
                    {
                        const bool needPreviewBlit = !pipeline_->preview_swapchain_10bit();
                        if (needPreviewBlit)
                        {
                            const auto t0 = std::chrono::steady_clock::now();
                            uploaded = pipeline_->blit_fp16_to_rgba8(rw, rh);
                            const auto t1 = std::chrono::steady_clock::now();
                            probeBlitNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                        }
                    }
                    if (uploaded)
                    {
                        const auto t0 = std::chrono::steady_clock::now();
                        pipeline_->present_preview(rw, rh);
                        const auto t1 = std::chrono::steady_clock::now();
                        probePresentNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                        probePresentTag = pipeline_->preview_swapchain_10bit() ? "preview-direct-10bit" : "preview-direct-rgba8";
                        sharedReady = true;
                    }
                }
            }
            else if (pipeline_ && haveArgb)
            {
                sharedW = w;
                sharedH = h;
                bool ensuredRt = false;
                bool ensuredSwap = false;
                {
                    const auto t0 = std::chrono::steady_clock::now();
                    ensuredRt = pipeline_->ensure_rt_and_pipeline(w, h);
                    const auto t1 = std::chrono::steady_clock::now();
                    probeEnsureRtNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                }
                if (pipeline_ && pipeline_->last_ensure_rt_rebuilt() && (frameId <= 5 || (frameId % 120) == 0))
                {
                    char rtmsg[320] = {};
                    sprintf_s(rtmsg,
                              "[DShow][Probe] ensureRt rebuild frame=%llu size=%dx%d reason=%s last_ms=%.3f",
                              static_cast<unsigned long long>(frameId),
                              w,
                              h,
                              pipeline_->last_ensure_rt_rebuild_reason(),
                              (double)pipeline_->last_ensure_rt_ns() / 1000000.0);
                    dshow_log(rtmsg);
                }
                if (ensuredRt)
                {
                    const auto t0 = std::chrono::steady_clock::now();
                    ensuredSwap = pipeline_->ensure_preview_swapchain(w, h);
                    const auto t1 = std::chrono::steady_clock::now();
                    probeEnsureSwapNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                }
                bool uploaded = false;
                if (ensuredRt && ensuredSwap)
                {
                    const auto t0 = std::chrono::steady_clock::now();
                    uploaded = pipeline_->upload_argb_frame(buf.data(), w, h, stride);
                    const auto t1 = std::chrono::steady_clock::now();
                    probeUploadNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                }
                if (uploaded)
                {
                    const auto t0 = std::chrono::steady_clock::now();
                    pipeline_->present_preview(w, h);
                    const auto t1 = std::chrono::steady_clock::now();
                    probePresentNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                    probePresentTag = "preview-argb";
                    sharedReady = true;
                }
            }

            if (sharedReady)
            {
                ++previewProbeStats_.frames;
                previewProbeStats_.copyLatestRawNs += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(tCopyRaw1 - tCopyRaw0).count();
                previewProbeStats_.ensureRtNs += probeEnsureRtNs;
                previewProbeStats_.ensureSwapNs += probeEnsureSwapNs;
                previewProbeStats_.uploadNs += probeUploadNs;
                previewProbeStats_.renderYuvNs += probeRenderYuvNs;
                previewProbeStats_.copySceneNs += probeCopySceneNs;
                previewProbeStats_.blitNs += probeBlitNs;
                previewProbeStats_.presentNs += probePresentNs;
                if (frameId <= 3 || (frameId % 120) == 0)
                    logPreviewProbeStats(frameId, sharedW, sharedH, canUseSharedRaw, probePresentTag);
            }

            if (vcb)
            {
                const bool wantVideoCallback = shouldDoSharedReadback(ptsNs, frameId, true, previewOnlyActive, vcb != nullptr, lastReadbackPtsNs);
                if (previewOnlyActive && frameId == 1)
                {
                    char msg[256] = {};
                    double fps = (negotiatedFpsNum_ > 0 && negotiatedFpsDen_ > 0) ? ((double)negotiatedFpsNum_ / (double)negotiatedFpsDen_) : 0.0;
                    sprintf_s(msg,
                              "[DShow] preview split active: ARGB video callback low-frequency mode while preview is active (%s %dx%d %.2ffps callback~%dfps)",
                              subtypeName(subtype_), width_, height_, fps, callbackTargetFps());
                    dshow_log(msg);
                }

                if (pipeline_ && sharedReady && wantVideoCallback)
                {
                    gcap_frame_t f{};
                    {
                        const auto t0 = std::chrono::steady_clock::now();
                        const bool readbackOk = pipeline_->readback_to_frame(sharedW, sharedH, ptsNs, frameId, &f);
                        const auto t1 = std::chrono::steady_clock::now();
                        previewProbeStats_.readbackNs += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                        ++previewProbeStats_.readbackFrames;
                        if (readbackOk)
                        {
                            if (frameId == 1 || (previewOnlyActive && previewProbeStats_.callbackFrames == 0))
                            {
                                char msg[320] = {};
                                double fps = (negotiatedFpsNum_ > 0 && negotiatedFpsDen_ > 0) ? ((double)negotiatedFpsNum_ / (double)negotiatedFpsDen_) : 0.0;
                                sprintf_s(msg, "[DShow] shared-scene frame -> %d x %d fmt=ARGB negotiated=%s %d x %d %.2ffps source=%s raw-candidate=%s raw-sink-plan=%s ingest=%s",
                                          sharedW, sharedH, subtypeName(subtype_), width_, height_, fps,
                                          callbackSourceName(lastCallbackSource_.load()),
                                          isRawCandidate() ? "YES" : "NO",
                                          rawSinkPlanned() ? "CUSTOM_V4_RAW_PREVIEW" : "NO",
                                          canUseSharedRaw ? (rawSubtype == MEDIASUBTYPE_NV12 ? "NV12-direct" : (rawSubtype == MEDIASUBTYPE_Y210 ? "Y210-direct" : "YUY2-direct")) : ((rawSubtype == MEDIASUBTYPE_RGB24 || rawSubtype == MEDIASUBTYPE_RGB32 || rawSubtype == MEDIASUBTYPE_ARGB32) ? "RGB-bridge" : "ARGB-bridge"));
                                OutputDebugStringA(msg);
                            }
                            vcb(&f, user);
                            ++previewProbeStats_.callbackFrames;
                            ctx_->Unmap(pipeline_->rt_stage_.Get(), 0);
                        }
                    }
                }
                else if (haveArgb && wantVideoCallback)
                {
                    gcap_frame_t f{};
                    f.data[0] = buf.data();
                    f.stride[0] = stride;
                    f.plane_count = 1;
                    f.width = w;
                    f.height = h;
                    f.format = GCAP_FMT_ARGB;
                    f.pts_ns = ptsNs;
                    f.frame_id = frameId;
                    if (f.frame_id == 1 || (previewOnlyActive && previewProbeStats_.callbackFrames == 0))
                    {
                        char msg[256] = {};
                        double fps = (negotiatedFpsNum_ > 0 && negotiatedFpsDen_ > 0) ? ((double)negotiatedFpsNum_ / (double)negotiatedFpsDen_) : 0.0;
                        sprintf_s(msg, "[DShow] callback frame -> %d x %d fmt=ARGB negotiated=%s %d x %d %.2ffps source=%s raw-candidate=%s raw-sink-plan=%s",
                                  w, h, subtypeName(subtype_), width_, height_, fps,
                                  callbackSourceName(lastCallbackSource_.load()),
                                  isRawCandidate() ? "YES" : "NO",
                                  rawSinkPlanned() ? "CUSTOM_V4_RAW_PREVIEW" : "NO");
                        OutputDebugStringA(msg);
                    }
                    vcb(&f, user);
                    ++previewProbeStats_.callbackFrames;
                }
            }
        }
    }
    if (framePumpThreadRunning_)
        logPumpExit("while-condition-ended-unexpectedly");
    else
        logPumpExit("while-condition-running-false");
    dshow_log("[DShow] framePumpLoop end");
}

bool DShowProvider::shouldDoSharedReadback(uint64_t ptsNs, uint64_t frameId, bool sharedReady, bool havePreview, bool haveCallback, uint64_t &lastReadbackPtsNs) const
{
    UNREFERENCED_PARAMETER(frameId);

    if (!sharedReady || !haveCallback)
        return false;

    // No preview window: keep the callback/readback path fully enabled.
    if (!havePreview)
        return true;

    // Preview window active: allow a low-frequency ARGB callback to coexist with smooth preview.
    const int targetFps = callbackTargetFps();
    if (targetFps <= 0)
        return false;

    if (lastReadbackPtsNs == 0)
    {
        lastReadbackPtsNs = ptsNs;
        return true;
    }

    const uint64_t minIntervalNs = static_cast<uint64_t>(1000000000.0 / static_cast<double>(targetFps));
    if (ptsNs > lastReadbackPtsNs && (ptsNs - lastReadbackPtsNs) >= minIntervalNs)
    {
        lastReadbackPtsNs = ptsNs;
        return true;
    }

    return false;
}

int DShowProvider::callbackTargetFps() const
{
    return callbackTargetFps_;
}

int DShowProvider::framePumpSleepMs() const
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

void DShowProvider::startFramePumpThread()
{
    stopFramePumpThread();
    framePumpThreadRunning_ = true;
    framePumpThread_ = std::thread([this]
                                   { framePumpLoop(); });
}

void DShowProvider::startSignalProbeThread()
{
    stopSignalProbeThread();
    signalProbeThreadRunning_ = true;
    signalProbeThread_ = std::thread([this]
                                     { signalProbeLoop(); });
}

void DShowProvider::stopSignalProbeThread()
{
    signalProbeThreadRunning_ = false;
    if (signalProbeThread_.joinable())
        signalProbeThread_.join();
}

void DShowProvider::signalProbeLoop()
{
    while (signalProbeThreadRunning_)
    {
        for (int i = 0; i < 30 && signalProbeThreadRunning_; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

        if (!signalProbeThreadRunning_ || !running_)
            continue;

        int index = -1;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            index = currentIndex_;
        }
        if (index < 0)
            continue;

        DShowSignalProbeResult probe{};
        const bool ok = dshow_probe_current_signal_by_index(index, probe) && probe.ok;
        const auto nowMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                     std::chrono::steady_clock::now().time_since_epoch())
                                                     .count());

        std::lock_guard<std::mutex> lock(mtx_);
        if (ok)
        {
            signalValid_ = true;
            signalW_ = probe.width;
            signalH_ = probe.height;
            signalFpsNum_ = probe.fps_num;
            signalFpsDen_ = (probe.fps_den > 0) ? probe.fps_den : 1;
            signalSubtype_ = probe.subtype;
            signalHasVendorCustomPage_ = probe.has_vendor_custom_page;
            if (probe.vendor_property_module[0])
                wcsncpy_s(signalVendorModule_, probe.vendor_property_module, _TRUNCATE);
            else
                signalVendorModule_[0] = 0;
        }
        else
        {
            signalValid_ = false;
        }
        lastSignalProbeMs_ = nowMs;
    }
}

void DShowProvider::stopFramePumpThread()
{
    char msg[256] = {};
    std::snprintf(msg, sizeof(msg),
                  "[DShow] stopFramePumpThread: joinable=%d running=%d sampleCount=%llu",
                  framePumpThread_.joinable() ? 1 : 0,
                  framePumpThreadRunning_ ? 1 : 0,
                  static_cast<unsigned long long>(rawRenderer_.sampleCount()));
    dshow_log(msg);
    framePumpThreadRunning_ = false;
    HANDLE frameEvt = rawRenderer_.frameReadyEvent();
    if (frameEvt)
        SetEvent(frameEvt);
    if (framePumpThread_.joinable())
        framePumpThread_.join();
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
    {
        char msg[192] = {};
        sprintf_s(msg, "[DShow] callback mode=preview-split low-frequency, target_fps=%d (preview on) / per-frame (preview off)", callbackTargetFps());
        dshow_log(msg);
    }
    if (vmrWindowless_ && previewHwnd_)
    {
        updatePreviewRect();
        vmrWindowless_->RepaintVideo(previewHwnd_, nullptr);
    }
    refreshSignalProbe(true);
    running_ = true;
    startSignalProbeThread();
    if (vcb_ || pcb_ || previewHwnd_)
        startFramePumpThread();
    return true;
}

void DShowProvider::stop()
{
    stopSignalProbeThread();
    stopFramePumpThread();
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
    signalValid_ = false;
    signalW_ = signalH_ = 0;
    signalFpsNum_ = 0;
    signalFpsDen_ = 1;
    signalSubtype_ = GUID_NULL;
    lastSignalProbeMs_ = 0;
    signalHasVendorCustomPage_ = false;
    signalVendorModule_[0] = 0;
}
