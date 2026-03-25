
#include <windows.h>
#include <dshow.h>
#include <d3d9.h>
#include <vmr9.h>

#include "dshow_provider.h"
#include <objbase.h>
#include <strsafe.h>
#include <initguid.h>
#include <dvdmedia.h>
#include <stdio.h>

using Microsoft::WRL::ComPtr;

namespace
{
    std::once_flag g_comOnce;
    HWND g_previewHwnd = nullptr;
    long g_previewW = 0;
    long g_previewH = 0;

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
        sprintf_s(buf, "[DShow] %s hr=0x%08X (%s)\n", prefix, static_cast<unsigned>(hr), sys[0] ? sys : "n/a");
        OutputDebugStringA(buf);
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
    out.fps_num = profile_.fps_num;
    out.fps_den = profile_.fps_den;
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

bool DShowProvider::setPreview(const gcap_preview_desc_t &desc)
{
    g_previewHwnd = reinterpret_cast<HWND>(desc.hwnd);
    char buf[128];
    sprintf_s(buf, "[DShow] setPreview hwnd=%p\n", g_previewHwnd);
    OutputDebugStringA(buf);
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
                di.symbolic_link[0] = '\0';
                list.push_back(di);

                char buf[256] = {};
                sprintf_s(buf, "[DShow] enumerate device[%d] = %s\n", index, nameBuf);
                OutputDebugStringA(buf);
                ++index;
            }
            VariantClear(&varName);
        }
        moniker.Reset();
    }

    return !list.empty();
}

bool DShowProvider::buildGraphForDevice(int index)
{
    dshow_log("[DShow] === buildGraphForDevice begin (VMR9 preview path) ===\n");

    graph_.Reset();
    mediaControl_.Reset();
    mediaEvent_.Reset();
    sourceFilter_.Reset();
    grabberFilter_.Reset();
    nullRenderer_.Reset();
    width_ = 0;
    height_ = 0;
    subtype_ = MEDIASUBTYPE_NULL;

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
    dshow_log("[DShow] OK: CreateClassEnumerator(CLSID_VideoInputDeviceCategory)\n");

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
        dshow_log("[DShow] device moniker not found\n");
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
                sprintf_s(buf, "[DShow] selected device[%d] = %s\n", index, nameBuf);
                OutputDebugStringA(buf);
            }
            VariantClear(&varName);
        }
    }

    HR_CHECK_LOG(moniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&sourceFilter_)));
    HR_CHECK_LOG(graph_->AddFilter(sourceFilter_.Get(), L"VideoCapture"));

    // Use nullRenderer_ member to hold VMR9 filter to avoid changing header.
    HR_CHECK_LOG(CoCreateInstance(CLSID_VideoMixingRenderer9, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&nullRenderer_)));
    HR_CHECK_LOG(graph_->AddFilter(nullRenderer_.Get(), L"VMR9"));

    ComPtr<IVMRFilterConfig9> vmrConfig;
    HR_CHECK_LOG(nullRenderer_.As(&vmrConfig));
    HR_CHECK_LOG(vmrConfig->SetNumberOfStreams(1));
    HR_CHECK_LOG(vmrConfig->SetRenderingMode(VMR9Mode_Windowless));

    // ComPtr<IVMRWindowlessControl9> vmrWindowless;
    HR_CHECK_LOG(nullRenderer_.As(&vmrWindowless));
    if (!g_previewHwnd)
    {
        dshow_log("[DShow] preview hwnd is null\n");
        return false;
    }
    HR_CHECK_LOG(vmrWindowless->SetVideoClippingWindow(g_previewHwnd));
    HR_CHECK_LOG(vmrWindowless->SetAspectRatioMode(VMR9ARMode_LetterBox));
    RECT rc{};
    GetClientRect(g_previewHwnd, &rc);

    {
        char buf[256];
        sprintf_s(buf, "[DShow] client rect = (%ld,%ld)-(%ld,%ld)\n",
                  rc.left, rc.top, rc.right, rc.bottom);
        OutputDebugStringA(buf);
    }

    HR_CHECK_LOG(vmrWindowless->SetVideoPosition(nullptr, &rc));
    ComPtr<ICaptureGraphBuilder2> capBuilder;
    HR_CHECK_LOG(CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&capBuilder)));
    HR_CHECK_LOG(capBuilder->SetFiltergraph(graph_.Get()));
    HR_CHECK_LOG(capBuilder->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video,
                                          sourceFilter_.Get(), nullptr, nullRenderer_.Get()));

    ComPtr<IAMStreamConfig> streamConfig;
    hr = capBuilder->FindInterface(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video,
                                   sourceFilter_.Get(), IID_PPV_ARGS(&streamConfig));
    if (SUCCEEDED(hr) && streamConfig)
    {
        AM_MEDIA_TYPE *pmt = nullptr;
        hr = streamConfig->GetFormat(&pmt);
        if (SUCCEEDED(hr) && pmt)
        {
            dshow_log("[DShow] OK: IAMStreamConfig::GetFormat()\n");
            if ((pmt->formattype == FORMAT_VideoInfo || pmt->formattype == FORMAT_VideoInfo2) &&
                pmt->cbFormat >= sizeof(VIDEOINFOHEADER) && pmt->pbFormat)
            {
                VIDEOINFOHEADER *vih = reinterpret_cast<VIDEOINFOHEADER *>(pmt->pbFormat);
                width_ = vih->bmiHeader.biWidth;
                height_ = abs(vih->bmiHeader.biHeight);
                subtype_ = pmt->subtype;

                char buf[256] = {};
                sprintf_s(buf, "[DShow] stream format: width=%d height=%d subtype={%08lX-0000-0010-...}\n",
                          width_, height_, subtype_.Data1);
                OutputDebugStringA(buf);
            }
            if (pmt->cbFormat && pmt->pbFormat)
                CoTaskMemFree(pmt->pbFormat);
            if (pmt->pUnk)
                pmt->pUnk->Release();
            CoTaskMemFree(pmt);
        }
    }

    if (g_previewW > 0 && g_previewH > 0)
    {
        RECT rc{0, 0, g_previewW, g_previewH};
        HRESULT rhr = vmrWindowless->SetVideoPosition(nullptr, &rc);
        dshow_log_hr("IVMRWindowlessControl9::SetVideoPosition", rhr);
    }

    dshow_log("[DShow] === buildGraphForDevice success (VMR9 preview path) ===\n");
    return true;
}

