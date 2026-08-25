#pragma once

#include <string>
#include <vector>

namespace gcap::audio
{
    struct device
    {
        std::string id;
        std::string name;
        int channels = 0;
        int sample_rate = 0;
        int bits_per_sample = 0;
        bool is_float = false;
        bool is_default = false;
    };

    std::vector<device> enumerate_devices();
    bool find_device_for_capture_name(const std::string &captureNameUtf8, device &out);
    bool start_preview(const char *deviceIdUtf8, std::string *error);
    bool start_dshow_preview(const char *videoDeviceNameUtf8, device &source, std::string *error);
    void stop_preview();
}
