#pragma once

/* Customer-facing GVFG capture API. Do not include internal gvendor/gdriver headers in applications. */

#include <stdint.h>

#ifdef _WIN32
#ifdef GVFG_BUILD
#define GVFG_API __declspec(dllexport)
#else
#define GVFG_API __declspec(dllimport)
#endif
#else
#define GVFG_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum
{
    GVFG_MAX_DEVICES = 16
};

typedef enum
{
    GVFG_OK = 0,
    GVFG_EINVAL = -1,
    GVFG_ENODEV = -2,
    GVFG_ESTATE = -3,
    GVFG_EIO = -4,
    GVFG_ENOTSUP = -5,
    GVFG_ETIMEOUT = -6
} gvfg_status_t;

typedef enum
{
    GVFG_PREVIEW_BITDEPTH_AUTO = 0,
    GVFG_PREVIEW_BITDEPTH_10BIT = 10,
    GVFG_PREVIEW_BITDEPTH_8BIT = 8
} gvfg_preview_bitdepth_t;

typedef struct
{
    int index;
    char name[128];
} gvfg_device_info_t;

typedef struct
{
    void *hwnd;
    int enable_preview;
    int swapchain_bitdepth;
} gvfg_preview_desc_t;

typedef struct
{
    int width;
    int height;
    double fps;
    int bit_depth;
    char pixel_format[32];
} gvfg_signal_status_t;

typedef struct
{
    gvfg_signal_status_t input_signal;
    double capture_fps;
    uint64_t delivered_frames;
} gvfg_runtime_info_t;

typedef struct
{
    int enabled;
    int active;
    int width;
    int height;
    int swapchain_bitdepth;
    int swapchain_10bit;
    char render_path[128];
    char backbuffer_format[64];
} gvfg_preview_info_t;

typedef struct
{
    const void *data;
    int stride;
    int width;
    int height;
    uint64_t pts_ns;
    uint64_t frame_id;
} gvfg_frame_t;

typedef struct gvfg_handle_t *gvfg_handle;

typedef void (*gvfg_on_frame_cb)(const gvfg_frame_t *frame, void *user);
typedef void (*gvfg_on_error_cb)(gvfg_status_t status, const char *message, void *user);

/* max_devices is clamped to GVFG_MAX_DEVICES. Pass NULL or 0 to query the device count only. */
GVFG_API int gvfg_enumerate_devices(gvfg_device_info_t *out_devices, int max_devices);
GVFG_API gvfg_status_t gvfg_create(gvfg_handle *out_handle);
GVFG_API gvfg_status_t gvfg_destroy(gvfg_handle handle);
GVFG_API gvfg_status_t gvfg_set_callbacks(gvfg_handle handle,
                                             gvfg_on_frame_cb on_frame,
                                             gvfg_on_error_cb on_error,
                                             void *user);
GVFG_API gvfg_status_t gvfg_set_preview(gvfg_handle handle, const gvfg_preview_desc_t *desc);
GVFG_API gvfg_status_t gvfg_open(gvfg_handle handle, int device_index);
GVFG_API gvfg_status_t gvfg_start(gvfg_handle handle);
GVFG_API gvfg_status_t gvfg_stop(gvfg_handle handle);
GVFG_API gvfg_status_t gvfg_get_signal_status(gvfg_handle handle, gvfg_signal_status_t *out_status);
GVFG_API gvfg_status_t gvfg_get_runtime_info(gvfg_handle handle, gvfg_runtime_info_t *out_info);
GVFG_API gvfg_status_t gvfg_get_preview_info(gvfg_handle handle, gvfg_preview_info_t *out_info);
GVFG_API const char *gvfg_strerror(gvfg_status_t status);

#ifdef __cplusplus
}
#endif
