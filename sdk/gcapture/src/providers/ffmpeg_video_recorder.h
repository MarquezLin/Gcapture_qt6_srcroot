#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

#include "gcapture.h"

inline int gcap_ffmpeg_recommended_bitrate_kbps(int width, int height, int fps_num, int fps_den, gcap_pixfmt_t input_format)
{
    const bool useHevc = input_format == GCAP_FMT_P010 || input_format == GCAP_FMT_Y210;
    if (width <= 0 || height <= 0)
        return useHevc ? 12000 : 8000;

    const int shortEdge = (std::min)(width, height);
    const double fps = (fps_den > 0) ? (static_cast<double>(fps_num) / static_cast<double>(fps_den)) : static_cast<double>(fps_num);
    const bool highFps = fps > 40.0;

    // H.264 values follow YouTube's SDR upload recommendations; HEVC uses a conservative lower table.
    if (shortEdge >= 4320)
        return useHevc ? (highFps ? 160000 : 110000) : (highFps ? 240000 : 160000);
    if (shortEdge >= 2160)
        return useHevc ? (highFps ? 45000 : 30000) : (highFps ? 68000 : 45000);
    if (shortEdge >= 1440)
        return useHevc ? (highFps ? 16000 : 11000) : (highFps ? 24000 : 16000);
    if (shortEdge >= 1080)
        return useHevc ? (highFps ? 9000 : 6000) : (highFps ? 12000 : 8000);
    if (shortEdge >= 720)
        return useHevc ? (highFps ? 5000 : 3500) : (highFps ? 7500 : 5000);
    return useHevc ? (highFps ? 3000 : 2000) : (highFps ? 4000 : 2500);
}

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
    //   NV12/YUY2/ARGB -> H.264 8-bit
    //   P010/Y210     -> HEVC when available
    //
    // Release builds are expected to use an LGPL shared FFmpeg build. Prefer
    // Intel Quick Sync, then Media Foundation, then an LGPL software encoder.
    bool force_hevc_main10 = false;
    bool force_h264 = false;
    bool audio_enabled = false;
    int audio_sample_rate = 0;
    int audio_channels = 0;
    int audio_bits_per_sample = 0;
    int audio_bitrate_kbps = 192;
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
    bool writeAudio(const uint8_t *data, size_t bytes, int64_t timeline_pts_ns = -1,
                    std::string *error = nullptr);
    void close();
    bool isOpen() const { return opened_; }

private:
    bool opened_ = false;
    FfmpegVideoRecordConfig cfg_{};

    struct Impl;
    Impl *impl_ = nullptr;
};
