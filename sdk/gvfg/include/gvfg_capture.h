#pragma once

/*
 * Customer-facing GVFG capture API.
 *
 * Include only this header in customer applications. Do not include internal
 * backend/gdriver headers; those layers are implementation details behind
 * gvfg.dll.
 *
 * Minimal capture flow:
 *
 *   gvfg_device_info_t devices[GVFG_MAX_DEVICES] = {};
 *   int count = gvfg_enumerate_devices(devices, GVFG_MAX_DEVICES);
 *
 *   gvfg_handle h = NULL;
 *   gvfg_create(&h);
 *   gvfg_set_callbacks(h, on_frame, on_error, user);
 *   // Optional: gvfg_set_frame_callback_interval(h, 6); // one callback per 6 frames
 *
 *   // Optional: let the SDK render preview directly into a native HWND.
 *   gvfg_preview_desc_t preview = {};
 *   preview.hwnd = hwnd;
 *   preview.enable_preview = 1;
 *   preview.swapchain_bitdepth = GVFG_PREVIEW_BITDEPTH_AUTO;
 *   gvfg_set_preview(h, &preview);
 *
 *   gvfg_open(h, devices[0].index);
 *   gvfg_start(h);
 *
 *   // Capture is now running. Use callbacks for frames/messages and
 *   // gvfg_get_runtime_info() for status.
 *
 *   gvfg_stop(h);
 *   gvfg_destroy(h);
 *
 * Threading notes:
 * - Frame and error callbacks are called from an SDK worker thread, not from
 *   the UI thread. UI applications should marshal callback work back to their
 *   UI thread.
 * - The frame data pointer is valid only during the callback. Copy the data if
 *   it must be used after the callback returns.
 */

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
    GVFG_EINVAL = -1,  /* Invalid argument, such as NULL handle/output pointer. */
    GVFG_ENODEV = -2,  /* No GVFG device, no signal, or device open failed. */
    GVFG_ESTATE = -3,  /* API was called in the wrong state. */
    GVFG_EIO = -4,     /* Driver/backend I/O failure. */
    GVFG_ENOTSUP = -5, /* Requested feature or format is not supported. */
    GVFG_ETIMEOUT = -6 /* Timed out waiting for driver/backend work. */
} gvfg_status_t;

typedef enum
{
    GVFG_PREVIEW_BITDEPTH_AUTO = 0,  /* Let the SDK choose the best swapchain format. */
    GVFG_PREVIEW_BITDEPTH_10BIT = 10, /* Prefer a 10-bit preview swapchain when available. */
    GVFG_PREVIEW_BITDEPTH_8BIT = 8    /* Force an 8-bit preview swapchain. */
} gvfg_preview_bitdepth_t;

typedef struct
{
    int index;       /* Device index to pass to gvfg_open(). */
    char name[128];  /* Display name for UI/logging. UTF-8, null-terminated. */
} gvfg_device_info_t;

typedef struct
{
    void *hwnd;              /* Native Windows HWND that stays alive while capture runs. */
    int enable_preview;      /* Non-zero enables SDK-managed preview rendering. */
    int swapchain_bitdepth;  /* gvfg_preview_bitdepth_t value. */
} gvfg_preview_desc_t;

typedef struct
{
    uint32_t valid_mask;            /* FPGA register read-valid mask: bit0=0x0c, bit1=0x18, bit2=0x1c, bit3=0x180. */
    int width_valid;                /* Non-zero when FPGA 0x10 read succeeded. */
    int height_valid;               /* Non-zero when FPGA 0x14 read succeeded. */
    uint32_t width_raw;             /* Raw FPGA 0x10 width register. */
    uint32_t height_raw;            /* Raw FPGA 0x14 height register. */
    uint32_t video_format_raw;      /* Raw FPGA 0x0c value. */
    int video_format_valid;
    int video_format_code;       /* 0=yuv422, 1=rgb, 2=yuv444, 3=yuv420 */
    char video_format[16];

    uint32_t frame_rate_raw;        /* Raw FPGA 0x18 value. */
    int frame_rate_valid;
    int frame_rate_code;         /* FPGA frame-rate code from 0x18 low nibble. */
    char frame_rate_bits[5];     /* 4-bit binary text, for example "0110". */
    char frame_rate_name[16];    /* None, 23.98, 24, 47.95, ..., or "--" for unsupported codes. */

    uint32_t bit_depth_raw;         /* Raw FPGA 0x1c value. */
    int bit_depth_valid;
    int bit_depth;               /* 8 or 10 when valid. */

    uint32_t status_raw;            /* Raw FPGA 0x180 value. */
    int status_valid;
    int sdi_locked;              /* FPGA 0x180 bit0 */
    int sdi_ddr_ok;              /* FPGA 0x180 bit1 */
    int hdmi_locked;             /* FPGA 0x180 bit2 */
    int hdmi_ddr_ok;             /* FPGA 0x180 bit3 */
} gvfg_fpga_signal_status_t;

