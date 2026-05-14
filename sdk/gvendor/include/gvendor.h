#pragma once

#include <stdint.h>
#include <stddef.h>
#include "gdriver_abi.h"

#ifdef _WIN32
#ifdef GVENDOR_BUILD
#define GVENDOR_API __declspec(dllexport)
#else
#define GVENDOR_API __declspec(dllimport)
#endif
#else
#define GVENDOR_API
#endif

#define GVENDOR_MAX_DEVICE_NAME 128
#define GVENDOR_MAX_DEVICE_PATH 512

#ifdef __cplusplus
extern "C"
{
#endif

typedef enum gv_status_t
{
    GV_OK = 0,
    GV_EINVAL,
    GV_ENODEV,
    GV_ESTATE,
    GV_ENOTSUP,
    GV_ETIMEOUT,
    GV_EIO,
    GV_EABI
} gv_status_t;

typedef struct gv_handle_t *gv_handle;

typedef struct gv_device_info_t
{
    char friendly_name[GDRIVER_MAX_FRIENDLY_NAME];
    char serial_number[GDRIVER_MAX_SERIAL_NUMBER];
    char firmware_version[GDRIVER_MAX_FIRMWARE_VERSION];
    char driver_version[GDRIVER_MAX_DRIVER_VERSION];
    uint32_t supported_inputs_mask;
    uint32_t supported_pixel_formats_mask;
    uint32_t max_video_channels;
    uint32_t max_audio_channels;
} gv_device_info_t;

typedef struct gv_device_entry_t
{
    char friendly_name[GVENDOR_MAX_DEVICE_NAME];
    char device_path[GVENDOR_MAX_DEVICE_PATH];
    gdriver_input_t inferred_input;
} gv_device_entry_t;

typedef struct gv_stream_desc_t
{
    uint32_t channel_index;
    gdriver_input_t input;
    uint32_t width;
    uint32_t height;
    uint32_t fps_num;
    uint32_t fps_den;
    gdriver_pixel_format_t pixel_format;
    uint32_t buffer_count;
    gdriver_memory_kind_t memory_kind;
    uint32_t flags;
} gv_stream_desc_t;

typedef struct gv_signal_status_t
{
    int signal_locked;
    gdriver_input_t input;
    uint32_t width;
    uint32_t height;
    uint32_t fps_num;
    uint32_t fps_den;
    gdriver_pixel_format_t pixel_format;
    uint32_t bit_depth;
} gv_signal_status_t;

typedef struct gv_frame_t
{
    const void *data; /* Valid until the next gv_wait_frame() on the same handle or gv_close(). */
    size_t data_size_bytes;
    uint64_t frame_id;
    uint64_t timestamp_ns;
    uint32_t width;
    uint32_t height;
    gdriver_pixel_format_t pixel_format;
    uint32_t bit_depth;
    uint32_t plane_count;
    uint32_t plane_offset_bytes[GDRIVER_MAX_PLANES];
    uint32_t plane_stride_bytes[GDRIVER_MAX_PLANES];
    uint32_t driver_buffer_index;
    uint32_t flags;
} gv_frame_t;

typedef struct gv_stream_stats_t
{
    gdriver_stream_state_t state;
    uint64_t frames_captured;
    uint64_t frames_delivered;
    uint64_t frames_dropped;
    uint64_t dma_errors;
    uint64_t interrupt_count;
} gv_stream_stats_t;

GVENDOR_API int gv_enumerate_devices(gv_device_entry_t *out, int max_devices);
GVENDOR_API gv_status_t gv_open_default(gv_handle *out);
GVENDOR_API gv_status_t gv_open_device_index(int device_index, gv_handle *out);
GVENDOR_API gv_status_t gv_close(gv_handle h);

GVENDOR_API gv_status_t gv_get_device_info(gv_handle h, gv_device_info_t *out);
GVENDOR_API gv_status_t gv_get_signal_status(gv_handle h, gv_signal_status_t *out);
GVENDOR_API gv_status_t gv_get_stream_stats(gv_handle h, gv_stream_stats_t *out);

GVENDOR_API gv_status_t gv_set_input(gv_handle h, gdriver_input_t input, uint32_t channel_index);
GVENDOR_API gv_status_t gv_configure_stream(gv_handle h, const gv_stream_desc_t *desc);
GVENDOR_API gv_status_t gv_start_stream(gv_handle h);
GVENDOR_API gv_status_t gv_stop_stream(gv_handle h);

/* Waits for one frame from the active KS stream. timeout_ms == 0 waits indefinitely. */
GVENDOR_API gv_status_t gv_wait_frame(gv_handle h, uint32_t timeout_ms, gv_frame_t *out);
/* KS-direct v0.1 uses SDK-owned staging memory, so release is currently a lightweight acknowledgment. */
GVENDOR_API gv_status_t gv_release_frame(gv_handle h, const gv_frame_t *frame);

GVENDOR_API const char *gv_strerror(gv_status_t status);
GVENDOR_API const char *gv_last_error(gv_handle h);

#ifdef __cplusplus
}
#endif
