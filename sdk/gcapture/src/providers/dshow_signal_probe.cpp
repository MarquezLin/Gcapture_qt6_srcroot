#include "dshow_signal_probe.h"

#include <objbase.h>
#include <dvdmedia.h>
#include <mfapi.h>
#include <mfobjects.h>
#include <wrl/client.h>
#include <vector>
#include <cmath>
#include <string>
#include <sstream>
#include <atomic>
#include <thread>
#include <ks.h>
#include <ksproxy.h>
#include <olectl.h>
#include <ocidl.h>
#include <strsafe.h>
#include <windows.h>
#include <initguid.h>

#pragma comment(lib, "Version.lib")

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

    static std::wstring utf8_to_wide(const char *s)
    {
        if (!s || !*s)
            return std::wstring();
        const int needed = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
        if (needed <= 1)
            return std::wstring();
        std::wstring out(static_cast<size_t>(needed - 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s, -1, &out[0], needed);
        return out;
    }

    static void dshow_probe_log(const std::string &s)
    {
        const std::wstring line = utf8_to_wide(("[DShowSignalProbe] " + s).c_str()) + L"\n";
        OutputDebugStringW(line.c_str());
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

    static std::wstring resolve_module_path_from_inproc(const std::wstring &inproc)
    {
        if (inproc.empty())
            return L"";

        wchar_t expanded[1024] = {};
        DWORD n = ExpandEnvironmentStringsW(inproc.c_str(), expanded,
                                            static_cast<DWORD>(sizeof(expanded) / sizeof(expanded[0])));
        std::wstring path = (n > 0 && n < (sizeof(expanded) / sizeof(expanded[0])))
                                ? std::wstring(expanded)
                                : inproc;

        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
            return path;

        wchar_t systemDir[MAX_PATH] = {};
        UINT sysLen = GetSystemDirectoryW(systemDir, MAX_PATH);
        if (sysLen > 0 && sysLen < MAX_PATH)
        {
            std::wstring candidate = std::wstring(systemDir) + L"\\" + path;
            if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES)
                return candidate;
        }

        wchar_t modulePath[MAX_PATH] = {};
        HMODULE hMod = NULL;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               inproc.c_str(), &hMod) && hMod)
        {
            if (GetModuleFileNameW(hMod, modulePath, MAX_PATH) > 0)
                return std::wstring(modulePath);
        }

        return path;
    }

    static std::string version_msls_to_string(DWORD ms, DWORD ls)
    {
        std::ostringstream oss;
        oss << HIWORD(ms) << "." << LOWORD(ms) << "." << HIWORD(ls) << "." << LOWORD(ls);
        return oss.str();
    }


    static std::string format_u64(unsigned long long v)
    {
        char buf[64] = {};
        sprintf_s(buf, sizeof(buf), "%llu", v);
        return std::string(buf);
    }

    static std::wstring get_directory_part(const std::wstring &path)
    {
        const std::wstring::size_type pos = path.find_last_of(L"\\/");
        if (pos == std::wstring::npos)
            return L"";
        return path.substr(0, pos);
    }

    static std::wstring get_filename_part(const std::wstring &path)
    {
        const std::wstring::size_type pos = path.find_last_of(L"\\/");
        if (pos == std::wstring::npos)
            return path;
        return path.substr(pos + 1);
    }

    static bool starts_with_case_insensitive(const std::wstring &s, const std::wstring &prefix)
    {
        if (s.size() < prefix.size())
            return false;
        for (size_t i = 0; i < prefix.size(); ++i)
        {
            if (towlower(s[i]) != towlower(prefix[i]))
                return false;
        }
        return true;
    }

    static std::string read_version_string_field(const std::wstring &path, const wchar_t *fieldName)
    {
        DWORD handle = 0;
        DWORD verSize = GetFileVersionInfoSizeW(path.c_str(), &handle);
        if (verSize == 0)
            return std::string();

        std::vector<BYTE> verData(verSize);
        if (!GetFileVersionInfoW(path.c_str(), 0, verSize, verData.data()))
            return std::string();

        struct LANGANDCODEPAGE { WORD wLanguage; WORD wCodePage; } *translate = NULL;
        UINT cbTranslate = 0;
        if (!VerQueryValueW(verData.data(), L"\\VarFileInfo\\Translation",
                            reinterpret_cast<LPVOID *>(&translate), &cbTranslate) ||
            !translate || cbTranslate < sizeof(LANGANDCODEPAGE))
            return std::string();

        wchar_t subBlock[256] = {};
        StringCchPrintfW(subBlock, sizeof(subBlock) / sizeof(subBlock[0]),
                         L"\\StringFileInfo\\%04x%04x\\%s",
                         translate[0].wLanguage, translate[0].wCodePage, fieldName);

        wchar_t *value = NULL;
        UINT valueLen = 0;
        if (VerQueryValueW(verData.data(), subBlock,
                           reinterpret_cast<LPVOID *>(&value), &valueLen) &&
            value && valueLen > 0)
            return wide_to_utf8(value);
        return std::string();
    }

    static void log_pe_exports(const std::wstring &path)
    {
        HMODULE h = LoadLibraryExW(path.c_str(), NULL, LOAD_LIBRARY_AS_DATAFILE | DONT_RESOLVE_DLL_REFERENCES);
        if (!h)
        {
            dshow_probe_log("  Exports=unavailable(load failed)");
            return;
        }

        const BYTE *base = reinterpret_cast<const BYTE *>(h);
        const IMAGE_DOS_HEADER *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
        if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE)
        {
            dshow_probe_log("  Exports=unavailable(bad dos header)");
            FreeLibrary(h);
            return;
        }

        const IMAGE_NT_HEADERS *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
        if (!nt || nt->Signature != IMAGE_NT_SIGNATURE)
        {
            dshow_probe_log("  Exports=unavailable(bad nt header)");
            FreeLibrary(h);
            return;
        }

        const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (dir.VirtualAddress == 0 || dir.Size == 0)
        {
            dshow_probe_log("  Exports=count=0");
            FreeLibrary(h);
            return;
        }

        const IMAGE_EXPORT_DIRECTORY *exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY *>(base + dir.VirtualAddress);
        dshow_probe_log(std::string("  Exports=count=") + format_u64(exp->NumberOfNames));
        if (exp->NumberOfNames > 0 && exp->AddressOfNames)
        {
            const DWORD *nameRVAs = reinterpret_cast<const DWORD *>(base + exp->AddressOfNames);
            const DWORD limit = exp->NumberOfNames < 64 ? exp->NumberOfNames : 64;
            for (DWORD i = 0; i < limit; ++i)
            {
                const char *namePtr = reinterpret_cast<const char *>(base + nameRVAs[i]);
                if (!namePtr)
                    continue;
                dshow_probe_log(std::string("  Export[") + format_u64(i) + "]=" + namePtr);
            }
            if (exp->NumberOfNames > limit)
                dshow_probe_log("  Export[...] truncated");
        }
        FreeLibrary(h);
    }

    static void log_sibling_modules(const std::wstring &resolvedPath)
    {
        if (resolvedPath.empty())
            return;

        const std::wstring dir = get_directory_part(resolvedPath);
        if (dir.empty())
            return;

        const std::wstring pattern = dir + L"\\SC0710.*";
        WIN32_FIND_DATAW ffd = {};
        HANDLE hFind = FindFirstFileW(pattern.c_str(), &ffd);
        if (hFind == INVALID_HANDLE_VALUE)
            return;

        int index = 0;
        do
        {
            if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            const std::wstring fileName = ffd.cFileName;
            std::wstring fullPath = dir + L"\\" + fileName;
            ULONGLONG size = (static_cast<ULONGLONG>(ffd.nFileSizeHigh) << 32) | ffd.nFileSizeLow;
            dshow_probe_log(std::string("  Sibling[") + format_u64(index) + "]=" +
                            wide_to_utf8(fullPath.c_str()) +
                            " size=" + format_u64(size));
            const std::string desc = read_version_string_field(fullPath, L"FileDescription");
            const std::string orig = read_version_string_field(fullPath, L"OriginalFilename");
            if (!desc.empty())
                dshow_probe_log(std::string("    FileDescription=") + desc);
            if (!orig.empty())
                dshow_probe_log(std::string("    OriginalFilename=") + orig);
            ++index;
        } while (FindNextFileW(hFind, &ffd));
        FindClose(hFind);
    }

    static void log_module_probe_info(const std::wstring &inproc)
    {
        if (inproc.empty())
            return;

        std::wstring resolved = resolve_module_path_from_inproc(inproc);
        dshow_probe_log(std::string("  ResolvedModulePath=") +
                        (resolved.empty() ? std::string("(none)") : wide_to_utf8(resolved.c_str())));

        DWORD attrs = resolved.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(resolved.c_str());
        bool exists = (attrs != INVALID_FILE_ATTRIBUTES) && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
        dshow_probe_log(std::string("  ModuleExists=") + (exists ? "YES" : "NO"));

        HMODULE h = NULL;
        if (exists)
            h = LoadLibraryExW(resolved.c_str(), NULL,
                               LOAD_LIBRARY_AS_DATAFILE | DONT_RESOLVE_DLL_REFERENCES);
        dshow_probe_log(std::string("  ModuleLoadable=") + (h ? "YES" : "NO"));
        if (h)
            FreeLibrary(h);

        if (!exists)
            return;

        DWORD handle = 0;
        DWORD verSize = GetFileVersionInfoSizeW(resolved.c_str(), &handle);
        if (verSize > 0)
        {
            std::vector<BYTE> verData(verSize);
            if (GetFileVersionInfoW(resolved.c_str(), 0, verSize, verData.data()))
            {
                VS_FIXEDFILEINFO *ffi = NULL;
                UINT ffiLen = 0;
                if (VerQueryValueW(verData.data(), L"\\",
                                   reinterpret_cast<LPVOID *>(&ffi), &ffiLen) &&
                    ffi && ffiLen >= sizeof(VS_FIXEDFILEINFO))
                {
                    dshow_probe_log(std::string("  FileVersion=") +
                                    version_msls_to_string(ffi->dwFileVersionMS,
                                                           ffi->dwFileVersionLS));
                }
            }
        }

        const std::string company = read_version_string_field(resolved, L"CompanyName");
        const std::string fileDesc = read_version_string_field(resolved, L"FileDescription");
        const std::string product = read_version_string_field(resolved, L"ProductName");
        const std::string original = read_version_string_field(resolved, L"OriginalFilename");
        if (!company.empty()) dshow_probe_log(std::string("  CompanyName=") + company);
        if (!fileDesc.empty()) dshow_probe_log(std::string("  FileDescription=") + fileDesc);
        if (!product.empty()) dshow_probe_log(std::string("  ProductName=") + product);
        if (!original.empty()) dshow_probe_log(std::string("  OriginalFilename=") + original);

        log_pe_exports(resolved);
        if (starts_with_case_insensitive(get_filename_part(resolved), L"SC0710."))
            log_sibling_modules(resolved);
    }



    class ProbePropertyPageSite : public IPropertyPageSite
    {
    public:
        ProbePropertyPageSite() : ref_(1) {}

        STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
        {
            if (!ppv)
                return E_POINTER;
            *ppv = nullptr;
            if (riid == IID_IUnknown || riid == IID_IPropertyPageSite)
            {
                *ppv = static_cast<IPropertyPageSite *>(this);
                AddRef();
                return S_OK;
            }
            return E_NOINTERFACE;
        }

        STDMETHODIMP_(ULONG) AddRef() override
        {
            return static_cast<ULONG>(InterlockedIncrement(&ref_));
        }

        STDMETHODIMP_(ULONG) Release() override
        {
            const ULONG v = static_cast<ULONG>(InterlockedDecrement(&ref_));
            if (v == 0)
                delete this;
            return v;
        }

        STDMETHODIMP OnStatusChange(DWORD dwFlags) override
        {
            char buf[128] = {};
            sprintf_s(buf, sizeof(buf), "PropertyPageSite OnStatusChange dwFlags=0x%08X",
                      static_cast<unsigned>(dwFlags));
            dshow_probe_log(buf);
            return S_OK;
        }

        STDMETHODIMP GetLocaleID(LCID *pLocaleID) override
        {
            if (!pLocaleID)
                return E_POINTER;
            *pLocaleID = GetUserDefaultLCID();
            return S_OK;
        }

        STDMETHODIMP GetPageContainer(IUnknown **ppUnk) override
        {
            if (!ppUnk)
                return E_POINTER;
            *ppUnk = nullptr;
            return E_NOTIMPL;
        }

        STDMETHODIMP TranslateAccelerator(MSG *) override
        {
            return E_NOTIMPL;
        }

    private:
        volatile long ref_;
    };

    static HWND create_probe_page_parent_window(const wchar_t *title, int width, int height)
    {
        static const wchar_t kClassName[] = L"DShowSignalProbePageParent";
        static bool classRegistered = false;
        if (!classRegistered)
        {
            WNDCLASSW wc = {};
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = GetModuleHandleW(NULL);
            wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
            wc.lpszClassName = kClassName;
            ATOM atom = RegisterClassW(&wc);
            if (atom)
                classRegistered = true;
        }

        if (!classRegistered)
            return NULL;

        const int w = (width > 0 ? width : 640);
        const int h = (height > 0 ? height : 480);
        return CreateWindowExW(WS_EX_TOOLWINDOW, kClassName,
                               (title && *title) ? title : L"DShow Signal Probe",
                               WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                               CW_USEDEFAULT, CW_USEDEFAULT, w, h,
                               NULL, NULL, GetModuleHandleW(NULL), NULL);
    }

    static void pump_probe_window_messages(HWND hwnd, DWORD durationMs)
    {
        const DWORD start = GetTickCount();
        MSG msg = {};
        while ((GetTickCount() - start) < durationMs)
        {
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (hwnd)
                UpdateWindow(hwnd);
            Sleep(15);
        }
    }

    static bool try_property_page_activate_cycle(const char *label,
                                                 const CLSID &clsid,
                                                 IUnknown *target,
                                                 DShowSignalProbeResult *probeOut)
    {
        if (!target)
            return false;

        ComPtr<IPropertyPage> page;
        HRESULT hr = CoCreateInstance(clsid, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&page));
        if (FAILED(hr) || !page)
        {
            dshow_probe_log(std::string(label) + " ActivateCycle CoCreateInstance(IPropertyPage) failed hr=0x" + hr_to_hex(hr));
            return false;
        }

        ProbePropertyPageSite *rawSite = new ProbePropertyPageSite();
        if (!rawSite)
            return false;
        ComPtr<IPropertyPageSite> site;
        site.Attach(rawSite);

        PROPPAGEINFO info = {};
        info.cb = sizeof(info);
        hr = page->GetPageInfo(&info);
        if (FAILED(hr))
        {
            dshow_probe_log(std::string(label) + " IPropertyPage::GetPageInfo failed hr=0x" + hr_to_hex(hr));
        }
        else
        {
            std::ostringstream oss;
            oss << label << " IPropertyPage::GetPageInfo: OK"
                << " size=" << info.size.cx << "x" << info.size.cy;
            if (info.pszTitle)
                oss << " title=" << wide_to_utf8(info.pszTitle);
            dshow_probe_log(oss.str());
        }

        hr = page->SetPageSite(site.Get());
        if (FAILED(hr))
        {
            dshow_probe_log(std::string(label) + " IPropertyPage::SetPageSite failed hr=0x" + hr_to_hex(hr));
            if (info.pszTitle) CoTaskMemFree(info.pszTitle);
            if (info.pszDocString) CoTaskMemFree(info.pszDocString);
            if (info.pszHelpFile) CoTaskMemFree(info.pszHelpFile);
            return false;
        }
        dshow_probe_log(std::string(label) + " IPropertyPage::SetPageSite: OK");

        IUnknown *objs[1] = {target};
        hr = page->SetObjects(1, objs);
        if (FAILED(hr))
        {
            dshow_probe_log(std::string(label) + " ActivateCycle IPropertyPage::SetObjects failed hr=0x" + hr_to_hex(hr));
            page->SetPageSite(NULL);
            if (info.pszTitle) CoTaskMemFree(info.pszTitle);
            if (info.pszDocString) CoTaskMemFree(info.pszDocString);
            if (info.pszHelpFile) CoTaskMemFree(info.pszHelpFile);
            return false;
        }
        dshow_probe_log(std::string(label) + " ActivateCycle IPropertyPage::SetObjects: OK");

        const int pageW = (info.size.cx > 0 ? info.size.cx : 640);
        const int pageH = (info.size.cy > 0 ? info.size.cy : 480);
        HWND hwnd = create_probe_page_parent_window(info.pszTitle, pageW, pageH);
        if (!hwnd)
        {
            dshow_probe_log(std::string(label) + " create hidden parent window failed");
            page->SetObjects(0, NULL);
            page->SetPageSite(NULL);
            if (info.pszTitle) CoTaskMemFree(info.pszTitle);
            if (info.pszDocString) CoTaskMemFree(info.pszDocString);
            if (info.pszHelpFile) CoTaskMemFree(info.pszHelpFile);
            return false;
        }

        RECT rc = {0, 0, pageW, pageH};
        hr = page->Activate(hwnd, &rc, FALSE);
        if (FAILED(hr))
        {
            dshow_probe_log(std::string(label) + " IPropertyPage::Activate failed hr=0x" + hr_to_hex(hr));
            DestroyWindow(hwnd);
            page->SetObjects(0, NULL);
            page->SetPageSite(NULL);
            if (info.pszTitle) CoTaskMemFree(info.pszTitle);
            if (info.pszDocString) CoTaskMemFree(info.pszDocString);
            if (info.pszHelpFile) CoTaskMemFree(info.pszHelpFile);
            return false;
        }
        dshow_probe_log(std::string(label) + " IPropertyPage::Activate: OK");

        dshow_probe_log(std::string(label) + " Activate-only probe: skip IPropertyPage::Show/message-loop to avoid vendor page crash");

        page->Deactivate();
        dshow_probe_log(std::string(label) + " IPropertyPage::Deactivate: OK");
        DestroyWindow(hwnd);
        page->SetObjects(0, NULL);
        page->SetPageSite(NULL);

        if (info.pszTitle) CoTaskMemFree(info.pszTitle);
        if (info.pszDocString) CoTaskMemFree(info.pszDocString);
        if (info.pszHelpFile) CoTaskMemFree(info.pszHelpFile);

        if (probeOut && clsid == DSHOW_SC0710_VENDOR_PAGE)
        {
            if (strcmp(label, "Filter") == 0)
                probeOut->vendor_page_activate_filter_ok = true;
            else if (strcmp(label, "CapturePin") == 0)
                probeOut->vendor_page_activate_capture_pin_ok = true;
        }

        return true;
    }

    static bool try_property_page_set_objects(const char *label,
                                              const CLSID &clsid,
                                              IUnknown *target,
                                              DShowSignalProbeResult *probeOut)
    {
        if (!target)
            return false;

        ComPtr<IPropertyPage> page;
        HRESULT hr = CoCreateInstance(clsid, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&page));
        if (FAILED(hr) || !page)
        {
            dshow_probe_log(std::string(label) + " CoCreateInstance(IPropertyPage) failed hr=0x" + hr_to_hex(hr));
            return false;
        }

        dshow_probe_log(std::string(label) + " CoCreateInstance(IPropertyPage): OK");
        if (probeOut && clsid == DSHOW_SC0710_VENDOR_PAGE)
            probeOut->vendor_page_create_ok = true;

        IUnknown *objs[1] = {target};
        hr = page->SetObjects(1, objs);
        if (FAILED(hr))
        {
            dshow_probe_log(std::string(label) + " IPropertyPage::SetObjects failed hr=0x" + hr_to_hex(hr));
            return false;
        }

        dshow_probe_log(std::string(label) + " IPropertyPage::SetObjects: OK");
        page->SetObjects(0, NULL);
        return true;
    }

    static void log_clsid_registry_info(const CLSID &clsid, DShowSignalProbeResult *probeOut)
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
        log_module_probe_info(dll);

        if (probeOut && clsid == DSHOW_SC0710_VENDOR_PAGE)
        {
            probeOut->has_vendor_custom_page = true;
            if (!dll.empty())
                wcsncpy_s(probeOut->vendor_property_module, dll.c_str(), _TRUNCATE);
        }
    }



    static bool find_video_input_filter_by_index(int devIndex, ComPtr<IMoniker> &outMoniker, ComPtr<IBaseFilter> &outFilter)
    {
        outMoniker.Reset();
        outFilter.Reset();

        ComPtr<ICreateDevEnum> devEnum;
        HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&devEnum));
        if (FAILED(hr) || !devEnum)
        {
            dshow_probe_log("OleFrame CoCreateInstance(CLSID_SystemDeviceEnum) failed hr=0x" + hr_to_hex(hr));
            return false;
        }

        ComPtr<IEnumMoniker> enumMoniker;
        hr = devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &enumMoniker, 0);
        if (hr != S_OK || !enumMoniker)
        {
            dshow_probe_log("OleFrame CreateClassEnumerator(CLSID_VideoInputDeviceCategory) failed hr=0x" + hr_to_hex(hr));
            return false;
        }

        ULONG fetched = 0;
        ComPtr<IMoniker> moniker;
        int cur = 0;
        while (enumMoniker->Next(1, &moniker, &fetched) == S_OK)
        {
            if (cur == devIndex)
            {
                ComPtr<IBaseFilter> filter;
                hr = moniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&filter));
                if (FAILED(hr) || !filter)
                {
                    dshow_probe_log("OleFrame moniker->BindToObject(IBaseFilter) failed hr=0x" + hr_to_hex(hr));
                    return false;
                }
                outMoniker = moniker;
                outFilter = filter;
                return true;
            }
            moniker.Reset();
            ++cur;
        }
        dshow_probe_log("OleFrame target device index not found");
        return false;
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

    static void log_property_pages(const char *label, IUnknown *unk, bool &hasPages, DShowSignalProbeResult *probeOut)
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
        {
            const CLSID &pageClsid = cauuid.pElems[i];
            log_clsid_registry_info(pageClsid, probeOut);

            const bool setObjectsOk = try_property_page_set_objects(label, pageClsid, unk, probeOut);
            if (probeOut && pageClsid == DSHOW_SC0710_VENDOR_PAGE)
            {
                if (strcmp(label, "Filter") == 0)
                    probeOut->vendor_page_setobjects_filter_ok = setObjectsOk;
                else if (strcmp(label, "CapturePin") == 0)
                    probeOut->vendor_page_setobjects_capture_pin_ok = setObjectsOk;
            }

            if (setObjectsOk)
                try_property_page_activate_cycle(label, pageClsid, unk, probeOut);
        }

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
            {"VENDOR_CUSTOM_PAGE", DSHOW_SC0710_VENDOR_PAGE},
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
                log_property_pages("Filter", sourceFilter.Get(), out.filter_has_property_pages, &out);
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
                log_property_pages("CapturePin", capturePin.Get(), out.capture_pin_has_property_pages, &out);
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
        return "ARGB";
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
    if (sub == MFVideoFormat_RGB32)
        return "RGB32";
    if (sub == MFVideoFormat_MJPG)
        return "MJPG";

    return "Unknown";
}

