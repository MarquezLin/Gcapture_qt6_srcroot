#pragma once
#include <stdint.h>
#include <stddef.h>
#include "gcap_audio.h"

/*
    gcapture public C API

    This header is the SDK boundary. It intentionally uses C-compatible types,
    fixed-size UTF-8 strings, and opaque handles so applications can use the SDK
    from Qt, Win32/MFC, C#, Python wrappers, or other UI frameworks.

    Typical lifecycle:
      1. gcap_enumerate()
      2. gcap_create() + gcap_open2(), or gcap_open()
      3. Optional: gcap_set_backend(), gcap_set_profile(), gcap_set_processing()
      4. Optional preview: gcap_set_preview()
      5. Optional callbacks: gcap_set_callbacks(), gcap_set_frame_packet_callback()
      6. gcap_start()
      7. Optional runtime operations: snapshot, recording, signal/runtime queries
      8. gcap_stop()
      9. gcap_close()

    Thread-safety notes:
      - A single gcap_handle should normally be controlled from one application
        thread, except frame/error callbacks which are invoked from SDK/backend
        worker threads.
      - Callback pointers are only valid during the callback call. Copy data you
        need before returning.
      - Do not call blocking UI code directly inside callbacks. Marshal to your
        UI thread.
      - Unless a function explicitly says otherwise, do not call it concurrently
        with gcap_close() on the same handle.

    String notes:
      - Public string parameters and string fields are UTF-8 unless documented
        otherwise.
      - Fixed-size output buffers are always intended to be NUL-terminated by the
        SDK on success.
*/

#ifdef _WIN32
#ifdef GCAPTURE_BUILD
#define GCAP_API __declspec(dllexport)
#else
#define GCAP_API __declspec(dllimport)
#endif
#else
#define GCAP_API
#endif

