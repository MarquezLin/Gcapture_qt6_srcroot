#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define GDRIVER_ABI_VERSION_MAJOR 0u
#define GDRIVER_ABI_VERSION_MINOR 1u
#define GDRIVER_ABI_VERSION_PATCH 0u

#define GDRIVER_ABI_MAKE_VERSION(major, minor, patch) \
    ((((uint32_t)(major) & 0xffu) << 24) | (((uint32_t)(minor) & 0xffu) << 16) | ((uint32_t)(patch) & 0xffffu))

#define GDRIVER_ABI_VERSION \
    GDRIVER_ABI_MAKE_VERSION(GDRIVER_ABI_VERSION_MAJOR, GDRIVER_ABI_VERSION_MINOR, GDRIVER_ABI_VERSION_PATCH)

#define GDRIVER_MAX_FRIENDLY_NAME 128
#define GDRIVER_MAX_SERIAL_NUMBER 64
#define GDRIVER_MAX_FIRMWARE_VERSION 64
#define GDRIVER_MAX_DRIVER_VERSION 64
#define GDRIVER_MAX_PLANES 3

typedef enum gdriver_status_t
{
    GDRIVER_STATUS_OK = 0,
    GDRIVER_STATUS_INVALID_ARGUMENT = 1,
    GDRIVER_STATUS_UNSUPPORTED = 2,
    GDRIVER_STATUS_INVALID_STATE = 3,
    GDRIVER_STATUS_NO_DEVICE = 4,
    GDRIVER_STATUS_TIMEOUT = 5,
    GDRIVER_STATUS_BUFFER_TOO_SMALL = 6,
    GDRIVER_STATUS_IO_ERROR = 7
} gdriver_status_t;

typedef enum gdriver_input_t
{
    GDRIVER_INPUT_UNKNOWN = 0,
    GDRIVER_INPUT_SDI = 1,
    GDRIVER_INPUT_HDMI = 2
} gdriver_input_t;

typedef enum gdriver_pixel_format_t
{
    GDRIVER_PIXFMT_UNKNOWN = 0,
    GDRIVER_PIXFMT_YUY2 = 1,
    GDRIVER_PIXFMT_UYVY = 2,
    GDRIVER_PIXFMT_RGB24 = 3,
    GDRIVER_PIXFMT_BGRX32 = 4,
    GDRIVER_PIXFMT_NV12 = 5,
    GDRIVER_PIXFMT_P010 = 6,
    GDRIVER_PIXFMT_Y210 = 7
} gdriver_pixel_format_t;

typedef enum gdriver_memory_kind_t
{
    GDRIVER_MEMORY_DRIVER_COPY = 0,
    GDRIVER_MEMORY_SHARED_SECTION = 1,
    GDRIVER_MEMORY_DMA_RING = 2
} gdriver_memory_kind_t;

typedef enum gdriver_stream_state_t
{
    GDRIVER_STREAM_STOPPED = 0,
    GDRIVER_STREAM_CONFIGURED = 1,
    GDRIVER_STREAM_RUNNING = 2
} gdriver_stream_state_t;

typedef struct gdriver_abi_version_t
{
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t driver_build;
    uint32_t reserved0;
} gdriver_abi_version_t;

typedef struct gdriver_device_info_t
{
    uint32_t struct_size;
    uint32_t abi_version;
    char friendly_name[GDRIVER_MAX_FRIENDLY_NAME];
    char serial_number[GDRIVER_MAX_SERIAL_NUMBER];
    char firmware_version[GDRIVER_MAX_FIRMWARE_VERSION];
    char driver_version[GDRIVER_MAX_DRIVER_VERSION];
    uint32_t supported_inputs_mask;
    uint32_t supported_pixel_formats_mask;
    uint32_t max_video_channels;
    uint32_t max_audio_channels;
} gdriver_device_info_t;

typedef struct gdriver_capability_t
{
    uint32_t width;
    uint32_t height;
    uint32_t fps_num;
    uint32_t fps_den;
    gdriver_pixel_format_t pixel_format;
    uint32_t bit_depth;
} gdriver_capability_t;

typedef struct gdriver_capabilities_t
{
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t capability_count;
    uint32_t capability_capacity;
    gdriver_capability_t capabilities[1];
} gdriver_capabilities_t;

typedef struct gdriver_set_input_t
{
    uint32_t struct_size;
    uint32_t abi_version;
    gdriver_input_t input;
    uint32_t channel_index;
} gdriver_set_input_t;

typedef struct gdriver_stream_config_t
{
    uint32_t struct_size;
    uint32_t abi_version;
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
} gdriver_stream_config_t;

typedef struct gdriver_signal_status_t
{
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t signal_locked;
    gdriver_input_t input;
    uint32_t width;
    uint32_t height;
    uint32_t fps_num;
    uint32_t fps_den;
    gdriver_pixel_format_t pixel_format;
    uint32_t bit_depth;
} gdriver_signal_status_t;

typedef struct gdriver_wait_frame_t
{
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t timeout_ms;
    uint32_t reserved0;
} gdriver_wait_frame_t;

typedef struct gdriver_frame_desc_t
{
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t frame_id;
    uint64_t timestamp_ns;
    uint32_t width;
    uint32_t height;
    gdriver_pixel_format_t pixel_format;
    uint32_t bit_depth;
    uint32_t plane_count;
    uint32_t data_size_bytes;
    uint32_t plane_offset_bytes[GDRIVER_MAX_PLANES];
    uint32_t plane_stride_bytes[GDRIVER_MAX_PLANES];
    uint32_t driver_buffer_index;
    uint32_t flags;
} gdriver_frame_desc_t;

typedef struct gdriver_release_frame_t
{
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t frame_id;
    uint32_t driver_buffer_index;
    uint32_t reserved0;
} gdriver_release_frame_t;

typedef struct gdriver_stream_stats_t
{
    uint32_t struct_size;
    uint32_t abi_version;
    gdriver_stream_state_t state;
    uint32_t reserved0;
    uint64_t frames_captured;
    uint64_t frames_delivered;
    uint64_t frames_dropped;
    uint64_t dma_errors;
    uint64_t interrupt_count;
} gdriver_stream_stats_t;

#ifdef __cplusplus
}
#endif
