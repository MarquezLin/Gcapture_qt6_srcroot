#include <cstdio>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "gvendor.h"

namespace
{
    static gdriver_input_t parse_input(int argc, char **argv)
    {
        if (argc < 2 || !argv[1])
            return GDRIVER_INPUT_HDMI;
        if (std::strcmp(argv[1], "sdi") == 0 || std::strcmp(argv[1], "SDI") == 0)
            return GDRIVER_INPUT_SDI;
        return GDRIVER_INPUT_HDMI;
    }

    static bool is_list_mode(int argc, char **argv)
    {
        return argc >= 2 && argv[1] && std::strcmp(argv[1], "list") == 0;
    }

    static bool has_arg(int argc, char **argv, const char *name)
    {
        for (int i = 1; i < argc; ++i)
        {
            if (argv[i] && std::strcmp(argv[i], name) == 0)
                return true;
        }
        return false;
    }

    static bool has_token(int argc, char **argv, const char *token)
    {
        for (int i = 1; i < argc; ++i)
        {
            if (argv[i] && std::strstr(argv[i], token))
                return true;
        }
        return false;
    }

    static int parse_device_index(int argc, char **argv)
    {
        for (int i = 1; i + 1 < argc; ++i)
        {
            if (argv[i] && std::strcmp(argv[i], "--device") == 0 && argv[i + 1])
                return std::atoi(argv[i + 1]);
        }
        return -1;
    }

    static int parse_int_arg(int argc, char **argv, const char *name, int fallback)
    {
        for (int i = 1; i + 1 < argc; ++i)
        {
            if (argv[i] && std::strcmp(argv[i], name) == 0 && argv[i + 1])
                return std::atoi(argv[i + 1]);
        }
        return fallback;
    }

    static bool has_exact_or_token(int argc, char **argv, const char *name)
    {
        return has_arg(argc, argv, name) || has_token(argc, argv, name);
    }

    static const char *pixfmt_name(gdriver_pixel_format_t fmt)
    {
        switch (fmt)
        {
        case GDRIVER_PIXFMT_YUY2:
            return "YUY2";
        case GDRIVER_PIXFMT_UYVY:
            return "UYVY";
        case GDRIVER_PIXFMT_RGB24:
            return "RGB24";
        case GDRIVER_PIXFMT_Y210:
            return "Y210";
        default:
            return "UNKNOWN";
        }
    }
}

