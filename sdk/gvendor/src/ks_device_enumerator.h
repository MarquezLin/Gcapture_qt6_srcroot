#pragma once

#ifdef _WIN32
#include <windows.h>
#include <string>
#include <vector>
#include "gdriver_abi.h"

namespace gvendor
{
    struct KsCaptureDevice
    {
        std::wstring interface_path;
        std::wstring friendly_name;
        gdriver_input_t inferred_input = GDRIVER_INPUT_UNKNOWN;
    };

    std::vector<KsCaptureDevice> enumerate_ks_capture_devices();
    bool device_matches_input(const KsCaptureDevice &device, gdriver_input_t input);
    bool device_is_preferred_capture_card(const KsCaptureDevice &device);
}
#endif
