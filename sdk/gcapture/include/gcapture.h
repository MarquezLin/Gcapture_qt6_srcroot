#pragma once
#include <stdint.h>
#include "gcap_audio.h"

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

    typedef enum
    {
        GCAP_BACKEND_WINMF_CPU = 0,
        GCAP_BACKEND_WINMF_GPU = 1,
        GCAP_BACKEND_DSHOW = 2,
        GCAP_BACKEND_AUTO = 3
    } gcap_backend_t;

    enum gcap_profile_mode_t
    {
        GCAP_PROFILE_DEVICE_DEFAULT = 0,
        GCAP_PROFILE_CUSTOM
    };

    typedef enum
    {
        GCAP_OK = 0,
        GCAP_EINVAL,
        GCAP_ENODEV,
        GCAP_ESTATE,
        GCAP_EIO,
        GCAP_ENOTSUP
    } gcap_status_t;

    typedef enum
    {
        GCAP_FMT_NV12,
        GCAP_FMT_YUY2,
        GCAP_FMT_ARGB,
        GCAP_FMT_P010,
        GCAP_FMT_Y210,
        GCAP_FMT_V210,
        GCAP_FMT_R210
    } gcap_pixfmt_t;

    typedef struct
    {
        int index;
        char name[128];
        char symbolic_link[256];
        unsigned caps; // bitmask: 1<<0:HDMI,1<<1:SDI,1<<2:BIT10...
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

    typedef struct
    {
        char driver_version[64];
        char firmware_version[64];
        char serial_number[64];
        gcap_input_t input;
        int pcie_gen;   // e.g. 2/3/4/5 (0=unknown)
        int pcie_lanes; // x1/x4/x8/x16 (0=unknown)
        int hdcp;       // 0/1, -1=unknown
    } gcap_device_props_t;

    typedef struct
    {
        int width, height;
        int fps_num, fps_den;  // negotiated FPS
        gcap_pixfmt_t pixfmt;  // NV12/YUY2/P010/ARGB...
        int bit_depth;         // 8/10/12 (0=unknown)
        gcap_colorspace_t csp; // BT.709/BT.2020...
        gcap_range_t range;    // limited/full
        int hdr;               // 0/1, -1=unknown
    } gcap_signal_status_t;

    typedef struct
    {
        int width;
        int height;
        int fps_num;
        int fps_den;
        gcap_pixfmt_t pixfmt;
        int bit_depth;
    } gcap_video_cap_t;

    typedef struct
    {
        char page_name[128];
        int capture_pin; // 0=filter, 1=capture pin
    } gcap_property_page_t;


    typedef struct
    {
        gcap_signal_status_t signal;       // best-effort input signal shown to UI
        gcap_signal_status_t signal_probe; // raw probe / fallback (e.g. DShow GetFormat)
        gcap_signal_status_t negotiated;   // negotiated media type actually delivered to app/backend
        double runtime_fps;               // measured FPS from actual delivered frames (0 = unknown)
        int active_backend;               // GCAP_BACKEND_*
        char backend_name[32];            // e.g. WinMF GPU / WinMF CPU / DShow Raw
        char frame_source[32];            // e.g. DXGI / CPU / RawSink
        char path_name[64];               // e.g. WinMF GPU Preview / DShow Raw Preview
        char source_format[32];           // legacy: negotiated media subtype name
        char render_format[32];           // canvas / scene processing format
        char input_signal_desc[64];       // e.g. RGB444 / BT.709 / 8-bit (may be inferred)
        char input_signal_note[32];       // e.g. Inferred / Driver / Unknown
        char negotiated_desc[32];         // e.g. RGB24 / NV12 / YUY2 / ARGB32
    } gcap_runtime_info_t;

    typedef enum
    {
        GCAP_DEINT_AUTO = 0,
        GCAP_DEINT_OFF,
        GCAP_DEINT_WEAVE,
        GCAP_DEINT_BOB
    } gcap_deinterlace_t;

    typedef struct
    {
        gcap_pixfmt_t preferred_pixfmt; // Auto=GCAP_FMT_*?（你可用 NV12/YUY2/P010）
        gcap_deinterlace_t deinterlace;
        gcap_range_t force_range; // unknown=auto
    } gcap_processing_opts_t;

    // ----------------------------
    // ProcAmp (Brightness/Contrast/Hue/Saturation/Sharpness)
    // UI uses 0..255, where 128 is neutral.
    // ----------------------------
    typedef struct
    {
        int brightness; // 0..255, 128 neutral
        int contrast;   // 0..255, 128 neutral
        int hue;        // 0..255, 128 neutral
        int saturation; // 0..255, 128 neutral
        int sharpness;  // 0..255, 128 neutral
    } gcap_procamp_t;

    typedef struct
    {
        int width, height;
        int fps_num, fps_den;
        gcap_pixfmt_t format;
        gcap_profile_mode_t mode;
    } gcap_profile_t;

    typedef struct
    {
        const void *data[3];
        int stride[3];
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
        GCAP_SOURCE_DSHOW_RENDERER = 4
    } gcap_frame_source_kind_t;

    typedef struct
    {
        int width, height;
        gcap_pixfmt_t format;
        int plane_count;
        const void *data[3];
        int stride[3];
        uint64_t pts_ns;
        uint64_t frame_id;
        int backend;
        int source_kind;
        int gpu_backed;
    } gcap_frame_packet_t;

    typedef struct
    {
        void *hwnd;         // native HWND
        int enable_preview; // 0/1
        int use_fp16_pipeline;
        int swapchain_10bit;
    } gcap_preview_desc_t;

    typedef void (*gcap_on_video_cb)(const gcap_frame_t *frame, void *user);
    typedef void (*gcap_on_frame_packet_cb)(const gcap_frame_packet_t *pkt, void *user);
    typedef void (*gcap_on_error_cb)(gcap_status_t code, const char *msg, void *user);

    typedef struct gcap_handle_t *gcap_handle;

    gcap_status_t gcap_enumerate(gcap_device_info_t *out, int max, int *count);
    GCAP_API gcap_status_t gcap_create(gcap_handle *out);
    gcap_status_t gcap_open(int device_index, gcap_handle *out);
    GCAP_API gcap_status_t gcap_open2(gcap_handle h, int device_index);
    gcap_status_t gcap_set_profile(gcap_handle h, const gcap_profile_t *prof);
    gcap_status_t gcap_set_buffers(gcap_handle h, int count, size_t bytes_hint);
    gcap_status_t gcap_set_callbacks(gcap_handle h, gcap_on_video_cb vcb, gcap_on_error_cb ecb, void *user);
    GCAP_API gcap_status_t gcap_set_frame_packet_callback(gcap_handle h, gcap_on_frame_packet_cb cb, void *user);
    gcap_status_t gcap_start(gcap_handle h);
    gcap_status_t gcap_start_recording(gcap_handle h, const char *path_utf8);
    gcap_status_t gcap_stop_recording(gcap_handle h);
    gcap_status_t gcap_stop(gcap_handle h);
    // Enumerate WASAPI capture endpoints (microphones / capture devices)
    GCAP_API gcap_status_t gcap_enumerate_audio_devices(gcap_audio_device_t *out, int max, int *count);
    // Select which WASAPI capture endpoint to use for recording.
    // device_id_utf8 = endpoint id from gcap_enumerate_audio_devices; nullptr/"" => use system default
    GCAP_API gcap_status_t gcap_set_recording_audio_device(gcap_handle h, const char *device_id_utf8);
    gcap_status_t gcap_close(gcap_handle h);
    GCAP_API void gcap_set_backend(int backend);
    // 選擇要用哪一張 D3D11 Adapter 來做 NV12→RGBA / DXGI 管線
    // adapter_index = -1 表示使用系統預設（原本的 nullptr / default adapter）
    GCAP_API void gcap_set_d3d_adapter(int adapter_index);

    // 查詢目前 handle 實際使用中的 backend。
    // 非 Auto 模式下通常等於 gcap_set_backend() 指定值；Auto 模式下則可能回傳 WinMF GPU / WinMF CPU / DShow。
    GCAP_API int gcap_get_active_backend(gcap_handle h);

    // --- OBS-like "Properties" ---
    gcap_status_t gcap_get_device_props(gcap_handle h, gcap_device_props_t *out);
    gcap_status_t gcap_get_signal_status(gcap_handle h, gcap_signal_status_t *out);
    GCAP_API gcap_status_t gcap_get_runtime_info(gcap_handle h, gcap_runtime_info_t *out);
    gcap_status_t gcap_set_processing(gcap_handle h, const gcap_processing_opts_t *opts);

    // Apply ProcAmp on CPU conversion path (NV12/YUY2->ARGB).
    // Passing nullptr resets to neutral.
    GCAP_API gcap_status_t gcap_set_procamp(gcap_handle h, const gcap_procamp_t *p);

    // 回傳系統可用的 audio capture device 數量
    GCAP_API int gcap_get_audio_device_count(void);

    // 列舉 audio capture devices
    // 回傳實際寫入的數量
    GCAP_API int gcap_enum_audio_devices(gcap_audio_device_t *out_devices, int max_devices);

    const char *gcap_strerror(gcap_status_t);
    GCAP_API gcap_status_t gcap_set_preview(gcap_handle h, const gcap_preview_desc_t *desc);
    // Enumerate DirectShow video format capabilities for a device index.
    // Returns actual written count. Pass nullptr or max_caps<=0 to query supported count only.
    GCAP_API int gcap_enum_video_caps(int device_index, gcap_video_cap_t *out_caps, int max_caps);
    // Enumerate available DirectShow property pages (filter + capture pin) for a device index.
    // Returns actual written count. Pass nullptr or max_pages<=0 to query supported count only.
    GCAP_API int gcap_enum_property_pages(int device_index, gcap_property_page_t *out_pages, int max_pages);

    // Open a device-specific vendor DShow property page for a device index (optional test / debug helper; not used by generic capture flow).
    GCAP_API int gcap_open_vendor_property_page(int device_index);
    GCAP_API int gcap_open_named_property_page(int device_index, const char *page_name_utf8, int capture_pin);

#ifdef __cplusplus
}
#endif
