#ifdef _WIN32

#include "gdriver_control_client.h"

#include <initguid.h>
#include <setupapi.h>

#include <sstream>
#include <vector>

#include "gdriver_control_codes.h"

namespace gcapture
{
    GDriverControlClient::~GDriverControlClient()
    {
        close();
    }

    bool GDriverControlClient::openDefault()
    {
        close();
        lastError_.clear();

        HDEVINFO devs = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_GDRIVER_CAPTURE, nullptr, nullptr,
                                             DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (devs == INVALID_HANDLE_VALUE)
            return fail("SetupDiGetClassDevs(GDRIVER_CAPTURE)");

        bool opened = false;
        for (DWORD index = 0; !opened; ++index)
        {
            SP_DEVICE_INTERFACE_DATA ifData = {};
            ifData.cbSize = sizeof(ifData);
            if (!SetupDiEnumDeviceInterfaces(devs, nullptr, &GUID_DEVINTERFACE_GDRIVER_CAPTURE, index, &ifData))
            {
                if (GetLastError() == ERROR_NO_MORE_ITEMS)
                    fail("enumerate GDRIVER_CAPTURE interface", ERROR_NOT_FOUND);
                else
                    fail("SetupDiEnumDeviceInterfaces(GDRIVER_CAPTURE)");
                break;
            }

            DWORD needed = 0;
            SetupDiGetDeviceInterfaceDetailW(devs, &ifData, nullptr, 0, &needed, nullptr);
            if (needed == 0)
                continue;

            std::vector<BYTE> detailBuf(needed, 0);
            auto *detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(detailBuf.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

            if (!SetupDiGetDeviceInterfaceDetailW(devs, &ifData, detail, needed, nullptr, nullptr))
            {
                fail("SetupDiGetDeviceInterfaceDetail(GDRIVER_CAPTURE)");
                continue;
            }

            HANDLE h = CreateFileW(detail->DevicePath,
                                   GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr,
                                   OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL,
                                   nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                fail("CreateFile(GDRIVER_CAPTURE)");
                continue;
            }

            handle_ = h;
            lastError_.clear();
            opened = true;
        }

        SetupDiDestroyDeviceInfoList(devs);
        return opened;
    }

    void GDriverControlClient::close()
    {
        if (handle_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

    bool GDriverControlClient::isOpen() const
    {
        return handle_ != INVALID_HANDLE_VALUE;
    }

    bool GDriverControlClient::getDeviceInfo(gdriver_device_info_t &out)
    {
        ZeroMemory(&out, sizeof(out));
        out.struct_size = sizeof(out);
        out.abi_version = GDRIVER_ABI_VERSION;
        return deviceIoControl(IOCTL_GDRIVER_GET_DEVICE_INFO, nullptr, 0, &out, sizeof(out));
    }

    bool GDriverControlClient::getSignalStatus(gdriver_signal_status_t &out)
    {
        ZeroMemory(&out, sizeof(out));
        out.struct_size = sizeof(out);
        out.abi_version = GDRIVER_ABI_VERSION;
        return deviceIoControl(IOCTL_GDRIVER_GET_SIGNAL_STATUS, nullptr, 0, &out, sizeof(out));
    }

    bool GDriverControlClient::fail(const char *operation, DWORD winerr)
    {
        std::ostringstream oss;
        oss << operation << " failed winerr=" << winerr;
        lastError_ = oss.str();
        return false;
    }

    bool GDriverControlClient::deviceIoControl(DWORD code, void *inBuffer, DWORD inSize, void *outBuffer, DWORD outSize)
    {
        if (!isOpen())
            return fail("DeviceIoControl without open handle", ERROR_INVALID_HANDLE);

        DWORD bytesReturned = 0;
        if (!DeviceIoControl(handle_, code, inBuffer, inSize, outBuffer, outSize, &bytesReturned, nullptr))
            return fail("DeviceIoControl");
        return true;
    }
}

#endif
