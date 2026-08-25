#pragma once

/*
    gcapture audio helper API

    Normal recording users should use:
      - gcap_audio_device_count()
      - gcap_audio_enum_devices()
      - gcap_set_recording_audio_device() from gcapture.h

    The low-level gcap_start_audio_capture()/gcap_stop_audio_capture() APIs run
    a simple WASAPI preview monitor from a capture endpoint to the default
    speaker. They are independent from the recording path.
*/

#ifdef __cplusplus
extern "C"
{
#endif

#ifdef _WIN32
#ifdef GCAPTURE_BUILD
#define GCAP_API __declspec(dllexport)
#else
#define GCAP_API __declspec(dllimport)
#endif
#else
#define GCAP_API
#endif

#define GCAP_AUDIO_ID_MAX 256
#define GCAP_AUDIO_NAME_MAX 256

    /** WASAPI capture endpoint information. */
    typedef struct gcap_audio_device_t
    {
        char id[GCAP_AUDIO_ID_MAX];       /** Opaque WASAPI endpoint id, UTF-8. Use with gcap_set_recording_audio_device(). */
        char name[GCAP_AUDIO_NAME_MAX];   /** Friendly endpoint name, UTF-8. */

        int channels;                     /** Channel count when known: 1, 2, 6, etc. 0 means unknown. */
        int sample_rate;                  /** Preferred/current sample rate when known, e.g. 48000. 0 means unknown. */
        int bits_per_sample;              /** 16 / 24 / 32 when known. 0 means unknown. */
        int is_float;                     /** 1 = IEEE float, 0 = PCM or unknown. */
        int is_default;                   /** 1 = system default capture endpoint. */
    } gcap_audio_device_t;

    /** Return number of active WASAPI capture endpoints. */
    GCAP_API int gcap_audio_device_count(void);

    /**
     * Enumerate active WASAPI capture endpoints.
     *
     * Parameters:
     *   out       - Output array. If null, the function returns total count.
     *   max_count - Number of entries available in out.
     *
     * Return:
     *   Number of entries written, or total count when out is null/max_count <= 0.
     */
    GCAP_API int gcap_audio_enum_devices(
        gcap_audio_device_t *out,
        int max_count);

    /** Legacy alias for gcap_audio_device_count(). Kept for source/binary compatibility. */
    GCAP_API int gcap_get_audio_device_count(void);

    /** Legacy alias for gcap_audio_enum_devices(). Kept for source/binary compatibility. */
    GCAP_API int gcap_enum_audio_devices(
        gcap_audio_device_t *out,
        int max_count);

    /**
     * Best-effort match from a video capture device friendly name to a WASAPI
     * capture endpoint. Returns 1 and writes out when a likely endpoint is found.
     */
    GCAP_API int gcap_audio_find_device_for_capture(
        const char *capture_device_name_utf8,
        gcap_audio_device_t *out);

    /** Experimental low-level audio capture configuration. Prefer gcap_set_recording_audio_device() for recording. */
    typedef struct gcap_audio_capture_config_t
    {
        const char *device_id;            /** Endpoint id from gcap_audio_enum_devices(). null/empty may mean default. */
        int sample_rate;                  /** Requested sample rate, commonly 48000. */
        int channels;                     /** Requested channels, commonly 1 or 2. */
    } gcap_audio_capture_config_t;

    /** Start low-level WASAPI audio preview/capture from cfg->device_id to the default speaker. */
    GCAP_API int gcap_start_audio_capture(
        const gcap_audio_capture_config_t *cfg);

    /**
     * Match a DirectShow audio input filter to a video capture device name and
     * monitor its PCM output through the Windows default audio renderer.
     * out_source may be null; when supplied it receives the matched filter and
     * connected PCM format.
     */
    GCAP_API int gcap_start_dshow_audio_preview(
        const char *video_device_name_utf8,
        gcap_audio_device_t *out_source);

    /** Stop the active WASAPI or DirectShow audio preview. */
    GCAP_API void gcap_stop_audio_capture(void);

#ifdef __cplusplus
}
#endif
