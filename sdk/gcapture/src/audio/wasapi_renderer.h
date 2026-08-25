#pragma once

#include <windows.h>
#include <mmreg.h>

#include <cstdint>
#include <atomic>
#include <memory>
#include <string>

namespace gcap::audio
{
    class wasapi_renderer
    {
    public:
        wasapi_renderer();
        ~wasapi_renderer();

        wasapi_renderer(const wasapi_renderer &) = delete;
        wasapi_renderer &operator=(const wasapi_renderer &) = delete;

        bool start(const WAVEFORMATEX *sourceFormat, std::string *error);
        bool write(const uint8_t *sourceData, uint32_t sourceFrames, uint32_t sourceFlags,
                   const std::atomic<bool> &keepRunning, std::string *error);
        void stop();

        uint32_t sample_rate() const;
        uint16_t channels() const;

    private:
        struct impl;
        std::unique_ptr<impl> impl_;
    };
}