#ifdef __cplusplus
extern "C"
{
#endif

    /** Capture backend selection. */
    typedef enum
    {
        /** Media Foundation CPU readback path. */
        GCAP_BACKEND_WINMF_CPU = 0,
        /** Media Foundation GPU/DXGI path. Recommended for WinMF preview when supported. */
        GCAP_BACKEND_WINMF_GPU = 1,
        /** DirectShow raw-sink path. Recommended for DShow capture devices. */
        GCAP_BACKEND_DSHOW = 2,
        /** SDK chooses an available backend. Actual backend can be queried by gcap_get_active_backend(). */
        GCAP_BACKEND_AUTO = 3
    } gcap_backend_t;

    /** Profile negotiation mode. */
    enum gcap_profile_mode_t
    {
        /** Let the device/backend choose its default format. */
        GCAP_PROFILE_DEVICE_DEFAULT = 0,
        /** Use width/height/fps/format from gcap_profile_t where supported. */
        GCAP_PROFILE_CUSTOM
    };


    /** SDK log level passed to gcap_log_callback_t. */
    typedef enum
    {
        GCAP_LOG_TRACE = 0,
        GCAP_LOG_DEBUG = 1,
        GCAP_LOG_INFO  = 2,
        GCAP_LOG_WARN  = 3,
        GCAP_LOG_ERROR = 4
    } gcap_log_level_t;

    /**
     * SDK log callback.
     *
     * The callback may be invoked from SDK/backend worker threads. Do not touch
     * UI objects directly; marshal the message to your UI/log thread.
     * message_utf8 is valid only during the callback call. Copy it if needed.
     */
    typedef void (*gcap_log_callback_t)(gcap_log_level_t level, const char *message_utf8, void *user);

    /** SDK status code returned by most high-level APIs. */
    typedef enum
    {
        GCAP_OK = 0,
        /** Invalid argument, null pointer, invalid state-independent parameter, or insufficient buffer. */
        GCAP_EINVAL,
        /** Device not found, removed, or unavailable. */
        GCAP_ENODEV,
        /** Operation is not valid in the current handle state. */
        GCAP_ESTATE,
        /** I/O, backend, driver, media, or file operation failed. */
        GCAP_EIO,
        /** Requested backend/format/operation is not supported. */
        GCAP_ENOTSUP
    } gcap_status_t;

    /** Pixel format used by capability, negotiated status, callbacks, and recording helpers. */
    typedef enum
    {
        GCAP_FMT_NV12, /** 8-bit YUV 4:2:0, two planes: Y + interleaved UV. */
        GCAP_FMT_YUY2, /** 8-bit YUV 4:2:2 packed. */
        GCAP_FMT_ARGB, /** 8-bit packed 32-bit RGB-like surface. In callbacks it is treated as BGRA/ARGB-compatible 4 bytes per pixel. */
        GCAP_FMT_P010, /** 10-bit YUV 4:2:0 in 16-bit containers, two planes. */
        GCAP_FMT_Y210, /** 10-bit YUV 4:2:2 packed in 16-bit containers. */
        GCAP_FMT_V210, /** 10-bit packed 4:2:2. Capability placeholder; conversion support may be limited. */
        GCAP_FMT_R210  /** 10-bit packed RGB. Capability placeholder; conversion support may be limited. */
    } gcap_pixfmt_t;

    /** Device enumeration result. */
    typedef struct
    {
        int index;                 /** Device index accepted by gcap_open() / gcap_open2(). */
        char name[128];            /** Friendly device name, UTF-8. */
        char symbolic_link[256];   /** Backend/device symbolic link when available, UTF-8. */
        unsigned caps;             /** Reserved bitmask. Current convention: 1<<0 HDMI, 1<<1 SDI, 1<<2 10-bit capable. */
    } gcap_device_info_t;

    typedef enum
    {
        GCAP_INPUT_UNKNOWN = 0,
        GCAP_INPUT_HDMI = 1,
        GCAP_INPUT_SDI = 2
    } gcap_input_t;

    typedef enum
    {
        GCAP_RANGE_UNKNOWN = 0,
        GCAP_RANGE_LIMITED = 1,
        GCAP_RANGE_FULL = 2
    } gcap_range_t;

    typedef enum
    {
        GCAP_CSP_UNKNOWN = 0,
        GCAP_CSP_BT601 = 1,
        GCAP_CSP_BT709 = 2,
        GCAP_CSP_BT2020 = 3
    } gcap_colorspace_t;

    /** Best-effort static device properties. Empty/zero/-1 fields mean unknown or not reported by the driver. */
    typedef struct
    {
        char driver_version[64];
        char firmware_version[64];
        char serial_number[64];
        gcap_input_t input;
        int pcie_gen;             /** 2/3/4/5, or 0 if unknown. */
        int pcie_lanes;           /** 1/4/8/16, or 0 if unknown. */
        int hdcp;                 /** 0 = no, 1 = yes, -1 = unknown. */
    } gcap_device_props_t;

    /** Signal/media-type status. Used for input probe, negotiated format, and runtime info. */
    typedef struct
    {
        int width, height;
        int fps_num, fps_den;      /** FPS numerator/denominator. 0 means unknown. */
        gcap_pixfmt_t pixfmt;
        int bit_depth;             /** 8/10/12, or 0 if unknown. */
        gcap_colorspace_t csp;
        gcap_range_t range;
        int hdr;                   /** 0 = SDR/unknown-not-HDR, 1 = HDR, -1 = unknown. */
    } gcap_signal_status_t;

    /** One video capability entry reported or inferred for a device/backend. */
    typedef struct
    {
        int width;
        int height;
        int fps_num;
        int fps_den;
        gcap_pixfmt_t pixfmt;
        int bit_depth;
        char subtype_name[32];       /** Backend/native media subtype name, e.g. HDYC/UYVY/v210/ARGB32. May be empty. */
    } gcap_video_cap_t;

    /** DirectShow property page descriptor. */
    typedef struct
    {
        char page_name[128];       /** Vendor/driver page name, UTF-8 when conversion succeeds. */
        int capture_pin;           /** 0 = filter property page, 1 = capture-pin property page. */
    } gcap_property_page_t;

    /** Runtime information for UI/status display and diagnostics. */
    typedef struct
    {
        gcap_signal_status_t signal;       /** Best-effort input signal shown to UI. */
        gcap_signal_status_t signal_probe; /** Raw probe/fallback result, for example DShow IAMStreamConfig::GetFormat(). */
        gcap_signal_status_t negotiated;   /** Media type actually negotiated/delivered by the backend. */
        double runtime_fps;                /** Measured delivered-frame FPS. 0 means unknown/not running yet. */
        int active_backend;                /** gcap_backend_t value actually in use. */
        char backend_name[32];             /** Display string, e.g. WinMF GPU / WinMF CPU / DShow Raw. */
        char frame_source[32];             /** Display string, e.g. DXGI / CPU / RawSink. */
        char path_name[64];                /** Display string describing active preview/capture path. */
        char source_format[32];            /** Legacy display field: negotiated media subtype name. */
        char render_format[32];            /** Display field: canvas / scene processing format. */
        char input_signal_desc[64];        /** Display field: RGB/YUV, color space, bit depth, may be inferred. */
        char input_signal_note[32];        /** Display field: Inferred / Driver / Unknown. */
        char negotiated_desc[32];          /** Display field: RGB24 / NV12 / YUY2 / ARGB32 / etc. */
        char selectable_caps_inline[512];  /** Active open-graph selectable modes summary after SDK filtering/policy. */
        char selectable_caps_tooltip[2048];/** Full active open-graph selectable modes after SDK filtering/policy. */
    } gcap_runtime_info_t;

    /** Human-readable recording configuration currently selected by the SDK. */
    typedef struct
    {
        char mode_name[64];       /** e.g. DShow + FFmpeg MP4 / Media Foundation Sink Writer. */
        char encoder_name[160];   /** e.g. FFmpeg HEVC / H.265 Main10. */
        char input_format[32];    /** SDK pixel format name. */
        char output_format[32];   /** Encoder/muxer output pixel/codec description. */
        int input_bit_depth;
        int output_bit_depth;
        int output_fps_num;
        int output_fps_den;
        int video_only;           /** 1 = current recorder writes video only; 0 = audio may be included when configured/supported. */
    } gcap_recording_info_t;

    /** Snapshot export flags. If desc->flags is 0, SDK exports RAW_ALL + TIFF + STATS + PNG. */
    typedef enum
    {
        GCAP_EXPORT_RAW_NATIVE      = 1 << 0, /** Native readback RAW: 10-bit source -> *_abgr2101010.raw; 8-bit source -> *_bgra8.raw. */
        GCAP_EXPORT_RAW_RGB10_U16   = 1 << 1, /** 10-bit RGB values stored in uint16 containers: *_rgb10_u16.raw. */
        GCAP_EXPORT_RAW_RGBA16      = 1 << 2, /** 10-bit values expanded to 16-bit RGBA: *_rgba16_expanded.raw. */
        GCAP_EXPORT_TIFF            = 1 << 3, /** WIC TIFF export: *.tiff. */
        GCAP_EXPORT_STATS           = 1 << 4, /** Text statistics: *.stats.txt. */
        GCAP_EXPORT_PNG             = 1 << 5, /** WIC PNG preview/export image: *.png. */
        GCAP_EXPORT_RAW_GIGABYTE_HEADER = 1 << 6, /** Additional RAW copies with 128-byte GIGABYTE RAW header: *_gigabyte_*.raw. */
        GCAP_EXPORT_RAW_ALL         = GCAP_EXPORT_RAW_NATIVE | GCAP_EXPORT_RAW_RGB10_U16 | GCAP_EXPORT_RAW_RGBA16
    } gcap_export_flags_t;

#define GCAP_GIGABYTE_RAW_HEADER_SIZE 128

    /*
     * GIGABYTE RAW header is a 128-byte ASCII header before the RAW payload:
     *   GIGABYTE_RAW
     *   header_size=128
     *   Width=<pixels>
     *   Height=<pixels>
     *   Format=<payload format>
     *   SourceBitDepth=<source bit depth>
     */

    /** Snapshot export request. */
    typedef struct
    {
        const char *base_path_utf8; /** Base path without SDK suffix. Must not be null/empty. UTF-8. */
        int flags;                  /** Bitwise OR of gcap_export_flags_t. 0 means default full export. */
    } gcap_snapshot_export_desc_t;

    /** Snapshot export result. Empty path means that file type was not requested or not generated for the source format. */
    typedef struct
    {
        char native_raw_path[512];  /** *_abgr2101010.raw for 10-bit sources, or *_bgra8.raw for 8-bit sources. */
        char fp16_raw_path[512];    /** *_fp16_rgba16f.raw, generated for 10-bit/FP16 scene readback when RAW export is requested. */
        char rgb10_u16_path[512];   /** *_rgb10_u16.raw, generated for 10-bit sources when RAW export is requested. */
        char rgba16_path[512];      /** *_rgba16_expanded.raw, generated for 10-bit sources when RAW export is requested. */
        char gigabyte_native_raw_path[512]; /** Headered mirror: *_gigabyte_abgr2101010.raw or *_gigabyte_bgra8.raw. */
        char gigabyte_fp16_raw_path[512];   /** Headered mirror: *_gigabyte_fp16_rgba16f.raw. */
        char gigabyte_rgb10_u16_path[512];  /** Headered mirror: *_gigabyte_rgb10_u16.raw. */
        char gigabyte_rgba16_path[512];     /** Headered mirror: *_gigabyte_rgba16_expanded.raw. */
        char tiff_path[512];        /** *.tiff when TIFF export is requested. */
        char stats_path[512];       /** *.stats.txt when STATS export is requested. */
        char png_path[512];         /** *.png when PNG export is requested. */
        int width;
        int height;
        gcap_pixfmt_t source_format;
        int source_bit_depth;
        int generated_flags;        /** Bitwise OR of actually generated gcap_export_flags_t values. */
    } gcap_snapshot_export_result_t;

    /** Recording statistics collected by the active recorder/backend. Unsupported fields remain 0. */
    typedef struct
    {
        uint64_t frames_written;
        uint64_t frames_dropped;
        uint64_t frames_duplicated;
        uint64_t input_frames;
        uint64_t unsupported_frames;
        uint64_t overwritten_frames;
        int width;
        int height;
        int fps_num;
        int fps_den;
        gcap_pixfmt_t input_pixfmt;
        int output_bit_depth;
        char encoder_name[128];
        char muxer_name[64];
        double avg_encode_fps;
        double avg_bitrate_kbps;
    } gcap_recording_stats_t;


    /**
     * Ramp-pattern validation status for gcap_tiff_analysis_t.
     *
     * Important: ramp validation is only meaningful for generated monotonic ramp test
     * images. A real camera/HDMI frame usually returns GCAP_TIFF_RAMP_NOT_APPLICABLE;
     * that does not mean the TIFF is 8-bit or invalid. Use likely_ten_bit_content,
     * min/max, unique_value_count, and effective_bit_depth as the general bit-depth
     * evidence for arbitrary images.
     */
    typedef enum
    {
        GCAP_TIFF_RAMP_NOT_CHECKED = 0,
        GCAP_TIFF_RAMP_NOT_APPLICABLE = 1,
        GCAP_TIFF_RAMP_DETECTED_VALID = 2,
        GCAP_TIFF_RAMP_DETECTED_INVALID = 3
    } gcap_tiff_ramp_status_t;

    /** TIFF bit-depth / optional ramp-pattern analysis result. Fixed-size C struct for SDK clients. */
    typedef struct
    {
        int ok;                         /** 1 = analysis succeeded, 0 = failed. */
        char path[512];                 /** Input TIFF path, UTF-8 when available. */
        char error[512];                /** Failure reason, UTF-8. Empty on success. */

        int width;
        int height;
        int channels;
        int bits_per_sample;
        int samples_per_pixel;
        int stored_bit_depth;
        int effective_bit_depth;

        char pixel_format_name[96];
        char photometric[64];

        uint64_t min_value;
        uint64_t max_value;
        uint64_t unique_value_count;

        int likely_ten_bit_ramp;              /** Backward-compatible alias: 1 only when ramp_status == GCAP_TIFF_RAMP_DETECTED_VALID. */
        int strict_ten_bit_ramp;              /** Strict monotonic ramp and likely 10-bit content. */
        int visual_ten_bit_ramp_candidate;    /** A monotonic visual ramp pattern was detected, regardless of bit-depth validity. */
        int likely_ten_bit_content;           /** General bit-depth evidence for arbitrary images; does not require a ramp pattern. */
        int values_look_shifted_10bit;
        int values_look_8bit_expanded;
        int ramp_status;                      /** gcap_tiff_ramp_status_t. Non-ramp images should be NOT_APPLICABLE, not failed. */

        char ramp_reason[512];
        char strict_ramp_reason[512];
        char visual_ramp_reason[512];
        char ramp_note[512];                  /** Human-readable explanation of ramp_status. */

        int sampled_row_y;
        char sampled_row_source[96];
        char sampled_row_logical10_rule[192];
        char sampled_row_raw16_csv[4096];
        char sampled_row_logical10_csv[4096];

        int preview_stride_bytes;        /** RGBA64 preview stride. Use gcap_read_tiff_preview_rgba64() to read data. */
        size_t preview_size_bytes;       /** Required RGBA64 preview buffer size. */
    } gcap_tiff_analysis_t;

    typedef enum
    {
        GCAP_DEINT_AUTO = 0,
        GCAP_DEINT_OFF,
        GCAP_DEINT_WEAVE,
        GCAP_DEINT_BOB
    } gcap_deinterlace_t;

    /** Optional processing preferences. Unsupported options are ignored or return GCAP_ENOTSUP depending on backend. */
    typedef struct
    {
        gcap_pixfmt_t preferred_pixfmt;   /** Preferred capture format. Use backend capability APIs to discover support. */
        gcap_deinterlace_t deinterlace;
        gcap_range_t force_range;         /** GCAP_RANGE_UNKNOWN = auto. */
    } gcap_processing_opts_t;

    /** ProcAmp settings. SDK UI scale is 0..255, where 128 is neutral. */
    typedef struct
    {
        int brightness;
        int contrast;
        int hue;
        int saturation;
        int sharpness;
    } gcap_procamp_t;

    /** One ProcAmp control range in SDK UI units. */
    typedef struct
    {
        int supported;      /** Non-zero if this control is supported by the active backend/path. */
        int min_value;      /** Minimum accepted value. */
        int max_value;      /** Maximum accepted value. */
        int step;           /** Recommended slider step. */
        int default_value;  /** Recommended neutral/default value. */
        int current_value;  /** Current value if available; otherwise default_value. */
    } gcap_procamp_range_t;

    /** ProcAmp capability block for Brightness/Contrast/Hue/Saturation/Sharpness. */
    typedef struct
    {
        gcap_procamp_range_t brightness;
        gcap_procamp_range_t contrast;
        gcap_procamp_range_t hue;
        gcap_procamp_range_t saturation;
        gcap_procamp_range_t sharpness;
    } gcap_procamp_caps_t;

    /** Requested capture profile. For GCAP_PROFILE_CUSTOM, set width/height/fps/format. */
    typedef struct
    {
        int width, height;
        int fps_num, fps_den;
        gcap_pixfmt_t format;
        gcap_profile_mode_t mode;
    } gcap_profile_t;

    /** Legacy frame callback payload. Prefer gcap_frame_packet_t for SDK clients that need backend/source metadata. */
    typedef struct
    {
        const void *data[3];       /** Plane pointers. Valid only during callback. */
        int stride[3];             /** Bytes per row for each plane. */
        int plane_count;
        int width, height;
        gcap_pixfmt_t format;
        uint64_t pts_ns;
        uint64_t frame_id;
    } gcap_frame_t;

    typedef enum
    {
        GCAP_SOURCE_UNKNOWN = 0,
        GCAP_SOURCE_WINMF_GPU = 1,
        GCAP_SOURCE_WINMF_CPU = 2,
        GCAP_SOURCE_DSHOW_RAWSINK = 3,
        GCAP_SOURCE_DSHOW_RENDERER = 4 /** Legacy/debug renderer path. Prefer GCAP_SOURCE_DSHOW_RAWSINK for SDK clients. */
    } gcap_frame_source_kind_t;

    /** Preferred raw frame callback payload. Plane pointers are valid only during callback. */
    typedef struct
    {
        int width, height;
        gcap_pixfmt_t format;
        int plane_count;
        const void *data[3];       /** Plane pointers. Do not retain after callback returns. */
        int stride[3];             /** Bytes per row for each plane. */
        uint64_t pts_ns;
        uint64_t frame_id;
        int backend;               /** gcap_backend_t value. */
        int source_kind;           /** gcap_frame_source_kind_t value. */
        int gpu_backed;            /** 1 when frame originated from GPU-backed path; callback data is still CPU-readable. */
    } gcap_frame_packet_t;

    typedef enum
    {
        GCAP_PREVIEW_BITDEPTH_8BIT = 0,
        GCAP_PREVIEW_BITDEPTH_10BIT = 1,
        GCAP_PREVIEW_BITDEPTH_AUTO = 2
    } gcap_preview_bitdepth_t;

    /** Native preview target. The SDK renders into the supplied HWND. */
    typedef struct
    {
        void *hwnd;                /** Native HWND. Required when enable_preview is non-zero. */
        int enable_preview;        /** 0 = disable preview, 1 = enable preview. */
        int use_fp16_pipeline;     /** 1 = use FP16 scene pipeline when available. Recommended for 10-bit/HDR-oriented paths. */
        int swapchain_10bit;       /** 0 = force 8-bit BGRA, 1 = prefer/force 10-bit RGB10A2, 2 = auto try 10-bit then fallback 8-bit. */
    } gcap_preview_desc_t;

    typedef void (*gcap_on_video_cb)(const gcap_frame_t *frame, void *user);
    typedef void (*gcap_on_frame_packet_cb)(const gcap_frame_packet_t *pkt, void *user);
    typedef void (*gcap_on_error_cb)(gcap_status_t code, const char *msg, void *user);

    typedef struct gcap_handle_t *gcap_handle;

    /**
     * Installs a process-wide SDK log callback.
     *
     * This is intended for SDK clients to receive WinMF/DShow/snapshot/recording
     * diagnostics in their own logging system. Passing cb = NULL disables the
     * callback. The callback may be invoked from SDK/backend worker threads.
     * The SDK may still also send messages to OutputDebugString on Windows as
     * a fallback.
     */
    GCAP_API void gcap_set_log_callback(gcap_log_callback_t cb, void *user);

    /**
     * Enumerate capture devices.
     *
     * Parameters:
     *   out   - Output array. Must not be null in the current implementation.
     *   max   - Number of entries available in out.
     *   count - Receives total/actual count when non-null.
     *
     * Return:
     *   GCAP_OK on success. GCAP_EINVAL for invalid output array/max.
     *
     * Thread-safety:
     *   Safe to call before any handle is created. Avoid calling repeatedly from
     *   a real-time callback thread because device enumeration can touch OS/driver APIs.
     */
    GCAP_API gcap_status_t gcap_enumerate(gcap_device_info_t *out, int max, int *count);

    /** Create an unopened handle. Use gcap_open2() before gcap_start(). */
    GCAP_API gcap_status_t gcap_create(gcap_handle *out);

    /** Convenience API: create and open a device in one call. Close with gcap_close(). */
    GCAP_API gcap_status_t gcap_open(int device_index, gcap_handle *out);

    /** Open an existing handle created by gcap_create(). Must be called before gcap_start(). */
    GCAP_API gcap_status_t gcap_open2(gcap_handle h, int device_index);

    /**
     * Set requested capture profile.
     *
     * Call state:
     *   Recommended after open and before start. Changing profile while running
     *   may return GCAP_ESTATE or may require stop/start depending on backend.
     *
     * Notes:
     *   Use gcap_enum_video_caps_ex() / gcap_get_recommended_profile() to find
     *   supported values. The driver may still negotiate a different final format;
     *   query gcap_get_runtime_info() after start to verify.
     */
    GCAP_API gcap_status_t gcap_set_profile(gcap_handle h, const gcap_profile_t *prof);

    /** Configure SDK buffering hints. Call before gcap_start(). Backend may ignore unsupported hints. */
    GCAP_API gcap_status_t gcap_set_buffers(gcap_handle h, int count, size_t bytes_hint);

    /**
     * Set legacy video/error callbacks.
     *
     * Callback lifetime:
     *   Frame data is valid only during the callback. Copy before returning.
     *   Passing null callbacks disables that callback type.
     */
    GCAP_API gcap_status_t gcap_set_callbacks(gcap_handle h, gcap_on_video_cb vcb, gcap_on_error_cb ecb, void *user);

    /**
     * Set preferred frame packet callback.
     *
     * Call state:
     *   Usually configured before gcap_start(); can be cleared before/after stop.
     *
     * Thread-safety:
     *   Invoked from SDK/backend worker thread. Do not block for long periods.
     */
    GCAP_API gcap_status_t gcap_set_frame_packet_callback(gcap_handle h, gcap_on_frame_packet_cb cb, void *user);

    /** Start capture/preview/callback delivery. Requires an opened handle. */
    GCAP_API gcap_status_t gcap_start(gcap_handle h);

    /**
     * Start recording to an output path.
     *
     * Call state:
     *   Requires an opened and normally running capture session. If called before
     *   capture is started, backend support is not guaranteed.
     *
     * Parameters:
     *   path_utf8 - Output file path, UTF-8. MP4 is the intended container for
     *               current FFmpeg/MF recorder paths.
     *
     * Notes:
     *   Use gcap_get_recording_info() after profile/backend selection to show the
     *   selected encoder/mode to the user. Use gcap_get_recording_stats() after or
     *   during recording to inspect counters supported by the active backend.
     */
    GCAP_API gcap_status_t gcap_start_recording(gcap_handle h, const char *path_utf8);

    /** Stop active recording. Safe to call when recording is not active; backend may return GCAP_ESTATE. */
    GCAP_API gcap_status_t gcap_stop_recording(gcap_handle h);

    /** Stop capture/preview/callback delivery. Recording should be stopped first. */
    GCAP_API gcap_status_t gcap_stop(gcap_handle h);

    /**
     * Enumerate WASAPI capture endpoints.
     *
     * Legacy status-code API. New clients may prefer gcap_audio_device_count()
     * and gcap_audio_enum_devices().
     */
    GCAP_API gcap_status_t gcap_enumerate_audio_devices(gcap_audio_device_t *out, int max, int *count);

    /**
     * Select WASAPI capture endpoint for recording.
     *
     * Call state:
     *   Call before gcap_start_recording(). If changed while recording, behavior is
     *   backend-dependent and not recommended.
     *
     * Parameters:
     *   device_id_utf8 - Endpoint id from audio enumeration. null or empty string
     *                    means system default capture endpoint.
     */
    GCAP_API gcap_status_t gcap_set_recording_audio_device(gcap_handle h, const char *device_id_utf8);

    /** Close and destroy a handle. Do not use the handle after this call. */
    GCAP_API gcap_status_t gcap_close(gcap_handle h);

    /** Set global preferred backend for subsequently opened sessions. Not per-handle. */
    GCAP_API void gcap_set_backend(int backend);

    /** Select D3D11 adapter index for GPU/DXGI processing. -1 means system default adapter. */
    GCAP_API void gcap_set_d3d_adapter(int adapter_index);

    /** Query actual backend currently used by an opened/running handle. Returns -1 for invalid handle. */
    GCAP_API int gcap_get_active_backend(gcap_handle h);

    /**
     * Legacy snapshot helper.
     *
     * Prefer gcap_export_snapshot() for new SDK clients because it returns the
     * generated paths and flags. This helper uses base_path_utf8 plus SDK-defined
     * suffixes and writes requested RAW/TIFF/STATS outputs.
     */
    GCAP_API gcap_status_t gcap_export_preview_scene_rgb10(gcap_handle h, const char *base_path_utf8,
                                                           int export_raw, int export_tiff, int export_stats);

    /**
     * Export a snapshot from the current preview scene/readback path.
     *
     * Call state:
     *   Requires a valid opened handle and a capture/preview scene that has already
     *   produced at least one frame. In normal use, call after gcap_start() and
     *   after frames are visible/delivered. It does not require the caller to have
     *   a frame callback. A preview HWND is strongly recommended because the export
     *   currently reads from the SDK preview/scene pipeline.
     *
     * Recording interaction:
     *   Intended to be callable while recording, but it may briefly contend with
     *   GPU/readback/file I/O. Avoid calling it from frame callbacks or timing-
     *   critical threads.
     *
     * Output naming:
     *   Given base_path_utf8 = "C:/cap/snapshot_001", generated paths are:
     *     10-bit source RAW:
     *       C:/cap/snapshot_001_abgr2101010.raw
     *       C:/cap/snapshot_001_fp16_rgba16f.raw
     *       C:/cap/snapshot_001_rgb10_u16.raw
     *       C:/cap/snapshot_001_rgba16_expanded.raw
     *     8-bit source RAW:
     *       C:/cap/snapshot_001_bgra8.raw
     *     TIFF:
     *       C:/cap/snapshot_001.tiff
     *     STATS:
     *       C:/cap/snapshot_001.stats.txt
     *     PNG:
     *       C:/cap/snapshot_001.png
     *
     * Parameters:
     *   h    - Opened capture handle.
     *   desc - Export request. desc->base_path_utf8 must not be null/empty.
     *          desc->flags == 0 means RAW_ALL | TIFF | STATS | PNG.
     *   out  - Optional result. If non-null, SDK clears it to zero before writing.
     *
     * Return:
     *   GCAP_OK on success. On failure, no guarantee is made that partial files were
     *   not written; callers may delete files using the expected suffixes if needed.
     */
    GCAP_API gcap_status_t gcap_export_snapshot(gcap_handle h, const gcap_snapshot_export_desc_t *desc,
                                                gcap_snapshot_export_result_t *out);

    /** Query best-effort static device properties. Can be called after open; values may be unknown. */
    GCAP_API gcap_status_t gcap_get_device_props(gcap_handle h, gcap_device_props_t *out);

    /** Query current/best-effort input signal. Can be called after open/start. */
    GCAP_API gcap_status_t gcap_get_signal_status(gcap_handle h, gcap_signal_status_t *out);

    /** Query combined runtime status. Most useful after gcap_start(). */
    GCAP_API gcap_status_t gcap_get_runtime_info(gcap_handle h, gcap_runtime_info_t *out);

    /** Set processing preferences such as preferred format, deinterlace, or forced range. Prefer calling before start. */
    GCAP_API gcap_status_t gcap_set_processing(gcap_handle h, const gcap_processing_opts_t *opts);

    /**
     * Query current ProcAmp values from the active backend/path.
     *
     * Can be called after gcap_open(). If the active backend/path does not
     * support SDK ProcAmp, returns GCAP_ENOTSUP.
     */
    GCAP_API gcap_status_t gcap_get_procamp(gcap_handle h, gcap_procamp_t *out);

    /**
     * Query ProcAmp control ranges/capabilities.
     *
     * The current implementation exposes SDK-side preview/conversion ProcAmp
     * controls in 0..255 units, default 128. These are not necessarily the
     * hardware driver's IAMVideoProcAmp ranges. Unsupported controls are marked
     * supported = 0.
     */
    GCAP_API gcap_status_t gcap_get_procamp_caps(gcap_handle h, gcap_procamp_caps_t *out);

    /** Apply ProcAmp settings on supported CPU/GPU preview/conversion paths. Passing null resets to neutral. */
    GCAP_API gcap_status_t gcap_set_procamp(gcap_handle h, const gcap_procamp_t *p);

    /** Reset ProcAmp settings to backend defaults/neutral values. */
    GCAP_API gcap_status_t gcap_reset_procamp(gcap_handle h);

    /** Legacy audio count helper. Prefer gcap_audio_device_count() for new code. */
    GCAP_API int gcap_get_audio_device_count(void);

    /** Legacy audio enumeration helper. Prefer gcap_audio_enum_devices() for new code. Returns number written, or total if out_devices is null. */
    GCAP_API int gcap_enum_audio_devices(gcap_audio_device_t *out_devices, int max_devices);

    /** Return number of active WASAPI capture devices. */
    GCAP_API int gcap_audio_device_count(void);

    /** Enumerate active WASAPI capture devices. Returns number written, or total if out_devices is null or max_devices <= 0. */
    GCAP_API int gcap_audio_enum_devices(gcap_audio_device_t *out_devices, int max_devices);

    /** Return stable SDK pixel format display name. Never returns null. */
    GCAP_API const char *gcap_pixfmt_name(gcap_pixfmt_t fmt);

    /** Return nominal bit depth for a pixel format, or 0 if unknown/unsupported. */
    GCAP_API int gcap_pixfmt_bit_depth(gcap_pixfmt_t fmt);

    /** Return non-zero if the pixel format is treated as 10-bit by SDK helpers. */
    GCAP_API int gcap_pixfmt_is_10bit(gcap_pixfmt_t fmt);

    /** Return non-zero if the pixel format is YUV-family. */
    GCAP_API int gcap_pixfmt_is_yuv(gcap_pixfmt_t fmt);

    /** Return default stride in bytes for a single row/primary plane at the given width, or 0 if unknown. */
    GCAP_API int gcap_pixfmt_default_stride(int width, gcap_pixfmt_t fmt);

    /** Return stable backend display name. Never returns null. */
    GCAP_API const char *gcap_backend_name(int backend);

    /** Return stable frame source display name. Never returns null. */
    GCAP_API const char *gcap_source_kind_name(int source_kind);

    /** Return non-zero when the recording helper would choose HEVC/H.265 Main10 for the input format. */
    GCAP_API int gcap_recording_uses_hevc_main10(gcap_pixfmt_t fmt);

    /** Return human-readable recording mode name for a backend. Never returns null. */
    GCAP_API const char *gcap_recording_mode_name(int backend);

    /**
     * Query current recording mode/encoder information.
     *
     * Call state:
     *   Can be called after open/profile selection, before or during recording.
     *   The result is based on current backend and negotiated/requested format;
     *   after gcap_start(), it is more accurate.
     */
    GCAP_API gcap_status_t gcap_get_recording_info(gcap_handle h, gcap_recording_info_t *out);

    /**
     * Query recording counters/statistics.
     *
     * Call state:
     *   Can be called during recording or after gcap_stop_recording() while the
     *   handle is still valid. Unsupported counters remain 0.
     */
    GCAP_API gcap_status_t gcap_get_recording_stats(gcap_handle h, gcap_recording_stats_t *out);

    /**
     * Convert callback frame packet to packed BGRA8.
     *
     * Parameters:
     *   src        - Frame packet received from gcap_on_frame_packet_cb.
     *   dst        - Destination buffer.
     *   dst_stride - Destination bytes per row. Must be >= src->width * 4.
     *
     * Supported input formats:
     *   GCAP_FMT_ARGB, GCAP_FMT_NV12, GCAP_FMT_YUY2, GCAP_FMT_Y210.
     *
     * Thread-safety:
     *   Stateless helper. It may be called from inside the frame callback if the
     *   destination buffer is caller-owned and conversion cost is acceptable.
     */
    GCAP_API gcap_status_t gcap_frame_to_bgra8(const gcap_frame_packet_t *src, void *dst, int dst_stride);


    /**
     * Analyze a TIFF file written by SDK snapshot export or any WIC-readable TIFF.
     *
     * The analysis uses Windows Imaging Component on Windows and reports stored
     * bit depth, effective bit depth heuristics, min/max/unique values, and
     * optional ramp-pattern validation. Ramp validation is only meaningful for
     * generated monotonic ramp test images; normal camera/HDMI content commonly
     * reports GCAP_TIFF_RAMP_NOT_APPLICABLE. That is not a failure and does not
     * imply 8-bit content. Use likely_ten_bit_content and the value statistics
     * as the general bit-depth evidence for arbitrary images.
     *
     * It does not allocate memory for the caller.
     */
    GCAP_API gcap_status_t gcap_analyze_tiff(const char *path_utf8, gcap_tiff_analysis_t *out);

    /**
     * Decode the first TIFF frame into RGBA64 little-endian pixels for preview.
     *
     * Pass dst = NULL or dst_size = 0 to query required_size/width/height/stride.
     * The destination buffer must be at least required_size bytes.
     */
    GCAP_API gcap_status_t gcap_read_tiff_preview_rgba64(const char *path_utf8,
                                                         void *dst,
                                                         size_t dst_size,
                                                         int *width,
                                                         int *height,
                                                         int *stride_bytes,
                                                         size_t *required_size);

    /** Return human-readable status string. Never returns null. */
    GCAP_API const char *gcap_strerror(gcap_status_t);

    /**
     * Configure native HWND preview.
     *
     * Call state:
     *   Recommended after open and before start. Some backends may allow changing
     *   preview target while stopped only. To disable preview, pass enable_preview=0.
     *
     * Notes:
     *   The SDK does not own the HWND. The application must keep it valid while
     *   preview is enabled/running.
     */
    GCAP_API gcap_status_t gcap_set_preview(gcap_handle h, const gcap_preview_desc_t *desc);

    /**
     * Enumerate DirectShow video format capabilities for a device index.
     *
     * Return:
     *   Number of entries written. Passing null or max_caps <= 0 returns supported
     *   count when available.
     *
     * Notes:
     *   DShow-specific legacy helper. New code can use gcap_enum_video_caps_ex().
     */
    GCAP_API int gcap_enum_video_caps(int device_index, gcap_video_cap_t *out_caps, int max_caps);

    /** Backend-aware capability enumeration helper. Returns number written, or count when out_caps is null/max_caps <= 0. */
    GCAP_API int gcap_enum_video_caps_ex(int backend, int device_index, gcap_video_cap_t *out_caps, int max_caps);

    /** Return SDK-recommended profile for backend/device based on current format priority and capabilities. */
    GCAP_API gcap_status_t gcap_get_recommended_profile(int backend, int device_index, gcap_profile_t *out_profile);

    /** Enumerate unique pixel formats supported by a device/backend. Returns number written, or count when out_formats is null/max_formats <= 0. */
    GCAP_API int gcap_enum_supported_pixel_formats(int backend, int device_index, gcap_pixfmt_t *out_formats, int max_formats);

    /** Enumerate available DirectShow property pages for a device. Returns number written, or count when out_pages is null/max_pages <= 0. */
    GCAP_API int gcap_enum_property_pages(int device_index, gcap_property_page_t *out_pages, int max_pages);

    /** Open a device-specific vendor DShow property page. Debug/test helper; not required for generic capture flow. */
    GCAP_API int gcap_open_vendor_property_page(int device_index);

    /** Open a named DirectShow property page. page_name_utf8 must match a name returned by gcap_enum_property_pages(). */
    GCAP_API int gcap_open_named_property_page(int device_index, const char *page_name_utf8, int capture_pin);

#ifdef __cplusplus
}
#endif
