#pragma once

/* Customer-facing GXDMA capture API. Do not include internal gvendor/gdriver headers in applications. */

#include <stdint.h>

#ifdef _WIN32
#ifdef GXDMA_BUILD
#define GXDMA_API __declspec(dllexport)
#else
#define GXDMA_API __declspec(dllimport)
#endif
#else
#define GXDMA_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum
{
    GXDMA_MAX_DEVICES = 16
};

typedef enum
{
    GXDMA_OK = 0,
    GXDMA_EINVAL = -1,
    GXDMA_ENODEV = -2,
    GXDMA_ESTATE = -3,
    GXDMA_EIO = -4,
    GXDMA_ENOTSUP = -5,
    GXDMA_ETIMEOUT = -6
} gxdma_status_t;

typedef enum
{
    GXDMA_PREVIEW_BITDEPTH_AUTO = 0,
    GXDMA_PREVIEW_BITDEPTH_10BIT = 10,
    GXDMA_PREVIEW_BITDEPTH_8BIT = 8
} gxdma_preview_bitdepth_t;

typedef struct
{
    int index;
    char name[128];
} gxdma_device_info_t;

typedef struct
{
    void *hwnd;
    int enable_preview;
    int swapchain_bitdepth;
} gxdma_preview_desc_t;

typedef struct
{
    int width;
    int height;
    double fps;
    int bit_depth;
    char pixel_format[32];
} gxdma_signal_status_t;

typedef struct
{
    gxdma_signal_status_t input_signal;
    double capture_fps;
    uint64_t delivered_frames;
} gxdma_runtime_info_t;

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
} gxdma_preview_info_t;

typedef struct
{
    const void *data;
    int stride;
    int width;
    int height;
    uint64_t pts_ns;
    uint64_t frame_id;
} gxdma_frame_t;

typedef struct gxdma_handle_t *gxdma_handle;

typedef void (*gxdma_on_frame_cb)(const gxdma_frame_t *frame, void *user);
typedef void (*gxdma_on_error_cb)(gxdma_status_t status, const char *message, void *user);

/* max_devices is clamped to GXDMA_MAX_DEVICES. Pass NULL or 0 to query the device count only. */
GXDMA_API int gxdma_enumerate_devices(gxdma_device_info_t *out_devices, int max_devices);
GXDMA_API gxdma_status_t gxdma_create(gxdma_handle *out_handle);
GXDMA_API gxdma_status_t gxdma_destroy(gxdma_handle handle);
GXDMA_API gxdma_status_t gxdma_set_callbacks(gxdma_handle handle,
                                             gxdma_on_frame_cb on_frame,
                                             gxdma_on_error_cb on_error,
                                             void *user);
GXDMA_API gxdma_status_t gxdma_set_preview(gxdma_handle handle, const gxdma_preview_desc_t *desc);
GXDMA_API gxdma_status_t gxdma_open(gxdma_handle handle, int device_index);
GXDMA_API gxdma_status_t gxdma_start(gxdma_handle handle);
GXDMA_API gxdma_status_t gxdma_stop(gxdma_handle handle);
GXDMA_API gxdma_status_t gxdma_get_signal_status(gxdma_handle handle, gxdma_signal_status_t *out_status);
GXDMA_API gxdma_status_t gxdma_get_runtime_info(gxdma_handle handle, gxdma_runtime_info_t *out_info);
GXDMA_API gxdma_status_t gxdma_get_preview_info(gxdma_handle handle, gxdma_preview_info_t *out_info);
GXDMA_API const char *gxdma_strerror(gxdma_status_t status);

#ifdef __cplusplus
}
#endif
