#pragma once

#include "audio_manager.h"

#include <memory>
#include <string>

namespace gcap::audio
{
    class dshow_monitor
    {
    public:
        dshow_monitor();
        ~dshow_monitor();

        dshow_monitor(const dshow_monitor &) = delete;
        dshow_monitor &operator=(const dshow_monitor &) = delete;

        bool start(const char *videoDeviceNameUtf8, device &source, std::string *error);
        void stop();

    private:
        struct impl;
        std::unique_ptr<impl> impl_;
    };
}
