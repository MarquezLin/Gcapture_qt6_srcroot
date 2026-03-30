#include "dshow_signal_probe.h"

#include <objbase.h>
#include <dvdmedia.h>
#include <wrl/client.h>
#include <vector>
#include <cmath>
#include <string>
#include <sstream>
#include <ks.h>
#include <ksproxy.h>
#include <olectl.h>
#include <strsafe.h>
#include <windows.h>
#include <initguid.h>

using Microsoft::WRL::ComPtr;

namespace
{
    // Some SDK / WDK combinations do not expose these PROPSETIDs / AMPROPSETIDs.
    // Define local copies so this file can still compile and probe QuerySupported().
    // These GUID values follow the standard DirectShow / KS definitions.
    static const GUID DSHOW_AMPROPSETID_Pin =
        {0x9B00F101, 0x1567, 0x11D1, {0xB3, 0xF1, 0x00, 0xAA, 0x00, 0x37, 0x61, 0xC5}};

    static const GUID DSHOW_AMPROPSETID_CopyProt =
        {0x0E8A0A40, 0x6AEF, 0x11D0, {0x9E, 0xD0, 0x00, 0xA0, 0x24, 0xCA, 0x19, 0xB3}};

    static const GUID DSHOW_AM_KSPROPSETID_TSRateChange =
        {0xA8985F22, 0xAC76, 0x11D0, {0xBD, 0xF5, 0x00, 0xAA, 0x00, 0xB6, 0x7A, 0x42}};

    static const GUID DSHOW_PROPSETID_VIDCAP_VIDEOCONTROL =
        {0xC6E13360, 0x30AC, 0x11D0, {0xA1, 0x8C, 0x00, 0xA0, 0xC9, 0x11, 0x89, 0x56}};

    static const GUID DSHOW_PROPSETID_VIDCAP_CAMERACONTROL =
        {0xC6E13370, 0x30AC, 0x11D0, {0xA1, 0x8C, 0x00, 0xA0, 0xC9, 0x11, 0x89, 0x56}};

    static const GUID DSHOW_PROPSETID_VIDCAP_DROPPEDFRAMES =
        {0xC6E13344, 0x30AC, 0x11D0, {0xA1, 0x8C, 0x00, 0xA0, 0xC9, 0x11, 0x89, 0x56}};

    static const GUID DSHOW_SC0710_VENDOR_PAGE =
        {0x15E3F6CE, 0xF3DD, 0x454A, {0x8D, 0xEA, 0xE4, 0xA3, 0x80, 0xFC, 0xCB, 0x26}};

    static std::string hr_to_hex(HRESULT hr)
    {
        char buf[32] = {};
        sprintf_s(buf, sizeof(buf), "%08X", static_cast<unsigned>(hr));
        return std::string(buf);
    }

    static void dshow_probe_log(const std::string &s)
    {
        std::string line = "[DShowSignalProbe] " + s;
        OutputDebugStringA(line.c_str());
    }

    static std::string wide_to_utf8(const wchar_t *ws)
    {
        if (!ws || !*ws)
            return std::string();
        const int needed = WideCharToMultiByte(CP_UTF8, 0, ws, -1, NULL, 0, NULL, NULL);
        if (needed <= 1)
            return std::string();
        std::string out(static_cast<size_t>(needed - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, ws, -1, &out[0], needed, NULL, NULL);
        return out;
    }

    static std::string guid_to_string(const GUID &g)
    {
        wchar_t buf[64] = {};
        StringFromGUID2(g, buf, static_cast<int>(sizeof(buf) / sizeof(buf[0])));
        return wide_to_utf8(buf);
    }

    static std::wstring guid_to_wstring(const GUID &g)
    {
        wchar_t buf[64] = {};
        StringFromGUID2(g, buf, static_cast<int>(sizeof(buf) / sizeof(buf[0])));
        return std::wstring(buf);
    }

    static std::wstring read_reg_default_string(HKEY root, const std::wstring &subkey)
    {
        HKEY hKey = NULL;
        if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ, &hKey) != ERROR_SUCCESS)
            return L"";