typedef struct
{
    int width;                       /* Signal width in pixels, from FPGA when available. */
    int height;                      /* Signal height in pixels, from FPGA when available. */
    double fps;                      /* Legacy field; GVFG leaves this 0. Use fpga.frame_rate_* instead. */
    int bit_depth;                   /* Delivered buffer bit depth. Kept for compatibility; prefer gvfg_runtime_info_t::delivered_frame. */
    char pixel_format[32];           /* Delivered buffer format. Kept for compatibility; prefer gvfg_runtime_info_t::delivered_frame. */
    gvfg_fpga_signal_status_t fpga;  /* Raw/decoded FPGA signal metadata. */
} gvfg_signal_status_t;

typedef struct
{
    int width;              /* Width of frames delivered to the customer callback. */
    int height;             /* Height of frames delivered to the customer callback. */
    int bit_depth;          /* Bit depth of the delivered frame buffer. */
    char pixel_format[32];  /* Delivered frame format, for example YUY2 or Y210. */
    int valid;              /* Non-zero after at least one frame has been delivered. */
} gvfg_delivered_frame_info_t;

typedef struct
{
    gvfg_signal_status_t input_signal; /* FPGA-reported signal metadata plus legacy buffer fields. */
    gvfg_delivered_frame_info_t delivered_frame; /* Frame buffer delivered by gvfg.dll to the app. */
    double capture_fps;                /* Runtime FPS measured by the SDK/app worker. */
    uint64_t delivered_frames;         /* Number of frames delivered by the SDK/app worker. */
} gvfg_runtime_info_t;

typedef struct
{
    int enabled;                  /* Non-zero when preview was requested. */
    int active;                   /* Non-zero when the preview pipeline is currently active. */
    int width;                    /* Preview render width. */
    int height;                   /* Preview render height. */
    int swapchain_bitdepth;       /* Actual preview swapchain bit depth, usually 8 or 10. */
    int swapchain_10bit;          /* Non-zero when the active swapchain is 10-bit. */
    char render_path[128];        /* Human-readable preview pipeline description. */
    char backbuffer_format[64];   /* DXGI backbuffer format name. */
} gvfg_preview_info_t;

typedef struct
{
    const void *data;   /* Frame pixels. Valid only during the frame callback. */
    int stride;         /* Bytes per row for data. */
    int width;          /* Frame width in pixels. */
    int height;         /* Frame height in pixels. */
    uint64_t pts_ns;    /* Presentation timestamp in nanoseconds. */
    uint64_t frame_id;  /* Monotonic frame identifier from the backend. */
} gvfg_frame_t;

typedef enum
{
    GVFG_EVENT_VIDEO_IRQ = 1,
    GVFG_EVENT_PLUG_IN = 2,
    GVFG_EVENT_PLUG_OUT = 3,
    GVFG_EVENT_CAPTURE_PAUSED = 4,
    GVFG_EVENT_CAPTURE_RESUMED = 5
} gvfg_event_type_t;

enum
{
    GVFG_EVENT_MASK_VIDEO_IRQ = 1u << 0,
    GVFG_EVENT_MASK_PLUG_IN = 1u << 1,
    GVFG_EVENT_MASK_PLUG_OUT = 1u << 2,
    GVFG_EVENT_MASK_CAPTURE_PAUSED = 1u << 3,
    GVFG_EVENT_MASK_CAPTURE_RESUMED = 1u << 4,
    GVFG_EVENT_MASK_HOTPLUG = GVFG_EVENT_MASK_PLUG_IN |
                              GVFG_EVENT_MASK_PLUG_OUT |
                              GVFG_EVENT_MASK_CAPTURE_PAUSED |
                              GVFG_EVENT_MASK_CAPTURE_RESUMED,
    GVFG_EVENT_MASK_DEFAULT = GVFG_EVENT_MASK_HOTPLUG,
    GVFG_EVENT_MASK_ALL = GVFG_EVENT_MASK_VIDEO_IRQ | GVFG_EVENT_MASK_HOTPLUG
};