namespace
{
    static std::wstring get_clsid_display_name(const CLSID &clsid)
    {
        const std::wstring clsidStr = guid_to_wstring(clsid);
        if (clsidStr.empty())
            return L"";
        return read_reg_default_string(HKEY_CLASSES_ROOT, L"CLSID\\" + clsidStr);
    }

    static bool collect_property_pages_for_target(IUnknown *unk, std::vector<GUID> &outPages)
    {
        outPages.clear();
        if (!unk)
            return false;

        ComPtr<ISpecifyPropertyPages> spp;
        HRESULT hr = unk->QueryInterface(IID_PPV_ARGS(&spp));
        if (FAILED(hr) || !spp)
            return false;

        CAUUID pages = {};
        hr = spp->GetPages(&pages);
        if (FAILED(hr) || !pages.pElems || pages.cElems == 0)
        {
            if (pages.pElems)
                CoTaskMemFree(pages.pElems);
            return false;
        }

        for (ULONG i = 0; i < pages.cElems; ++i)
            outPages.push_back(pages.pElems[i]);
        CoTaskMemFree(pages.pElems);
        return !outPages.empty();
    }

    static bool open_property_page_worker(int devIndex, const wchar_t *pageName, bool capturePin, const wchar_t *windowTitle)
    {
        if (!pageName || !*pageName)
            return false;

        const std::wstring wantedName(pageName);
        try
        {
            std::thread worker([devIndex, wantedName, capturePin, windowTitle]() {
                HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                const bool needUninit = SUCCEEDED(hr);
                if (hr != S_OK && hr != S_FALSE && hr != RPC_E_CHANGED_MODE)
                {
                    dshow_probe_log("OleFrame CoInitializeEx(APARTMENTTHREADED) failed hr=0x" + hr_to_hex(hr));
                    return;
                }

                ComPtr<IMoniker> moniker;
                ComPtr<IBaseFilter> filter;
                if (!find_video_input_filter_by_index(devIndex, moniker, filter) || !filter)
                {
                    dshow_probe_log("OleFrame failed to bind filter for device index=" + std::to_string(devIndex));
                    if (needUninit)
                        CoUninitialize();
                    return;
                }

                IUnknown *targetUnk = filter.Get();
                ComPtr<IGraphBuilder> graph;
                ComPtr<ICaptureGraphBuilder2> capBuilder;
                ComPtr<IPin> pin;
                if (capturePin)
                {
                    hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&graph));
                    if (SUCCEEDED(hr) && graph)
                    {
                        graph->AddFilter(filter.Get(), L"VideoCapture");
                        hr = CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&capBuilder));
                        if (SUCCEEDED(hr) && capBuilder)
                        {
                            capBuilder->SetFiltergraph(graph.Get());
                            pin.Attach(find_capture_pin(capBuilder.Get(), filter.Get()));
                        }
                    }
                    if (!pin)
                    {
                        dshow_probe_log("OleFrame capture pin not found for device index=" + std::to_string(devIndex));
                        if (needUninit)
                            CoUninitialize();
                        return;
                    }
                    targetUnk = pin.Get();
                }

