#pragma once

#include <stdint.h>
#include <stddef.h>
#include "gdriver_abi.h"

typedef enum xdma_status_t
{
    XDMA_OK = 0,
    XDMA_EINVAL,
    XDMA_ENODEV,
    XDMA_ESTATE,
    XDMA_ENOTSUP,
    XDMA_ETIMEOUT,
    XDMA_EIO,
    XDMA_EABI
} xdma_status_t;

typedef struct xdma_device_info_t
{
    char friendly_name[GDRIVER_MAX_FRIENDLY_NAME];
    char serial_number[GDRIVER_MAX_SERIAL_NUMBER];
    char firmware_version[GDRIVER_MAX_FIRMWARE_VERSION];
    char driver_version[GDRIVER_MAX_DRIVER_VERSION];
    uint32_t supported_inputs_mask;
    uint32_t supported_pixel_formats_mask;
} xdma_device_info_t;

typedef struct xdma_stream_desc_t
{
    gdriver_input_t input;
    uint32_t width;
    uint32_t height;
    gdriver_pixel_format_t pixel_format;
    uint32_t buffer_count;
    gdriver_memory_kind_t memory_kind;
    uint32_t flags;
} xdma_stream_desc_t;

typedef struct xdma_signal_status_t
{
    int signal_locked;
    gdriver_input_t input;
    uint32_t width;
    uint32_t height;
    gdriver_pixel_format_t pixel_format;
    uint32_t bit_depth;
    uint32_t fpga_valid_mask;       /* bit0:0x0c, bit1:0x18, bit2:0x1c, bit3:0x180 */
    uint32_t fpga_width_valid;      /* Non-zero when FPGA 0x10 read succeeded. */
    uint32_t fpga_height_valid;     /* Non-zero when FPGA 0x14 read succeeded. */
    uint32_t fpga_width_raw;        /* Raw FPGA 0x10 width register. */
    uint32_t fpga_height_raw;       /* Raw FPGA 0x14 height register. */
    uint32_t fpga_video_format_raw; /* 0x0c: 0=yuv422, 1=rgb, 2=yuv444, 3=yuv420 */
    uint32_t fpga_frame_rate_raw;   /* 0x18 low nibble: frame-rate code */
    uint32_t fpga_bit_depth_raw;    /* 0x1c: 8 or 10 */
    uint32_t fpga_status_raw;       /* 0x180: bit0 SDI lock, bit1 SDI DDR, bit2 HDMI lock, bit3 HDMI DDR */
} xdma_signal_status_t;

typedef struct xdma_frame_t
{
    const void *data; /* Valid until the next XdmaCaptureSession::wait_frame() or close(). */
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
} xdma_frame_t;

typedef struct xdma_stream_stats_t
{
    gdriver_stream_state_t state;
    uint64_t frames_captured;
    uint64_t frames_delivered;
    uint64_t frames_dropped;
    uint64_t dma_errors;
    uint64_t interrupt_count;
} xdma_stream_stats_t;

typedef enum xdma_event_type_t
{
    XDMA_EVENT_VIDEO_IRQ = 1,
    XDMA_EVENT_PLUG_IN = 2,
    XDMA_EVENT_PLUG_OUT = 3,
    XDMA_EVENT_CAPTURE_PAUSED = 4,
    XDMA_EVENT_CAPTURE_RESUMED = 5
} xdma_event_type_t;

enum
{
    XDMA_EVENT_MASK_VIDEO_IRQ = 1u << 0,
    XDMA_EVENT_MASK_PLUG_IN = 1u << 1,
    XDMA_EVENT_MASK_PLUG_OUT = 1u << 2,
    XDMA_EVENT_MASK_CAPTURE_PAUSED = 1u << 3,
    XDMA_EVENT_MASK_CAPTURE_RESUMED = 1u << 4,
    XDMA_EVENT_MASK_HOTPLUG = XDMA_EVENT_MASK_PLUG_IN |
                              XDMA_EVENT_MASK_PLUG_OUT |
                              XDMA_EVENT_MASK_CAPTURE_PAUSED |
                              XDMA_EVENT_MASK_CAPTURE_RESUMED,
    XDMA_EVENT_MASK_DEFAULT = XDMA_EVENT_MASK_HOTPLUG,
    XDMA_EVENT_MASK_ALL = XDMA_EVENT_MASK_VIDEO_IRQ | XDMA_EVENT_MASK_HOTPLUG
};

typedef struct xdma_event_t
{
    xdma_event_type_t type;
    uint32_t irq_bit;
    uint32_t irq_mask;
    uint64_t timestamp_ns;
} xdma_event_t;

typedef void (*xdma_event_callback_t)(const xdma_event_t *event, void *user);
