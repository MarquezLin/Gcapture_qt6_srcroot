#pragma once

#include <cstdint>
#include <string>

#include "gcapture.h"

struct FfmpegVideoRecordConfig
{
    std::string path;
    int width = 0;
    int height = 0;
    int fps_num = 30;
    int fps_den = 1;
    int bitrate_kbps = 8000;
    gcap_pixfmt_t input_format = GCAP_FMT_NV12;

    // Auto-selected by input_format:
    //   NV12/YUY2/ARGB -> H.264 8-bit yuv420p
    //   P010/Y210     -> HEVC Main10 yuv420p10le
    bool force_hevc_main10 = false;
};

struct FfmpegVideoFrameView
{
    gcap_pixfmt_t format = GCAP_FMT_NV12;
    int width = 0;
    int height = 0;
    const uint8_t *data[4] = {};
    int stride[4] = {};
    int64_t pts = 0; // in encoder time_base units
};

class FfmpegVideoRecorder
{
public:
    FfmpegVideoRecorder();
    ~FfmpegVideoRecorder();

    bool open(const FfmpegVideoRecordConfig &cfg, std::string *error = nullptr);
    bool writeFrame(const FfmpegVideoFrameView &frame, std::string *error = nullptr);
    void close();
    bool isOpen() const { return opened_; }

private:
    bool opened_ = false;
    FfmpegVideoRecordConfig cfg_{};

    struct Impl;
    Impl *impl_ = nullptr;
};