                std::vector<GUID> pages;
                if (!collect_property_pages_for_target(targetUnk, pages))
                {
                    dshow_probe_log("OleFrame target GetPages failed or returned empty list");
                    if (needUninit)
                        CoUninitialize();
                    return;
                }

                GUID matched = GUID_NULL;
                for (const GUID &page : pages)
                {
                    const std::wstring displayName = get_clsid_display_name(page);
                    if (!displayName.empty() && _wcsicmp(displayName.c_str(), wantedName.c_str()) == 0)
                    {
                        matched = page;
                        break;
                    }
                }

                if (matched == GUID_NULL)
                {
                    dshow_probe_log("OleFrame target page name not found: " + wide_to_utf8(wantedName.c_str()));
                    if (needUninit)
                        CoUninitialize();
                    return;
                }

                GUID *pageBuf = (GUID *)CoTaskMemAlloc(sizeof(GUID));
                if (!pageBuf)
                {
                    dshow_probe_log("OleFrame CoTaskMemAlloc failed for selected page");
                    if (needUninit)
                        CoUninitialize();
                    return;
                }
                pageBuf[0] = matched;
                IUnknown *objects[1] = {targetUnk};

                dshow_probe_log("OleFrame launching OleCreatePropertyFrame for page: " + wide_to_utf8(wantedName.c_str()));
                hr = OleCreatePropertyFrame(nullptr,
                                            120,
                                            120,
                                            windowTitle,
                                            1,
                                            objects,
                                            1,
                                            pageBuf,
                                            GetUserDefaultLCID(),
                                            0,
                                            nullptr);
                if (SUCCEEDED(hr))
                    dshow_probe_log("OleFrame OleCreatePropertyFrame returned OK for page: " + wide_to_utf8(wantedName.c_str()));
                else
                    dshow_probe_log("OleFrame OleCreatePropertyFrame failed hr=0x" + hr_to_hex(hr) + " page=" + wide_to_utf8(wantedName.c_str()));