typedef struct
{
    gvfg_event_type_t type;
    uint32_t channel;
    uint32_t irq_bit;
    uint32_t irq_mask;
    uint64_t timestamp_ns;
} gvfg_event_t;

/* Opaque session handle created by gvfg_create() and released by gvfg_destroy(). */
typedef struct gvfg_handle_t *gvfg_handle;

/* Called when the SDK provides a frame for application use.
 * This callback is not required for SDK-managed preview rendering.
 */
typedef void (*gvfg_on_frame_cb)(const gvfg_frame_t *frame, void *user);

/* Called for capture events such as PLUG_IN / PLUG_OUT. */
typedef void (*gvfg_on_event_cb)(const gvfg_event_t *event, void *user);

/* Called for asynchronous SDK messages or errors. */
typedef void (*gvfg_on_error_cb)(gvfg_status_t status, const char *message, void *user);

/*
 * Enumerate GVFG capture devices.
 *
 * Parameters:
 * - out_devices: Output array that receives device entries. Pass NULL to query
 *   the device count only.
 * - max_devices: Number of entries available in out_devices. Values larger
 *   than GVFG_MAX_DEVICES are clamped. Pass 0 when out_devices is NULL.
 *
 * Returns:
 * - When out_devices is non-NULL, returns the number of entries written.
 * - When out_devices is NULL or max_devices is 0, returns the number of
 *   available devices.
 * - Returns <= 0 when no device is available.
 */
GVFG_API int gvfg_enumerate_devices(gvfg_device_info_t *out_devices, int max_devices);

/*
 * Create a GVFG capture session.
 *
 * Parameters:
 * - out_handle: Receives the new session handle. Must not be NULL.
 *
 * Returns:
 * - GVFG_OK on success.
 * - GVFG_EINVAL if out_handle is NULL.
 *
 * The returned handle starts in the closed state. Release it with
 * gvfg_destroy().
 */
GVFG_API gvfg_status_t gvfg_create(gvfg_handle *out_handle);

/*
 * Destroy a GVFG capture session.
 *
 * Parameters:
 * - handle: Session handle returned by gvfg_create().
 *
 * Returns:
 * - GVFG_OK.
 *
 * If capture is still running, it is stopped first. After this call, the handle
 * must not be used again.
 */
GVFG_API gvfg_status_t gvfg_destroy(gvfg_handle handle);

/*
 * Register frame and error callbacks.
 *
 * Parameters:
 * - handle: Session handle returned by gvfg_create().
 * - on_frame: Function called when a frame is available. Pass NULL if frame
 *   callbacks are not needed.
 * - on_error: Function called for asynchronous SDK messages/errors. Pass NULL
 *   if error callbacks are not needed.
 * - user: Application-defined pointer passed back to both callbacks.
 *
 * Returns:
 * - GVFG_OK on success.
 * - GVFG_EINVAL if handle is NULL.
 *
 * Callbacks are called from an SDK worker thread. The application must marshal
 * UI work to its UI thread.
 */
GVFG_API gvfg_status_t gvfg_set_callbacks(gvfg_handle handle,
                                             gvfg_on_frame_cb on_frame,
                                             gvfg_on_error_cb on_error,
                                             void *user);

/*
 * Configure frame callback rate.
 *
 * Parameters:
 * - handle: Session handle returned by gvfg_create().
 * - frame_interval: 0 or 1 calls on_frame for every delivered frame. N > 1
 *   calls on_frame once for every N delivered backend frames.
 *
 * Returns:
 * - GVFG_OK on success.
 * - GVFG_EINVAL if handle is NULL.
 *
 * This only affects the application frame callback. SDK-managed preview
 * rendering, gvfg_get_runtime_info(), and capture itself continue at full rate.
 */
GVFG_API gvfg_status_t gvfg_set_frame_callback_interval(gvfg_handle handle,
                                                        uint32_t frame_interval);

/*
 * Register capture event callback.
 *
 * Parameters:
 * - handle: Session handle returned by gvfg_create().
 * - on_event: Function called when an enabled capture event occurs. Pass NULL
 *   to disable event callbacks.
 * - user: Application-defined pointer passed back to the callback.
 * - event_mask: GVFG_EVENT_MASK_* bits. Pass 0 to use GVFG_EVENT_MASK_DEFAULT.
 *
 * Returns:
 * - GVFG_OK on success.
 * - GVFG_EINVAL if handle is NULL.
 *
 * VIDEO_IRQ is not included in the default mask because it can occur once per
 * video frame. Enable GVFG_EVENT_MASK_VIDEO_IRQ only for debug or when the
 * application explicitly needs it.
 */
