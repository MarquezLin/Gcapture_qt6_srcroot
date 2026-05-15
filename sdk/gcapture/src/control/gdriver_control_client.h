#pragma once

#ifdef _WIN32

#include <windows.h>
#include <string>

#include "gdriver_abi.h"
#include "gcapture.h"

namespace gcapture
{
    class GDriverControlClient
    {
    public:
        GDriverControlClient() = default;
        ~GDriverControlClient();

        GDriverControlClient(const GDriverControlClient &) = delete;
        GDriverControlClient &operator=(const GDriverControlClient &) = delete;

        bool openDefault();
        void close();
        bool isOpen() const;

        bool getDeviceInfo(gdriver_device_info_t &out);
        bool getSignalStatus(gdriver_signal_status_t &out);

        const std::string &lastError() const { return lastError_; }

    private:
        bool fail(const char *operation, DWORD winerr = GetLastError());
        bool deviceIoControl(DWORD code, void *inBuffer, DWORD inSize, void *outBuffer, DWORD outSize);

        HANDLE handle_ = INVALID_HANDLE_VALUE;
        std::string lastError_;
    };
}

#endif