                CoTaskMemFree(pageBuf);
                if (needUninit)
                    CoUninitialize();
                dshow_probe_log("OleFrame thread exit");
            });

            worker.detach();
            dshow_probe_log("OleFrame launch accepted: worker thread detached");
            return true;
        }
        catch (...)
        {
            dshow_probe_log("OleFrame launch failed: exception while creating worker thread");
            return false;
        }
    }
}



static bool dshow_extract_video_cap(const AM_MEDIA_TYPE &mt, gcap_video_cap_t &out)
{
    DShowSignalProbeResult tmp{};
    if (!dshow_extract_media_type(mt, tmp))
        return false;

    out.width = tmp.width;
    out.height = tmp.height;
    out.fps_num = tmp.fps_num;
    out.fps_den = tmp.fps_den;
    out.pixfmt = gcap_subtype_to_pixfmt(tmp.subtype);
    out.bit_depth = gcap_pixfmt_bitdepth(out.pixfmt);
    return (out.width > 0 && out.height > 0);
}

static int copy_video_cap_out(const gcap_video_cap_t &src, gcap_video_cap_t *outCaps, int maxCaps, int written)
{
    if (outCaps && written < maxCaps)
        outCaps[written] = src;
    return written + 1;
}

static int copy_property_page_out(const std::wstring &name, bool capturePin, gcap_property_page_t *outPages, int maxPages, int written)
{
    if (outPages && written < maxPages)
    {
        auto &dst = outPages[written];
        ZeroMemory(&dst, sizeof(dst));
        WideCharToMultiByte(CP_UTF8, 0, name.c_str(), -1, dst.page_name, static_cast<int>(sizeof(dst.page_name)), nullptr, nullptr);
        dst.capture_pin = capturePin ? 1 : 0;
    }
    return written + 1;
}

