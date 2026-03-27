#include "dshow_signal_probe.h"

#include <objbase.h>
#include <dvdmedia.h>
#include <wrl/client.h>
#include <vector>
#include <cmath>

using Microsoft::WRL::ComPtr;

static bool dshow_extract_media_type(const AM_MEDIA_TYPE &mt, DShowSignalProbeResult &out)
{
    if (!mt.pbFormat)
        return false;

    LONG width = 0;
    LONG height = 0;
    REFERENCE_TIME avg = 0;

    if ((mt.formattype == FORMAT_VideoInfo || mt.formattype == FORMAT_VideoInfo2) &&
        mt.cbFormat >= sizeof(VIDEOINFOHEADER) && mt.pbFormat)
    {
        if (mt.formattype == FORMAT_VideoInfo2 && mt.cbFormat >= sizeof(VIDEOINFOHEADER2))
        {
            auto *vih2 = reinterpret_cast<const VIDEOINFOHEADER2 *>(mt.pbFormat);
            width = vih2->bmiHeader.biWidth;
            height = vih2->bmiHeader.biHeight;
            avg = vih2->AvgTimePerFrame;
        }
        else
        {
            auto *vih = reinterpret_cast<const VIDEOINFOHEADER *>(mt.pbFormat);
            width = vih->bmiHeader.biWidth;
            height = vih->bmiHeader.biHeight;
            avg = vih->AvgTimePerFrame;
        }
    }

    out.width = (int)width;
    out.height = (int)std::abs(height);
    out.subtype = mt.subtype;
    out.fps_den = 1;
    out.fps_num = (avg > 0) ? (int)((10000000LL + avg / 2) / avg) : 0;
    out.ok = (out.width > 0 && out.height > 0);
    return out.ok;
}

static void dshow_free_media_type(AM_MEDIA_TYPE &mt)
{
    if (mt.cbFormat != 0 && mt.pbFormat)
    {
        CoTaskMemFree(mt.pbFormat);
        mt.cbFormat = 0;
        mt.pbFormat = nullptr;
    }
    if (mt.pUnk)
    {
        mt.pUnk->Release();
        mt.pUnk = nullptr;
    }
}

bool dshow_probe_current_signal_by_index(int devIndex, DShowSignalProbeResult &out)
{
    out = {};

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool need_uninit = SUCCEEDED(hr);
    if (hr != S_OK && hr != S_FALSE && hr != RPC_E_CHANGED_MODE)
        return false;

    bool ok = false;
    do
    {
        ComPtr<ICreateDevEnum> devEnum;
        hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&devEnum));
        if (FAILED(hr))
            break;

        ComPtr<IEnumMoniker> enumMoniker;
        hr = devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &enumMoniker, 0);
        if (hr != S_OK)
            break;

        ComPtr<IMoniker> moniker;
        ULONG fetched = 0;
        int cur = 0;
        while (enumMoniker->Next(1, &moniker, &fetched) == S_OK)
        {
            if (cur == devIndex)
                break;
            moniker.Reset();
            ++cur;
        }
        if (!moniker)
            break;

        ComPtr<IBaseFilter> sourceFilter;
        hr = moniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&sourceFilter));
        if (FAILED(hr))
            break;

        ComPtr<IGraphBuilder> graph;
        hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&graph));
        if (FAILED(hr))
            break;

        hr = graph->AddFilter(sourceFilter.Get(), L"VideoCapture");
        if (FAILED(hr))
            break;

        ComPtr<ICaptureGraphBuilder2> capBuilder;
        hr = CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&capBuilder));
        if (FAILED(hr))
            break;
        capBuilder->SetFiltergraph(graph.Get());

        ComPtr<IAMStreamConfig> cfg;
        hr = capBuilder->FindInterface(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, sourceFilter.Get(), IID_PPV_ARGS(&cfg));
        if (FAILED(hr) || !cfg)
            break;

        AM_MEDIA_TYPE *pmt = nullptr;
        hr = cfg->GetFormat(&pmt);
        if (SUCCEEDED(hr) && pmt)
        {
            ok = dshow_extract_media_type(*pmt, out);
            dshow_free_media_type(*pmt);
            CoTaskMemFree(pmt);
            if (ok)
                break;
        }

        int count = 0, capSize = 0;
        hr = cfg->GetNumberOfCapabilities(&count, &capSize);
        if (FAILED(hr) || count <= 0 || capSize <= 0)
            break;

        std::vector<unsigned char> caps((size_t)capSize);
        for (int i = 0; i < count; ++i)
        {
            AM_MEDIA_TYPE *capsMt = nullptr;
            if (FAILED(cfg->GetStreamCaps(i, &capsMt, caps.data())) || !capsMt)
                continue;

            if (dshow_extract_media_type(*capsMt, out))
            {
                dshow_free_media_type(*capsMt);
                CoTaskMemFree(capsMt);
                ok = true;
                break;
            }

            dshow_free_media_type(*capsMt);
            CoTaskMemFree(capsMt);
        }
    } while (false);

    if (need_uninit)
        CoUninitialize();
    return ok;
}

gcap_pixfmt_t gcap_subtype_to_pixfmt(const GUID &sub)
{
    if (sub == MEDIASUBTYPE_NV12) return GCAP_FMT_NV12;
    if (sub == MEDIASUBTYPE_YUY2) return GCAP_FMT_YUY2;
    if (sub == MEDIASUBTYPE_Y210) return GCAP_FMT_Y210;
#ifdef MFVideoFormat_NV12
    if (sub == MFVideoFormat_NV12) return GCAP_FMT_NV12;
    if (sub == MFVideoFormat_YUY2) return GCAP_FMT_YUY2;
    if (sub == MFVideoFormat_P010) return GCAP_FMT_P010;
    if (sub == MFVideoFormat_Y210) return GCAP_FMT_Y210;
    if (sub == MFVideoFormat_ARGB32) return GCAP_FMT_ARGB;
#endif
    return GCAP_FMT_ARGB;
}

int gcap_pixfmt_bitdepth(gcap_pixfmt_t f)
{
    switch (f)
    {
    case GCAP_FMT_P010:
    case GCAP_FMT_Y210:
    case GCAP_FMT_R210:
    case GCAP_FMT_V210:
        return 10;
    default:
        return 8;
    }
}

const char *gcap_pixfmt_name(gcap_pixfmt_t f)
{
    switch (f)
    {
    case GCAP_FMT_NV12: return "NV12";
    case GCAP_FMT_YUY2: return "YUY2";
    case GCAP_FMT_P010: return "P010";
    case GCAP_FMT_Y210: return "Y210";
    case GCAP_FMT_ARGB: return "ARGB";
    default: return "Unknown";
    }
}

const char *gcap_subtype_name(const GUID &sub)
{
    if (sub == MEDIASUBTYPE_NV12) return "NV12";
    if (sub == MEDIASUBTYPE_YUY2) return "YUY2";
    if (sub == MEDIASUBTYPE_Y210) return "Y210";
#ifdef MFVideoFormat_NV12
    if (sub == MFVideoFormat_NV12) return "NV12";
    if (sub == MFVideoFormat_YUY2) return "YUY2";
    if (sub == MFVideoFormat_P010) return "P010";
    if (sub == MFVideoFormat_Y210) return "Y210";
    if (sub == MFVideoFormat_ARGB32) return "ARGB";
#endif
    return "Unknown";
}
