#include "dshow_raw_renderer.h"

#include <algorithm>
#include <cstring>

namespace
{
    static inline void yuvToRgb(int y, int u, int v, uint8_t &b, uint8_t &g, uint8_t &r)
    {
        const int c = y - 16;
        const int d = u - 128;
        const int e = v - 128;

        const int rr = (298 * c + 409 * e + 128) >> 8;
        const int gg = (298 * c - 100 * d - 208 * e + 128) >> 8;
        const int bb = (298 * c + 516 * d + 128) >> 8;

        auto clampByteLocal = [](int vv) -> uint8_t {
            if (vv < 0) return 0;
            if (vv > 255) return 255;
            return static_cast<uint8_t>(vv);
        };

        r = clampByteLocal(rr);
        g = clampByteLocal(gg);
        b = clampByteLocal(bb);
    }
}

const char *DShowRawRenderer::subtypeName(const GUID &g)
{
    if (g == MEDIASUBTYPE_NV12) return "NV12";
    if (g == MEDIASUBTYPE_YUY2) return "YUY2";
    if (g == MEDIASUBTYPE_Y210) return "Y210";
    if (g == MEDIASUBTYPE_MJPG) return "MJPG";
    if (g == MEDIASUBTYPE_RGB24) return "RGB24";
    if (g == MEDIASUBTYPE_RGB32) return "RGB32";
    if (g == MEDIASUBTYPE_ARGB32) return "ARGB32";
    return "UNKNOWN";
}


