#ifdef _WIN32
#include "gvendor.h"
#include "xdma_capture_session.h"

#include <new>
#include <vector>

namespace
{
    static void copy_wide_to_utf8(const std::wstring &src, char *dst, size_t dstSize)
    {
        if (!dst || dstSize == 0)
            return;
        dst[0] = '\0';
        if (src.empty())
            return;
        WideCharToMultiByte(CP_UTF8, 0, src.c_str(), -1, dst, static_cast<int>(dstSize), nullptr, nullptr);
        dst[dstSize - 1] = '\0';
    }
}

struct gv_handle_t
{
    gvendor::XdmaCaptureSession session;
};

extern "C"
{
    GVENDOR_API int gv_enumerate_devices(gv_device_entry_t *out, int max_devices)
    {
        const std::vector<gvendor::XdmaDevice> devices = gvendor::enumerate_xdma_devices();
        if (!out || max_devices <= 0)
            return static_cast<int>(devices.size());

        const int n = max_devices < static_cast<int>(devices.size()) ? max_devices : static_cast<int>(devices.size());
        for (int i = 0; i < n; ++i)
        {
            const auto &device = devices[static_cast<size_t>(i)];
            gv_device_entry_t entry = {};
            copy_wide_to_utf8(device.friendly_name, entry.friendly_name, sizeof(entry.friendly_name));
            copy_wide_to_utf8(device.interface_path, entry.device_path, sizeof(entry.device_path));
            entry.inferred_input = GDRIVER_INPUT_SDI;
            out[i] = entry;
        }
        return n;
    }

    GVENDOR_API gv_status_t gv_open_default(gv_handle *out)
    {
        if (!out)
            return GV_EINVAL;
        *out = nullptr;

        gv_handle handle = new (std::nothrow) gv_handle_t();
        if (!handle)
            return GV_EIO;

        const gv_status_t st = handle->session.open_default();
        if (st != GV_OK)
        {
            delete handle;
            return st;
        }

        *out = handle;
        return GV_OK;
    }

    GVENDOR_API gv_status_t gv_open_device_index(int device_index, gv_handle *out)
    {
        if (!out || device_index < 0)
            return GV_EINVAL;
        *out = nullptr;

        const std::vector<gvendor::XdmaDevice> devices = gvendor::enumerate_xdma_devices();
        const size_t index = static_cast<size_t>(device_index);
        if (index >= devices.size())
            return GV_ENODEV;

        gv_handle handle = new (std::nothrow) gv_handle_t();
        if (!handle)
            return GV_EIO;

        const gv_status_t st = handle->session.open_device_index(index);
        if (st != GV_OK)
        {
            delete handle;
            return st;
        }

        *out = handle;
        return GV_OK;
    }

    GVENDOR_API gv_status_t gv_close(gv_handle h)
    {
        if (!h)
            return GV_EINVAL;
        const gv_status_t st = h->session.close();
        delete h;
        return st;
    }

    GVENDOR_API gv_status_t gv_get_device_info(gv_handle h, gv_device_info_t *out)
    {
        if (!h || !out)
            return GV_EINVAL;
        return h->session.get_device_info(*out);
    }

    GVENDOR_API gv_status_t gv_get_signal_status(gv_handle h, gv_signal_status_t *out)
    {
        if (!h || !out)
            return GV_EINVAL;
        return h->session.get_signal_status(*out);
    }

    GVENDOR_API gv_status_t gv_get_stream_stats(gv_handle h, gv_stream_stats_t *out)
    {
        if (!h || !out)
            return GV_EINVAL;
        return h->session.get_stream_stats(*out);
    }

    GVENDOR_API gv_status_t gv_set_input(gv_handle h, gdriver_input_t input, uint32_t channel_index)
    {
        if (!h)
            return GV_EINVAL;
        return h->session.set_input(input, channel_index);
    }

    GVENDOR_API gv_status_t gv_configure_stream(gv_handle h, const gv_stream_desc_t *desc)
    {
        if (!h || !desc)
            return GV_EINVAL;
        return h->session.configure_stream(*desc);
    }

    GVENDOR_API gv_status_t gv_start_stream(gv_handle h)
    {
        if (!h)
            return GV_EINVAL;
        return h->session.start_stream();
    }

    GVENDOR_API gv_status_t gv_stop_stream(gv_handle h)
    {
        if (!h)
            return GV_EINVAL;
        return h->session.stop_stream();
    }

    GVENDOR_API gv_status_t gv_wait_frame(gv_handle h, uint32_t timeout_ms, gv_frame_t *out)
    {
        if (!h || !out)
            return GV_EINVAL;
        return h->session.wait_frame(timeout_ms, *out);
    }

    GVENDOR_API gv_status_t gv_release_frame(gv_handle h, const gv_frame_t *frame)
    {
        if (!h || !frame)
            return GV_EINVAL;
        return h->session.release_frame(*frame);
    }

    GVENDOR_API const char *gv_strerror(gv_status_t status)
    {
        switch (status)
        {
        case GV_OK:
            return "ok";
        case GV_EINVAL:
            return "invalid argument";
        case GV_ENODEV:
            return "device not found";
        case GV_ESTATE:
            return "invalid state";
        case GV_ENOTSUP:
            return "not supported";
        case GV_ETIMEOUT:
            return "timeout";
        case GV_EIO:
            return "i/o error";
        case GV_EABI:
            return "abi mismatch";
        default:
            return "unknown";
        }
    }

    GVENDOR_API const char *gv_last_error(gv_handle h)
    {
        if (!h)
            return "";
        return h->session.last_error();
    }
}
#endif
