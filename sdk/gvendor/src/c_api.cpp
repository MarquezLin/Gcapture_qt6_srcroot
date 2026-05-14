#ifdef _WIN32
#include "gvendor.h"
#include "ks_capture_session.h"
#include "ks_device_enumerator.h"

#include <new>
#include <vector>

struct gv_handle_t
{
    gvendor::KsCaptureSession session;
};

extern "C"
{
    GVENDOR_API int gv_enumerate_devices(gv_device_entry_t *out, int max_devices)
    {
        const std::vector<gvendor::KsCaptureDevice> devices = gvendor::enumerate_ks_capture_devices();
        if (!out || max_devices <= 0)
            return static_cast<int>(devices.size());

        const int n = (std::min)(max_devices, static_cast<int>(devices.size()));
        for (int i = 0; i < n; ++i)
        {
            std::memset(&out[i], 0, sizeof(out[i]));
            WideCharToMultiByte(CP_UTF8, 0, devices[static_cast<size_t>(i)].friendly_name.c_str(), -1,
                                out[i].friendly_name, static_cast<int>(sizeof(out[i].friendly_name)), nullptr, nullptr);
            WideCharToMultiByte(CP_UTF8, 0, devices[static_cast<size_t>(i)].interface_path.c_str(), -1,
                                out[i].device_path, static_cast<int>(sizeof(out[i].device_path)), nullptr, nullptr);
            out[i].inferred_input = devices[static_cast<size_t>(i)].inferred_input;
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

        gv_handle handle = new (std::nothrow) gv_handle_t();
        if (!handle)
            return GV_EIO;

        const gv_status_t st = handle->session.open_device_index(static_cast<size_t>(device_index));
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