DShowRawRenderer::DShowRawRenderer()
{
    frameReadyEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

DShowRawRenderer::~DShowRawRenderer()
{
    if (frameReadyEvent_)
    {
        CloseHandle(frameReadyEvent_);
        frameReadyEvent_ = nullptr;
    }
}

uint8_t DShowRawRenderer::clampByte(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return static_cast<uint8_t>(v);
}

void DShowRawRenderer::reset()
{
    std::lock_guard<std::mutex> lock(sampleMtx_);
    subtype_ = MEDIASUBTYPE_NULL;
    width_ = 0;
    height_ = 0;
    fpsNum_ = 0;
    fpsDen_ = 0;
    latestSample_.clear();
    latestStride_ = 0;
    sampleCount_ = 0;
    lastSampleBytes_ = 0;
    if (frameReadyEvent_)
        SetEvent(frameReadyEvent_);
}

void DShowRawRenderer::setNegotiated(const GUID &subtype, int width, int height, int fpsNum, int fpsDen)
{
    std::lock_guard<std::mutex> lock(sampleMtx_);
    const bool sameFormat = (subtype_ == subtype && width_ == width && height_ == height &&
                             fpsNum_ == fpsNum && fpsDen_ == fpsDen);
    subtype_ = subtype;
    width_ = width;
    height_ = height;
    fpsNum_ = fpsNum;
    fpsDen_ = fpsDen;
    if (!sameFormat)
    {
        latestSample_.clear();
        latestStride_ = 0;
        sampleCount_ = 0;
        lastSampleBytes_ = 0;
    }
}

bool DShowRawRenderer::isSupportedSubtype() const
{
    return subtype_ == MEDIASUBTYPE_NV12 || subtype_ == MEDIASUBTYPE_YUY2 || subtype_ == MEDIASUBTYPE_Y210;
}

GUID DShowRawRenderer::negotiatedFormat() const
{
    if (subtype_ == MEDIASUBTYPE_NV12) return MEDIASUBTYPE_NV12;
    if (subtype_ == MEDIASUBTYPE_YUY2) return MEDIASUBTYPE_YUY2;
    if (subtype_ == MEDIASUBTYPE_Y210) return MEDIASUBTYPE_Y210;
    return MEDIASUBTYPE_NULL;
}

const char *DShowRawRenderer::negotiatedSubtypeName() const
{
    return subtypeName(subtype_);
}

bool DShowRawRenderer::pushSample(const uint8_t *data, size_t bytes, int sampleStride)
{
    if (!data || bytes == 0 || width_ <= 0 || height_ <= 0 || !isSupportedSubtype())
        return false;

    std::lock_guard<std::mutex> lock(sampleMtx_);
    latestSample_.assign(data, data + bytes);
    latestStride_ = sampleStride;
    sampleCount_ += 1;
    lastSampleBytes_ = bytes;
    if (frameReadyEvent_)
        SetEvent(frameReadyEvent_);
    return true;
}

bool DShowRawRenderer::hasFrame() const
{
    std::lock_guard<std::mutex> lock(sampleMtx_);
    return !latestSample_.empty();
}

bool DShowRawRenderer::copyLatestRaw(std::vector<uint8_t> &out, int &w, int &h, int &stride, GUID &subtype) const
{
    std::lock_guard<std::mutex> lock(sampleMtx_);
    if (latestSample_.empty() || width_ <= 0 || height_ <= 0)
        return false;

    out = latestSample_;
    w = width_;
    h = height_;
    subtype = subtype_;
    if (latestStride_ > 0)
    {
        stride = latestStride_;
    }
    else if (subtype_ == MEDIASUBTYPE_NV12)
    {
        stride = width_;
    }
    else if (subtype_ == MEDIASUBTYPE_YUY2)
    {
        stride = width_ * 2;
    }
    else if (subtype_ == MEDIASUBTYPE_Y210)
    {
        stride = width_ * 4;
    }
    else
    {
        stride = 0;
    }
    return true;
}

bool DShowRawRenderer::copyLatestFrameToArgb(std::vector<uint8_t> &out, int &w, int &h, int &stride) const
{
    std::vector<uint8_t> raw;
    GUID subtype = MEDIASUBTYPE_NULL;
    if (!copyLatestRaw(raw, w, h, stride, subtype))
        return false;

    if (subtype == MEDIASUBTYPE_NV12)
    {
        nv12ToArgb(raw.data(), w, h, stride, out, stride);
        return true;
    }
    if (subtype == MEDIASUBTYPE_YUY2)
    {
        yuy2ToArgb(raw.data(), w, h, stride, out, stride);
        return true;
    }
    if (subtype == MEDIASUBTYPE_Y210)
    {
        y210ToArgb(raw.data(), w, h, stride, out, stride);
        return true;
    }
    return false;
}

void DShowRawRenderer::nv12ToArgb(const uint8_t *src, int width, int height, int srcStride, std::vector<uint8_t> &dst, int &dstStride)
{
    dstStride = width * 4;
    dst.resize(static_cast<size_t>(dstStride) * static_cast<size_t>(height));

    const uint8_t *yPlane = src;
    const uint8_t *uvPlane = src + static_cast<size_t>(srcStride) * static_cast<size_t>(height);

    for (int y = 0; y < height; ++y)
    {
        const uint8_t *yRow = yPlane + static_cast<size_t>(y) * srcStride;
        const uint8_t *uvRow = uvPlane + static_cast<size_t>(y / 2) * srcStride;
        uint8_t *dstRow = dst.data() + static_cast<size_t>(y) * dstStride;
        for (int x = 0; x < width; ++x)
        {
            const int Y = yRow[x];
            const int U = uvRow[(x & ~1) + 0];
            const int V = uvRow[(x & ~1) + 1];
            uint8_t b = 0, g = 0, r = 0;
            yuvToRgb(Y, U, V, b, g, r);
            uint8_t *p = dstRow + static_cast<size_t>(x) * 4;
            p[0] = b;
            p[1] = g;
            p[2] = r;
            p[3] = 255;
        }
    }
}

void DShowRawRenderer::yuy2ToArgb(const uint8_t *src, int width, int height, int srcStride, std::vector<uint8_t> &dst, int &dstStride)
{
    dstStride = width * 4;
    dst.resize(static_cast<size_t>(dstStride) * static_cast<size_t>(height));

    for (int y = 0; y < height; ++y)
    {
        const uint8_t *srcRow = src + static_cast<size_t>(y) * srcStride;
        uint8_t *dstRow = dst.data() + static_cast<size_t>(y) * dstStride;
        for (int x = 0; x < width; x += 2)
        {
            const uint8_t y0 = srcRow[x * 2 + 0];
            const uint8_t u  = srcRow[x * 2 + 1];
            const uint8_t y1 = srcRow[x * 2 + 2];
            const uint8_t v  = srcRow[x * 2 + 3];

            uint8_t b = 0, g = 0, r = 0;
            yuvToRgb(y0, u, v, b, g, r);
            uint8_t *p0 = dstRow + static_cast<size_t>(x) * 4;
            p0[0] = b; p0[1] = g; p0[2] = r; p0[3] = 255;

            if (x + 1 < width)
            {
                yuvToRgb(y1, u, v, b, g, r);
                uint8_t *p1 = dstRow + static_cast<size_t>(x + 1) * 4;
                p1[0] = b; p1[1] = g; p1[2] = r; p1[3] = 255;
            }
        }
    }
}

void DShowRawRenderer::y210ToArgb(const uint8_t *src, int width, int height, int srcStride, std::vector<uint8_t> &dst, int &dstStride)
{
    dstStride = width * 4;
    dst.resize(static_cast<size_t>(dstStride) * static_cast<size_t>(height));

    for (int y = 0; y < height; ++y)
    {
        const uint16_t *srcRow = reinterpret_cast<const uint16_t *>(src + static_cast<size_t>(y) * static_cast<size_t>(srcStride));
        uint8_t *dstRow = dst.data() + static_cast<size_t>(y) * static_cast<size_t>(dstStride);
        for (int x = 0; x < width; x += 2)
        {
            const int base = x * 2;
            const uint16_t y0_10 = srcRow[base + 0] & 1023u;
            const uint16_t u_10  = srcRow[base + 1] & 1023u;
            const uint16_t y1_10 = (x + 1 < width) ? (srcRow[base + 2] & 1023u) : y0_10;
            const uint16_t v_10  = (x + 1 < width) ? (srcRow[base + 3] & 1023u) : (srcRow[base + 1] & 1023u);

            const int y0 = (y0_10 * 255 + 511) / 1023;
            const int y1 = (y1_10 * 255 + 511) / 1023;
            const int u  = (u_10  * 255 + 511) / 1023;
            const int v  = (v_10  * 255 + 511) / 1023;

            uint8_t b = 0, g = 0, r = 0;
            yuvToRgb(y0, u, v, b, g, r);
            uint8_t *p0 = dstRow + static_cast<size_t>(x) * 4;
            p0[0] = b; p0[1] = g; p0[2] = r; p0[3] = 255;

            if (x + 1 < width)
            {
                yuvToRgb(y1, u, v, b, g, r);
                uint8_t *p1 = dstRow + static_cast<size_t>(x + 1) * 4;
                p1[0] = b; p1[1] = g; p1[2] = r; p1[3] = 255;
            }
        }
    }
}

uint64_t DShowRawRenderer::sampleCount() const
{
    std::lock_guard<std::mutex> lock(sampleMtx_);
    return sampleCount_;
}

size_t DShowRawRenderer::lastSampleBytes() const
{
    std::lock_guard<std::mutex> lock(sampleMtx_);
    return lastSampleBytes_;
}

HANDLE DShowRawRenderer::frameReadyEvent() const
{
    return frameReadyEvent_;
}