        wchar_t buf[1024] = {};
        DWORD type = 0;
        DWORD cb = sizeof(buf);
        LONG rc = RegQueryValueExW(hKey, NULL, NULL, &type, reinterpret_cast<LPBYTE>(buf), &cb);
        RegCloseKey(hKey);

        if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
            return L"";
        return std::wstring(buf);
    }

    static void log_clsid_registry_info(const CLSID &clsid)
    {
        std::wstring g = guid_to_wstring(clsid);
        std::wstring base = L"CLSID\\" + g;
        std::wstring name = read_reg_default_string(HKEY_CLASSES_ROOT, base);
        std::wstring dll = read_reg_default_string(HKEY_CLASSES_ROOT, base + L"\\InprocServer32");
        std::wstring prog = read_reg_default_string(HKEY_CLASSES_ROOT, base + L"\\ProgID");

        dshow_probe_log("CLSID " + wide_to_utf8(g.c_str()));
        dshow_probe_log("  Name=" + (name.empty() ? std::string("(none)") : wide_to_utf8(name.c_str())));
        dshow_probe_log("  InprocServer32=" + (dll.empty() ? std::string("(none)") : wide_to_utf8(dll.c_str())));
        dshow_probe_log("  ProgID=" + (prog.empty() ? std::string("(none)") : wide_to_utf8(prog.c_str())));
    }

    static void read_moniker_strings(IMoniker *moniker, DShowSignalProbeResult &out)
    {
        if (!moniker)
            return;

        ComPtr<IPropertyBag> bag;
        if (SUCCEEDED(moniker->BindToStorage(NULL, NULL, IID_PPV_ARGS(&bag))) && bag)
        {
            VARIANT v;
            VariantInit(&v);
            if (SUCCEEDED(bag->Read(L"FriendlyName", &v, NULL)) && v.vt == VT_BSTR && v.bstrVal)
                wcsncpy_s(out.friendly_name, v.bstrVal, _TRUNCATE);
            VariantClear(&v);

            VariantInit(&v);
            if (SUCCEEDED(bag->Read(L"DevicePath", &v, NULL)) && v.vt == VT_BSTR && v.bstrVal)
                wcsncpy_s(out.device_path, v.bstrVal, _TRUNCATE);
            VariantClear(&v);
        }
    }

    static void log_property_pages(const char *label, IUnknown *unk, bool &hasPages)
    {
        if (!unk)
            return;

        ComPtr<ISpecifyPropertyPages> spp;
        if (FAILED(unk->QueryInterface(IID_PPV_ARGS(&spp))) || !spp)
        {
            dshow_probe_log(std::string(label) + " ISpecifyPropertyPages: NO");
            return;
        }

        hasPages = true;
        CAUUID cauuid = {};
        HRESULT hr = spp->GetPages(&cauuid);
        if (FAILED(hr))
        {
            dshow_probe_log(std::string(label) + " GetPages failed hr=0x" + hr_to_hex(hr));
            return;
        }

        std::ostringstream oss;
        oss << label << " property pages: count=" << cauuid.cElems;
        for (ULONG i = 0; i < cauuid.cElems; ++i)
            oss << " [" << i << "]=" << guid_to_string(cauuid.pElems[i]);
        dshow_probe_log(oss.str());

        for (ULONG i = 0; i < cauuid.cElems; ++i)
            log_clsid_registry_info(cauuid.pElems[i]);

        if (cauuid.pElems)
            CoTaskMemFree(cauuid.pElems);
    }

    struct KsSetCandidate
    {
        const char *name;
        GUID set;
    };

    static void dump_known_ks_query_support(const char *label, IKsPropertySet *ksps)
    {
        if (!ksps)
            return;

        static const KsSetCandidate sets[] = {
            {"AMPROPSETID_Pin", DSHOW_AMPROPSETID_Pin},
            {"AMPROPSETID_CopyProt", DSHOW_AMPROPSETID_CopyProt},
            {"AM_KSPROPSETID_TSRateChange", DSHOW_AM_KSPROPSETID_TSRateChange},
            {"PROPSETID_VIDCAP_VIDEOCONTROL", DSHOW_PROPSETID_VIDCAP_VIDEOCONTROL},
            {"PROPSETID_VIDCAP_CAMERACONTROL", DSHOW_PROPSETID_VIDCAP_CAMERACONTROL},
            {"PROPSETID_VIDCAP_DROPPEDFRAMES", DSHOW_PROPSETID_VIDCAP_DROPPEDFRAMES},
            {"SC0710_VENDOR_PAGE", DSHOW_SC0710_VENDOR_PAGE},
        };

        const size_t setCount = sizeof(sets) / sizeof(sets[0]);
        for (size_t i = 0; i < setCount; ++i)
        {
            const KsSetCandidate &candidate = sets[i];
            DWORD support = 0;
            HRESULT hr = ksps->QuerySupported(candidate.set, 0, &support);
            char buf[256] = {};
            sprintf_s(buf, sizeof(buf),
                      "%s QuerySupported set=%s hr=0x%08X support=0x%08X",
                      label,
                      candidate.name,
                      static_cast<unsigned>(hr),
                      static_cast<unsigned>(support));
            dshow_probe_log(buf);
        }
    }

    static void log_ks_support(const char *label, IUnknown *unk,
                               bool &hasKsPropertySet,
                               bool &hasKsControl)
    {
        if (!unk)
            return;

        ComPtr<IKsPropertySet> ksps;
        hasKsPropertySet = (SUCCEEDED(unk->QueryInterface(IID_PPV_ARGS(&ksps))) && ksps);
        dshow_probe_log(std::string(label) + " IKsPropertySet: " + (hasKsPropertySet ? "YES" : "NO"));
        if (hasKsPropertySet)
            dump_known_ks_query_support(label, ksps.Get());

        ComPtr<IKsControl> ksc;
        hasKsControl = (SUCCEEDED(unk->QueryInterface(__uuidof(IKsControl), reinterpret_cast<void **>(ksc.GetAddressOf()))) && ksc);
        dshow_probe_log(std::string(label) + " IKsControl: " + (hasKsControl ? "YES" : "NO"));
    }

    static IPin *find_capture_pin(ICaptureGraphBuilder2 *capBuilder, IBaseFilter *sourceFilter)
    {
        if (!capBuilder || !sourceFilter)
            return NULL;

        IPin *pin = NULL;
        HRESULT hr = capBuilder->FindPin(sourceFilter,
                                         PINDIR_OUTPUT,
                                         &PIN_CATEGORY_CAPTURE,
                                         &MEDIATYPE_Video,
                                         FALSE,
                                         0,
                                         &pin);
        if (FAILED(hr))
            return NULL;
        return pin;
    }

    static void log_pin_identity(IPin *pin)
    {
        if (!pin)
            return;
        PIN_INFO info = {};
        if (SUCCEEDED(pin->QueryPinInfo(&info)))
        {
            std::string name = wide_to_utf8(info.achName);
            dshow_probe_log(std::string("Capture pin name: ") + (name.empty() ? "(unknown)" : name));
            if (info.pFilter)
                info.pFilter->Release();
        }
    }

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
                const VIDEOINFOHEADER2 *vih2 = reinterpret_cast<const VIDEOINFOHEADER2 *>(mt.pbFormat);
                width = vih2->bmiHeader.biWidth;
                height = vih2->bmiHeader.biHeight;
                avg = vih2->AvgTimePerFrame;
            }
            else
            {
                const VIDEOINFOHEADER *vih = reinterpret_cast<const VIDEOINFOHEADER *>(mt.pbFormat);
                width = vih->bmiHeader.biWidth;
                height = vih->bmiHeader.biHeight;
                avg = vih->AvgTimePerFrame;
            }
        }

        out.width = static_cast<int>(width);
        out.height = static_cast<int>(std::abs(height));
        out.subtype = mt.subtype;
        out.fps_den = 1;
        out.fps_num = (avg > 0) ? static_cast<int>((10000000LL + avg / 2) / avg) : 0;
        out.ok = (out.width > 0 && out.height > 0);
        return out.ok;
    }

    static void dshow_free_media_type(AM_MEDIA_TYPE &mt)
    {
        if (mt.cbFormat != 0 && mt.pbFormat)
        {
            CoTaskMemFree(mt.pbFormat);
            mt.cbFormat = 0;
            mt.pbFormat = NULL;
        }
        if (mt.pUnk)
        {
            mt.pUnk->Release();
            mt.pUnk = NULL;
        }
    }

    static bool dshow_probe_impl(int devIndex, DShowSignalProbeResult &out, bool verbose)
    {
        out = DShowSignalProbeResult();

        HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        const bool need_uninit = SUCCEEDED(hr);
        if (hr != S_OK && hr != S_FALSE && hr != RPC_E_CHANGED_MODE)
            return false;

        bool ok = false;
        do
        {
            ComPtr<ICreateDevEnum> devEnum;
            hr = CoCreateInstance(CLSID_SystemDeviceEnum, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&devEnum));
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

            read_moniker_strings(moniker.Get(), out);
            if (verbose)
            {
                dshow_probe_log(std::string("Device index=") + std::to_string(devIndex));
                {
                    std::string fn = wide_to_utf8(out.friendly_name);
                    dshow_probe_log(std::string("FriendlyName=") + (fn.empty() ? "(unknown)" : fn));
                }
                {
                    std::string dp = wide_to_utf8(out.device_path);
                    dshow_probe_log(std::string("DevicePath=") + (dp.empty() ? "(unknown)" : dp));
                }
            }

            ComPtr<IBaseFilter> sourceFilter;
            hr = moniker->BindToObject(NULL, NULL, IID_PPV_ARGS(&sourceFilter));
            if (FAILED(hr))
                break;

            if (verbose)
            {
                log_property_pages("Filter", sourceFilter.Get(), out.filter_has_property_pages);
                log_ks_support("Filter", sourceFilter.Get(), out.filter_has_ks_property_set, out.filter_has_ks_control);
            }

            ComPtr<IGraphBuilder> graph;
            hr = CoCreateInstance(CLSID_FilterGraph, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&graph));
            if (FAILED(hr))
                break;

            hr = graph->AddFilter(sourceFilter.Get(), L"VideoCapture");
            if (FAILED(hr))
                break;

            ComPtr<ICaptureGraphBuilder2> capBuilder;
            hr = CoCreateInstance(CLSID_CaptureGraphBuilder2, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&capBuilder));
            if (FAILED(hr))
                break;
            capBuilder->SetFiltergraph(graph.Get());

            ComPtr<IPin> capturePin;
            capturePin.Attach(find_capture_pin(capBuilder.Get(), sourceFilter.Get()));
            if (capturePin && verbose)
            {
                log_pin_identity(capturePin.Get());
                log_property_pages("CapturePin", capturePin.Get(), out.capture_pin_has_property_pages);
                log_ks_support("CapturePin", capturePin.Get(), out.capture_pin_has_ks_property_set, out.capture_pin_has_ks_control);
            }

            ComPtr<IAMStreamConfig> cfg;
            hr = capBuilder->FindInterface(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, sourceFilter.Get(), IID_PPV_ARGS(&cfg));
            if (FAILED(hr) || !cfg)
                break;

            AM_MEDIA_TYPE *pmt = NULL;
            hr = cfg->GetFormat(&pmt);
            if (SUCCEEDED(hr) && pmt)
            {
                ok = dshow_extract_media_type(*pmt, out);
                dshow_free_media_type(*pmt);
                CoTaskMemFree(pmt);
                if (ok)
                {
                    out.from_get_format = true;
                    if (verbose)
                    {
                        std::ostringstream oss;
                        oss << "GetFormat -> " << out.width << "x" << out.height
                            << " " << gcap_subtype_name(out.subtype)
                            << " " << out.fps_num << "/" << out.fps_den << " fps";
                        dshow_probe_log(oss.str());
                        dshow_probe_log("IMPORTANT: GetFormat is capture-pin / negotiated format, not guaranteed true input signal.");
                    }
                    break;
                }
            }

            int count = 0;
            int capSize = 0;
            hr = cfg->GetNumberOfCapabilities(&count, &capSize);
            if (FAILED(hr) || count <= 0 || capSize <= 0)
                break;

            std::vector<unsigned char> caps(static_cast<size_t>(capSize));
            for (int i = 0; i < count; ++i)
            {
                AM_MEDIA_TYPE *capsMt = NULL;
                if (FAILED(cfg->GetStreamCaps(i, &capsMt, caps.data())) || !capsMt)
                    continue;

                if (dshow_extract_media_type(*capsMt, out))
                {
                    dshow_free_media_type(*capsMt);
                    CoTaskMemFree(capsMt);
                    ok = true;
                    out.from_stream_caps = true;
                    if (verbose)
                    {
                        std::ostringstream oss;
                        oss << "GetStreamCaps[" << i << "] -> " << out.width << "x" << out.height
                            << " " << gcap_subtype_name(out.subtype)
                            << " " << out.fps_num << "/" << out.fps_den << " fps";
                        dshow_probe_log(oss.str());
                        dshow_probe_log("IMPORTANT: GetStreamCaps is capability list, also not guaranteed true input signal.");
                    }
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
} // namespace

bool dshow_probe_current_signal_by_index(int devIndex, DShowSignalProbeResult &out)
{
    return dshow_probe_impl(devIndex, out, false);
}

void dshow_dump_signal_diagnostics_by_index(int devIndex)
{
    DShowSignalProbeResult out{};
    bool ok = dshow_probe_impl(devIndex, out, true);
    if (!ok)
        dshow_probe_log("Probe failed.");
}

gcap_pixfmt_t gcap_subtype_to_pixfmt(const GUID &sub)
{
    if (sub == MEDIASUBTYPE_NV12)
        return GCAP_FMT_NV12;
    if (sub == MEDIASUBTYPE_YUY2)
        return GCAP_FMT_YUY2;
    if (sub == MEDIASUBTYPE_Y210)
        return GCAP_FMT_Y210;
#ifdef MFVideoFormat_NV12
    if (sub == MFVideoFormat_NV12)
        return GCAP_FMT_NV12;
    if (sub == MFVideoFormat_YUY2)
        return GCAP_FMT_YUY2;
    if (sub == MFVideoFormat_P010)
        return GCAP_FMT_P010;
    if (sub == MFVideoFormat_Y210)
        return GCAP_FMT_Y210;
    if (sub == MFVideoFormat_ARGB32)
        return GCAP_FMT_ARGB;
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
    case GCAP_FMT_NV12:
        return "NV12";
    case GCAP_FMT_YUY2:
        return "YUY2";
    case GCAP_FMT_P010:
        return "P010";
    case GCAP_FMT_Y210:
        return "Y210";
    case GCAP_FMT_ARGB:
        return "ARGB32";
    default:
        return "Unknown";
    }
}

const char *gcap_subtype_name(const GUID &sub)
{
    if (sub == MEDIASUBTYPE_NV12)
        return "NV12";
    if (sub == MEDIASUBTYPE_YUY2)
        return "YUY2";
    if (sub == MEDIASUBTYPE_Y210)
        return "Y210";
    if (sub == MEDIASUBTYPE_RGB24)
        return "RGB24";
    if (sub == MEDIASUBTYPE_RGB32)
        return "RGB32";
    if (sub == MEDIASUBTYPE_ARGB32)
        return "ARGB32";
#ifdef MFVideoFormat_NV12
    if (sub == MFVideoFormat_NV12)
        return "NV12";
    if (sub == MFVideoFormat_YUY2)
        return "YUY2";
    if (sub == MFVideoFormat_P010)
        return "P010";
    if (sub == MFVideoFormat_Y210)
        return "Y210";
    if (sub == MFVideoFormat_ARGB32)
        return "ARGB32";
#endif
    return "Unknown";
}
