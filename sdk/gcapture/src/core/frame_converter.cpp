// frame_converter.cpp
#include "frame_converter.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

static constexpr float kPi = 3.14159265358979323846f;

static inline void yuv_to_rgb(int Y, int U, int V, uint8_t &R, uint8_t &G, uint8_t &B)
{
    int C = Y - 16;
    int D = U - 128;
    int E = V - 128;
    int r = (298 * C + 409 * E + 128) >> 8;
    int g = (298 * C - 100 * D - 208 * E + 128) >> 8;
    int b = (298 * C + 516 * D + 128) >> 8;
    R = (uint8_t)std::clamp(r, 0, 255);
    G = (uint8_t)std::clamp(g, 0, 255);
    B = (uint8_t)std::clamp(b, 0, 255);
}

// Hue: rotate chroma (U/V) around 128.
static inline void apply_hue_to_uv(uint8_t &U, uint8_t &V, const gcap::ProcAmpParams &p)
{
    // Map 0..255 with 128 neutral to angle [-pi, +pi]
    const float ang = (float(p.hue) - 128.0f) * (kPi / 128.0f);
    const float cs = std::cos(ang);
    const float sn = std::sin(ang);

    const float u = float(U) - 128.0f;
    const float v = float(V) - 128.0f;

    const int u2 = int(u * cs - v * sn + 128.0f + 0.5f);
    const int v2 = int(u * sn + v * cs + 128.0f + 0.5f);

    U = (uint8_t)std::clamp(u2, 0, 255);
    V = (uint8_t)std::clamp(v2, 0, 255);
}

// Brightness/Contrast/Saturation in RGB domain.
static inline void apply_bcs_rgb(uint8_t &R, uint8_t &G, uint8_t &B, const gcap::ProcAmpParams &p)
{
    int r = R, g = G, b = B;

    // Brightness: [-255..+255] roughly
    const float br = (float(p.brightness) - 128.0f) / 128.0f * 255.0f;
    // Contrast: [0..~2]
    const float ct = float(p.contrast) / 128.0f;

    r = int((r - 128) * ct + 128 + br);
    g = int((g - 128) * ct + 128 + br);
    b = int((b - 128) * ct + 128 + br);

    // Saturation
    const float sat = float(p.saturation) / 128.0f;
    const int gray = (r + g + b) / 3;
    r = int(gray + (r - gray) * sat);
    g = int(gray + (g - gray) * sat);
    b = int(gray + (b - gray) * sat);

    R = (uint8_t)std::clamp(r, 0, 255);
    G = (uint8_t)std::clamp(g, 0, 255);
    B = (uint8_t)std::clamp(b, 0, 255);
}

// Simple 3x3 unsharp mask (ARGB/BGRA buffer).
// sharpness: 128 neutral; >128 sharpen; <128 soften.
static void apply_sharpness_bgra(uint8_t *bgra, int w, int h, int stride, int sharpness)
{
    if (!bgra || w <= 2 || h <= 2)
        return;
    if (sharpness == 128)
        return;

    // amount in [-1.0..+1.0] approx
    const float amt = (float(sharpness) - 128.0f) / 128.0f;

    std::vector<uint8_t> src((size_t)h * (size_t)stride);
    std::memcpy(src.data(), bgra, (size_t)h * (size_t)stride);

    auto at = [&](int x, int y, int c) -> int
    {
        x = std::clamp(x, 0, w - 1);
        y = std::clamp(y, 0, h - 1);
        return src[(size_t)y * (size_t)stride + (size_t)x * 4 + (size_t)c];
    };

    for (int y = 0; y < h; ++y)
    {
        uint8_t *dstRow = bgra + (size_t)y * (size_t)stride;
        for (int x = 0; x < w; ++x)
        {
            // 3x3 box blur
            int sumB = 0, sumG = 0, sumR = 0;
            for (int ky = -1; ky <= 1; ++ky)
            {
                for (int kx = -1; kx <= 1; ++kx)
                {
                    sumB += at(x + kx, y + ky, 0);
                    sumG += at(x + kx, y + ky, 1);
                    sumR += at(x + kx, y + ky, 2);
                }
            }
            const int blurB = (sumB + 4) / 9;
            const int blurG = (sumG + 4) / 9;
            const int blurR = (sumR + 4) / 9;

            const int origB = at(x, y, 0);
            const int origG = at(x, y, 1);
            const int origR = at(x, y, 2);

            int outB = int(origB + amt * float(origB - blurB));
            int outG = int(origG + amt * float(origG - blurG));
            int outR = int(origR + amt * float(origR - blurR));

            dstRow[x * 4 + 0] = (uint8_t)std::clamp(outB, 0, 255);
            dstRow[x * 4 + 1] = (uint8_t)std::clamp(outG, 0, 255);
            dstRow[x * 4 + 2] = (uint8_t)std::clamp(outR, 0, 255);
            dstRow[x * 4 + 3] = 255;
        }
    }
}

