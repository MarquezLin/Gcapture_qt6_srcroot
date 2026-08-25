#pragma once

#include "audio_manager.h"

#include <memory>
#include <string>

namespace gcap::audio
{
    class dshow_preview
    {
    public:
        dshow_preview();
        ~dshow_preview();

        dshow_preview(const dshow_preview &) = delete;
        dshow_preview &operator=(const dshow_preview &) = delete;

        bool start(const char *videoDeviceNameUtf8, device &source, std::string *error);
        void stop();

    private:
        struct impl;
        std::unique_ptr<impl> impl_;
    };
}