bool DShowProvider::open(int index)
{
    char buf[128];
    sprintf_s(buf, "[DShow] open() current preview_hwnd_=%p\n", g_previewHwnd);
    OutputDebugStringA(buf);
    ensure_com();
    dshow_log("[DShow] open() begin\n");
    close();
    std::lock_guard<std::mutex> lock(mtx_);

    if (!buildGraphForDevice(index))
    {
        dshow_log("[DShow] open() failed\n");
        return false;
    }

    currentIndex_ = index;
    dshow_log("[DShow] open() success\n");
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

    dshow_log("[DShow] start() OK: mediaControl_->Run()\n");
    if (vmrWindowless && g_previewHwnd)
    {
        RECT rc{};
        GetClientRect(g_previewHwnd, &rc);
        char buf[256];
        sprintf_s(buf, "[DShow] start() rect = (%ld,%ld)-(%ld,%ld)\n",
                  rc.left, rc.top, rc.right, rc.bottom);
        OutputDebugStringA(buf);
        vmrWindowless->SetVideoPosition(nullptr, &rc);
        vmrWindowless->RepaintVideo(g_previewHwnd, nullptr);
    }
    running_ = true;
    return true;
}

void DShowProvider::stop()
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (mediaControl_ && running_)
    {
        mediaControl_->Stop();
        dshow_log("[DShow] stop()\n");
        running_ = false;
    }
}

void DShowProvider::close()
{
    dshow_log("[DShow] close()\n");
    stop();

    std::lock_guard<std::mutex> lock(mtx_);
    nullRenderer_.Reset();
    grabberFilter_.Reset();
    sourceFilter_.Reset();
    mediaEvent_.Reset();
    mediaControl_.Reset();
    graph_.Reset();
    width_ = 0;
    height_ = 0;
    subtype_ = MEDIASUBTYPE_NULL;
    currentIndex_ = -1;
}

void DShowProvider::onSample(double, BYTE *, long)
{
    // This preview test build does not use callbacks.
}

SampleGrabberCBImpl::SampleGrabberCBImpl(DShowProvider *owner)
    : owner_(owner)
{
}

STDMETHODIMP SampleGrabberCBImpl::QueryInterface(REFIID riid, void **ppv)
{
    if (!ppv)
        return E_POINTER;
    if (riid == IID_IUnknown)
    {
        *ppv = static_cast<IUnknown *>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG)
SampleGrabberCBImpl::AddRef()
{
    return ++refCount_;
}

STDMETHODIMP_(ULONG)
SampleGrabberCBImpl::Release()
{
    ULONG r = --refCount_;
    if (r == 0)
        delete this;
    return r;
}

STDMETHODIMP SampleGrabberCBImpl::BufferCB(double sampleTime, BYTE *buffer, long len)
{
    if (owner_)
        owner_->onSample(sampleTime, buffer, len);
    return S_OK;
}