bool dshow_open_vendor_property_page_by_index(int devIndex)
{
    // Optional vendor-specific debug helper. The current known page name happens to
    // be SC0710-specific, but this helper is intentionally kept out of the generic
    // capture flow.
    return open_property_page_worker(devIndex, L"SC0710 PCI, Custom Property Page", false, L"Vendor Property Page");
}

bool dshow_open_named_property_page_by_index(int devIndex, const wchar_t *pageName, bool capturePin)
{
    return open_property_page_worker(devIndex,
                                     pageName,
                                     capturePin,
                                     capturePin ? L"Capture Pin Property Page" : L"Filter Property Page");
}


int dshow_enum_video_caps_by_index(int devIndex, gcap_video_cap_t *outCaps, int maxCaps)
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool need_uninit = SUCCEEDED(hr);
    if (hr != S_OK && hr != S_FALSE && hr != RPC_E_CHANGED_MODE)
        return 0;

    int written = 0;
    do
    {
        ComPtr<IMoniker> moniker;
        ComPtr<IBaseFilter> sourceFilter;
        if (!find_video_input_filter_by_index(devIndex, moniker, sourceFilter) || !sourceFilter)
            break;

        ComPtr<IGraphBuilder> graph;
        hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&graph));
        if (FAILED(hr) || !graph)
            break;
        hr = graph->AddFilter(sourceFilter.Get(), L"VideoCapture");
        if (FAILED(hr))
            break;

        ComPtr<ICaptureGraphBuilder2> capBuilder;
        hr = CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&capBuilder));
        if (FAILED(hr) || !capBuilder)
            break;
        capBuilder->SetFiltergraph(graph.Get());

        ComPtr<IAMStreamConfig> cfg;
        hr = capBuilder->FindInterface(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, sourceFilter.Get(), IID_PPV_ARGS(&cfg));
        if (FAILED(hr) || !cfg)
            break;

        int count = 0;
        int capSize = 0;
        hr = cfg->GetNumberOfCapabilities(&count, &capSize);
        if (FAILED(hr) || count <= 0 || capSize <= 0)
            break;

        std::vector<unsigned char> caps(static_cast<size_t>(capSize));
        for (int i = 0; i < count; ++i)
        {
            AM_MEDIA_TYPE *capsMt = nullptr;
            if (FAILED(cfg->GetStreamCaps(i, &capsMt, caps.data())) || !capsMt)
                continue;

            gcap_video_cap_t cap{};
            if (dshow_extract_video_cap(*capsMt, cap))
                written = copy_video_cap_out(cap, outCaps, maxCaps, written);

            dshow_free_media_type(*capsMt);
            CoTaskMemFree(capsMt);
        }
    } while (false);

    if (need_uninit)
        CoUninitialize();
    return written;
}

