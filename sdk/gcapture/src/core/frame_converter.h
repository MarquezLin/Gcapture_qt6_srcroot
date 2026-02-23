// frame_converter.h
#pragma once
#include <cstdint>

namespace gcap
{
    /**
     * ProcAmp (processing amplifier) parameters.
     * UI uses 0~255, where 128 means "neutral".
     */
    struct ProcAmpParams
    {
        int brightness = 128; // 0..255, 128 neutral
        int contrast = 128;   // 0..255, 128 neutral
        int hue = 128;        // 0..255, 128 neutral
        int saturation = 128; // 0..255, 128 neutral
        int sharpness = 128;  // 0..255, 128 neutral
    };

    // NV12 → ARGB
    void nv12_to_argb(const uint8_t *y, const uint8_t *uv,
                      int width, int height, int yStride, int uvStride,
                      uint8_t *outARGB, int outStride);

    // NV12 → ARGB + ProcAmp
    void nv12_to_argb(const uint8_t *y, const uint8_t *uv,
                      int width, int height, int yStride, int uvStride,
                      uint8_t *outARGB, int outStride,
                      const ProcAmpParams &p);

    // YUY2 → ARGB
    void yuy2_to_argb(const uint8_t *yuy2,
                      int width, int height, int yuy2Stride,
                      uint8_t *outARGB, int outStride);

    // YUY2 → ARGB + ProcAmp
    void yuy2_to_argb(const uint8_t *yuy2,
                      int width, int height, int yuy2Stride,
                      uint8_t *outARGB, int outStride,
                      const ProcAmpParams &p);
}
