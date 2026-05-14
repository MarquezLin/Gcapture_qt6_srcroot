#ifdef _WIN32
#include "ks_device_enumerator.h"

#include <setupapi.h>
#include <ks.h>
#include <algorithm>
#include <cwctype>

namespace
{
    static std::wstring lower_copy(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        return value;
    }

    static std::wstring query_device_string(HDEVINFO devs, SP_DEVINFO_DATA &devInfo, DWORD property)
    {
        DWORD type = 0;
        DWORD needed = 0;
        SetupDiGetDeviceRegistryPropertyW(devs, &devInfo, property, &type, nullptr, 0, &needed);
        if (needed == 0)
            return std::wstring();

        std::vector<BYTE> buf(needed + sizeof(wchar_t), 0);
        if (!SetupDiGetDeviceRegistryPropertyW(devs, &devInfo, property, &type, buf.data(),
                                               static_cast<DWORD>(buf.size()), &needed))
            return std::wstring();
        if (type != REG_SZ && type != REG_MULTI_SZ)
            return std::wstring();
        return std::wstring(reinterpret_cast<const wchar_t *>(buf.data()));
    }

    static gdriver_input_t infer_input_from_name(const std::wstring &friendly)
    {
        const std::wstring lower = lower_copy(friendly);
        if (lower.find(L"hdmi") != std::wstring::npos)
            return GDRIVER_INPUT_HDMI;
        if (lower.find(L"sdi") != std::wstring::npos)
            return GDRIVER_INPUT_SDI;
        return GDRIVER_INPUT_UNKNOWN;
    }

    static bool looks_like_audio_device(const std::wstring &friendly)
    {
        const std::wstring lower = lower_copy(friendly);
        return lower.find(L"audio") != std::wstring::npos ||
               lower.find(L"音訊") != std::wstring::npos ||
               lower.find(L"音频") != std::wstring::npos;
    }
    static bool looks_like_virtual_test_device(const std::wstring &friendly)
    {
        return lower_copy(friendly).find(L"vision") != std::wstring::npos;
    }
}

namespace gvendor
{
    std::vector<KsCaptureDevice> enumerate_ks_capture_devices()
    {
        std::vector<KsCaptureDevice> result;

        HDEVINFO devs = SetupDiGetClassDevsW(&KSCATEGORY_CAPTURE, nullptr, nullptr,
                                             DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (devs == INVALID_HANDLE_VALUE)
            return result;

        for (DWORD index = 0;; ++index)
        {
            SP_DEVICE_INTERFACE_DATA ifData = {};
            ifData.cbSize = sizeof(ifData);
            if (!SetupDiEnumDeviceInterfaces(devs, nullptr, &KSCATEGORY_CAPTURE, index, &ifData))
            {
                if (GetLastError() == ERROR_NO_MORE_ITEMS)
                    break;
                continue;
            }

            DWORD needed = 0;
            SetupDiGetDeviceInterfaceDetailW(devs, &ifData, nullptr, 0, &needed, nullptr);
            if (needed == 0)
                continue;

            std::vector<BYTE> detailBuf(needed, 0);
            auto *detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(detailBuf.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

            SP_DEVINFO_DATA devInfo = {};
            devInfo.cbSize = sizeof(devInfo);
            if (!SetupDiGetDeviceInterfaceDetailW(devs, &ifData, detail, needed, nullptr, &devInfo))
                continue;

            KsCaptureDevice device;
            device.interface_path = detail->DevicePath;
            device.friendly_name = query_device_string(devs, devInfo, SPDRP_FRIENDLYNAME);
            if (device.friendly_name.empty())
                device.friendly_name = query_device_string(devs, devInfo, SPDRP_DEVICEDESC);
            if (looks_like_audio_device(device.friendly_name))
                continue;
            if (looks_like_virtual_test_device(device.friendly_name))
                continue;
            device.inferred_input = infer_input_from_name(device.friendly_name);
            result.push_back(device);
        }

        SetupDiDestroyDeviceInfoList(devs);
        return result;
    }

    bool device_matches_input(const KsCaptureDevice &device, gdriver_input_t input)
    {
        if (input == GDRIVER_INPUT_UNKNOWN)
            return true;
        return device.inferred_input == input;
    }

    bool device_is_preferred_capture_card(const KsCaptureDevice &device)
    {
        const std::wstring lower = lower_copy(device.friendly_name);
        if (lower.find(L"capture card") != std::wstring::npos)
            return true;
        return lower.find(L"gigabyte") != std::wstring::npos &&
               lower.find(L"capture") != std::wstring::npos;
    }
}
#endif