int main(int argc, char **argv)
{
    std::printf("args:");
    for (int i = 1; i < argc; ++i)
        std::printf(" [%s]", argv[i] ? argv[i] : "");
    std::printf("\n");

    if (is_list_mode(argc, argv))
    {
        const int total = gv_enumerate_devices(nullptr, 0);
        std::printf("devices: %d\n", total);
        if (total <= 0)
            return 0;
        std::vector<gv_device_entry_t> devices(static_cast<size_t>(total));
        const int written = gv_enumerate_devices(devices.data(), total);
        for (int i = 0; i < written; ++i)
        {
            std::printf("[%d] %s input=%u\n    %s\n",
                        i,
                        devices[static_cast<size_t>(i)].friendly_name[0] ? devices[static_cast<size_t>(i)].friendly_name : "(unknown)",
                        static_cast<unsigned>(devices[static_cast<size_t>(i)].inferred_input),
                        devices[static_cast<size_t>(i)].device_path);
        }
        return 0;
    }

    gv_handle h = nullptr;
    const int deviceIndex = parse_device_index(argc, argv);
    gv_status_t st = deviceIndex >= 0 ? gv_open_device_index(deviceIndex, &h) : gv_open_default(&h);
    if (st != GV_OK)
    {
        std::printf("gv_open failed: %s\n", gv_strerror(st));
        return 1;
    }

    const gdriver_input_t input = parse_input(argc, argv);
    st = gv_set_input(h, input, 0);
    if (st != GV_OK)
    {
        std::printf("gv_set_input failed: %s (%s)\n", gv_strerror(st), gv_last_error(h));
        gv_close(h);
        return 2;
    }

    gv_device_info_t info = {};
    st = gv_get_device_info(h, &info);
    if (st == GV_OK)
        std::printf("device: %s\n", info.friendly_name[0] ? info.friendly_name : "(unknown)");

    gv_stream_desc_t desc = {};
    desc.channel_index = 0;
    desc.input = input;
    desc.width = has_exact_or_token(argc, argv, "720p") ? 1280u : 1920u;
    desc.height = has_exact_or_token(argc, argv, "720p") ? 720u : 1080u;
    desc.fps_num = 30000;
    desc.fps_den = 1001;
    desc.pixel_format = (input == GDRIVER_INPUT_HDMI) ? GDRIVER_PIXFMT_RGB24 : GDRIVER_PIXFMT_YUY2;
    desc.buffer_count = 1;
    desc.memory_kind = GDRIVER_MEMORY_DRIVER_COPY;

    st = gv_configure_stream(h, &desc);
    if (st != GV_OK)
    {
        std::printf("gv_configure_stream failed: %s (%s)\n", gv_strerror(st), gv_last_error(h));
        gv_close(h);
        return 3;
    }
    std::printf("configure ok\n");

    gv_signal_status_t signal = {};
    st = gv_get_signal_status(h, &signal);
    if (st == GV_OK)
    {
        std::printf("configured stream: %ux%u fps=%u/%u fmt=%s(%u) input=%u locked=%d\n",
                    signal.width,
                    signal.height,
                    signal.fps_num,
                    signal.fps_den,
                    pixfmt_name(signal.pixel_format),
                    static_cast<unsigned>(signal.pixel_format),
                    static_cast<unsigned>(signal.input),
                    signal.signal_locked);
    }

    st = gv_start_stream(h);
    if (st != GV_OK)
    {
        std::printf("gv_start_stream failed: %s (%s)\n", gv_strerror(st), gv_last_error(h));
        gv_close(h);
        return 4;
    }
    std::printf("start ok\n");

    if (has_arg(argc, argv, "--no-frame") || has_arg(argc, argv, "no-frame") || has_token(argc, argv, "no-frame"))
    {
        std::printf("no-frame mode: stop after start\n");
        gv_stop_stream(h);
        gv_close(h);
        return 0;
    }

    const int frameCount = (std::max)(1, parse_int_arg(argc, argv, "--frames", 1));
    std::printf("waiting frame%s... count=%d\n", frameCount > 1 ? "s" : "", frameCount);

    int captured = 0;
    size_t totalBytes = 0;
    const auto startedAt = std::chrono::steady_clock::now();
    for (int i = 0; i < frameCount; ++i)
    {
        gv_frame_t frame = {};
        st = gv_wait_frame(h, 5000, &frame);
        if (st != GV_OK)
        {
            std::printf("gv_wait_frame failed at %d/%d: %s (%s)\n",
                        i + 1,
                        frameCount,
                        gv_strerror(st),
                        gv_last_error(h));
            gv_stop_stream(h);
            gv_close(h);
            return 5;
        }

        ++captured;
        totalBytes += frame.data_size_bytes;
        if (i == 0 || i + 1 == frameCount || ((i + 1) % 30) == 0)
        {
            std::printf("frame ok: %d/%d id=%llu bytes=%zu %ux%u fmt=%s(%u) stride=%u\n",
                        i + 1,
                        frameCount,
                        static_cast<unsigned long long>(frame.frame_id),
                        frame.data_size_bytes,
                        frame.width,
                        frame.height,
                        pixfmt_name(frame.pixel_format),
                        static_cast<unsigned>(frame.pixel_format),
                        frame.plane_stride_bytes[0]);
        }

        gv_release_frame(h, &frame);
    }

    const auto endedAt = std::chrono::steady_clock::now();
    const double elapsedSec = std::chrono::duration<double>(endedAt - startedAt).count();
    if (captured > 1 && elapsedSec > 0.0)
    {
        std::printf("capture summary: frames=%d elapsed=%.3fs fps=%.2f total_bytes=%zu\n",
                    captured,
                    elapsedSec,
                    static_cast<double>(captured) / elapsedSec,
                    totalBytes);
    }

    gv_stop_stream(h);
    gv_close(h);
    return 0;
}