int dshow_enum_property_pages_by_index(int devIndex, gcap_property_page_t *outPages, int maxPages)
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool need_uninit = SUCCEEDED(hr);
    if (hr != S_OK && hr != S_FALSE && hr != RPC_E_CHANGED_MODE)
        return 0;

    int written = 0;
    do
    {
        ComPtr<IMoniker> moniker;
        ComPtr<IBaseFilter> filter;
        if (!find_video_input_filter_by_index(devIndex, moniker, filter) || !filter)
            break;

        std::vector<GUID> filterPages;
        if (collect_property_pages_for_target(filter.Get(), filterPages))
        {
            for (const GUID &page : filterPages)
            {
                const std::wstring displayName = get_clsid_display_name(page);
                if (!displayName.empty())
                    written = copy_property_page_out(displayName, false, outPages, maxPages, written);
            }
        }

        ComPtr<IGraphBuilder> graph;
        hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&graph));
        if (FAILED(hr) || !graph)
            break;
        hr = graph->AddFilter(filter.Get(), L"VideoCapture");
        if (FAILED(hr))
            break;

        ComPtr<ICaptureGraphBuilder2> capBuilder;
        hr = CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&capBuilder));
        if (FAILED(hr) || !capBuilder)
            break;
        capBuilder->SetFiltergraph(graph.Get());

        ComPtr<IPin> pin;
        pin.Attach(find_capture_pin(capBuilder.Get(), filter.Get()));
        if (pin)
        {
            std::vector<GUID> pinPages;
            if (collect_property_pages_for_target(pin.Get(), pinPages))
            {
                for (const GUID &page : pinPages)
                {
                    const std::wstring displayName = get_clsid_display_name(page);
                    if (!displayName.empty())
                        written = copy_property_page_out(displayName, true, outPages, maxPages, written);
                }
            }
        }
    } while (false);

    if (need_uninit)
        CoUninitialize();
    return written;
}