void gcap::nv12_to_argb(const uint8_t *y, const uint8_t *uv,
                        int w, int h, int yStride, int uvStride,
                        uint8_t *out, int outStride)
{
    ProcAmpParams p; // neutral
    nv12_to_argb(y, uv, w, h, yStride, uvStride, out, outStride, p);
}

void gcap::nv12_to_argb(const uint8_t *y, const uint8_t *uv,
                        int w, int h, int yStride, int uvStride,
                        uint8_t *out, int outStride,
                        const ProcAmpParams &p)
{
    for (int j = 0; j < h; ++j)
    {
        const uint8_t *yRow = y + j * yStride;
        const uint8_t *uvRow = uv + (j / 2) * uvStride;
        uint8_t *dst = out + j * outStride;

        for (int i = 0; i < w; i += 2)
        {
            uint8_t Y1 = yRow[i], Y2 = yRow[i + 1];
            uint8_t U = uvRow[i], V = uvRow[i + 1];

            if (p.hue != 128)
                apply_hue_to_uv(U, V, p);

            uint8_t r, g, b;

            yuv_to_rgb(Y1, U, V, r, g, b);
            if (p.brightness != 128 || p.contrast != 128 || p.saturation != 128)
                apply_bcs_rgb(r, g, b, p);
            dst[0] = b;
            dst[1] = g;
            dst[2] = r;
            dst[3] = 255; // BGRA

            yuv_to_rgb(Y2, U, V, r, g, b);
            if (p.brightness != 128 || p.contrast != 128 || p.saturation != 128)
                apply_bcs_rgb(r, g, b, p);
            dst[4] = b;
            dst[5] = g;
            dst[6] = r;
            dst[7] = 255;
            dst += 8;
        }
    }

    if (p.sharpness != 128)
        apply_sharpness_bgra(out, w, h, outStride, p.sharpness);
}

// ------------------------------------------------------------
// YUY2 → ARGB
// ------------------------------------------------------------
void gcap::yuy2_to_argb(const uint8_t *yuy2,
                        int width, int height,
                        int strideYUY2,
                        uint8_t *outARGB, int outStride)
{
    ProcAmpParams p;
    yuy2_to_argb(yuy2, width, height, strideYUY2, outARGB, outStride, p);
}

void gcap::yuy2_to_argb(const uint8_t *yuy2,
                        int width, int height,
                        int strideYUY2,
                        uint8_t *outARGB, int outStride,
                        const ProcAmpParams &p)
{
    for (int y = 0; y < height; y++)
    {
        const uint8_t *src = yuy2 + y * strideYUY2;
        uint8_t *dst = outARGB + y * outStride;

        for (int x = 0; x < width; x += 2)
        {
            int Y0 = src[0];
            uint8_t U = src[1];
            int Y1 = src[2];
            uint8_t V = src[3];

            if (p.hue != 128)
                apply_hue_to_uv(U, V, p);

            uint8_t r0, g0, b0;
            uint8_t r1, g1, b1;

            yuv_to_rgb(Y0, U, V, r0, g0, b0);
            if (p.brightness != 128 || p.contrast != 128 || p.saturation != 128)
                apply_bcs_rgb(r0, g0, b0, p);

            yuv_to_rgb(Y1, U, V, r1, g1, b1);
            if (p.brightness != 128 || p.contrast != 128 || p.saturation != 128)
                apply_bcs_rgb(r1, g1, b1, p);

            // pixel 0
            dst[0] = b0;
            dst[1] = g0;
            dst[2] = r0;
            dst[3] = 255; // BGRA

            // pixel 1
            dst[4] = b1;
            dst[5] = g1;
            dst[6] = r1;
            dst[7] = 255;

            src += 4;
            dst += 8;
        }
    }

    if (p.sharpness != 128)
        apply_sharpness_bgra(outARGB, width, height, outStride, p.sharpness);
}