GVFG_API gvfg_status_t gvfg_set_event_callback(gvfg_handle handle,
                                               gvfg_on_event_cb on_event,
                                               void *user,
                                               uint32_t event_mask);

/*
 * Configure SDK-managed preview rendering.
 *
 * Parameters:
 * - handle: Session handle returned by gvfg_create().
 * - desc: Preview configuration. Must not be NULL. When preview is enabled,
 *   desc->hwnd must be a valid native Windows HWND.
 *
 * Returns:
 * - GVFG_OK on success.
 * - GVFG_EINVAL if handle or desc is NULL.
 *
 * This API is optional. If enabled, call it before gvfg_start() and keep the
 * HWND alive while capture is running. Applications that render frames
 * themselves can skip this API and use the frame callback instead.
 */
GVFG_API gvfg_status_t gvfg_set_preview(gvfg_handle handle, const gvfg_preview_desc_t *desc);

/*
 * Open a device by index from gvfg_enumerate_devices().
 *
 * Parameters:
 * - handle: Session handle returned by gvfg_create().
 * - device_index: Device index from gvfg_device_info_t::index.
 *
 * Returns:
 * - GVFG_OK on success.
 * - GVFG_EINVAL if handle is NULL.
 * - GVFG_ENODEV if the device cannot be opened.
 * - GVFG_EIO for driver/backend failures.
 *
 * The current implementation selects the SDI input internally.
 */
GVFG_API gvfg_status_t gvfg_open(gvfg_handle handle, int device_index);

/*
 * Configure and start capture on an opened device.
 *
 * Parameters:
 * - handle: Opened session handle.
 *
 * Returns:
 * - GVFG_OK on success, including when capture is already running.
 * - GVFG_EINVAL if handle is NULL.
 * - GVFG_ESTATE if no device is open.
 * - GVFG_EIO or another status code if stream configuration/start fails.
 *
 * After success, frames and asynchronous messages are delivered through the
 * callbacks registered by gvfg_set_callbacks().
 */
GVFG_API gvfg_status_t gvfg_start(gvfg_handle handle);

/*
 * Stop capture.
 *
 * Parameters:
 * - handle: Session handle returned by gvfg_create().
 *
 * Returns:
 * - GVFG_OK on success, including when capture is already stopped.
 * - GVFG_EINVAL if handle is NULL.
 *
 * This waits for the SDK capture worker thread to exit.
 */
GVFG_API gvfg_status_t gvfg_stop(gvfg_handle handle);

/*
 * Query current signal information and delivered buffer format.
 *
 * Parameters:
 * - handle: Opened session handle.
 * - out_status: Receives signal status. Must not be NULL.
 *
 * Returns:
 * - GVFG_OK on success.
 * - GVFG_EINVAL if handle or out_status is NULL.
 * - GVFG_ENODEV if no valid signal information is available.
 *
 * The FPGA metadata describes the hardware signal. The pixel_format and
 * bit_depth fields describe the buffer format delivered by the customer API.
 */
GVFG_API gvfg_status_t gvfg_get_signal_status(gvfg_handle handle, gvfg_signal_status_t *out_status);

/*
 * Query runtime capture diagnostics.
 *
 * Parameters:
 * - handle: Opened or running session handle.
 * - out_info: Receives runtime information. Must not be NULL.
 *
 * Returns:
 * - GVFG_OK on success.
 * - GVFG_EINVAL if handle or out_info is NULL.
 *
 * The result includes current signal status, SDK-measured capture FPS, and the
 * number of frames delivered by the SDK.
 */
GVFG_API gvfg_status_t gvfg_get_runtime_info(gvfg_handle handle, gvfg_runtime_info_t *out_info);

/*
 * Query SDK-managed preview diagnostics.
 *
 * Parameters:
 * - handle: Session handle returned by gvfg_create().
 * - out_info: Receives preview information. Must not be NULL.
 *
 * Returns:
 * - GVFG_OK on success.
 * - GVFG_EINVAL if handle or out_info is NULL.
 *
 * This is useful only when the application uses gvfg_set_preview().
 */
GVFG_API gvfg_status_t gvfg_get_preview_info(gvfg_handle handle, gvfg_preview_info_t *out_info);

/*
 * Convert a GVFG status code to a static English error string.
 *
 * Parameters:
 * - status: Status code returned by a GVFG API.
 *
 * Returns:
 * - Static null-terminated English string. The caller must not free it.
 */
GVFG_API const char *gvfg_strerror(gvfg_status_t status);

#ifdef __cplusplus
}
#endif
