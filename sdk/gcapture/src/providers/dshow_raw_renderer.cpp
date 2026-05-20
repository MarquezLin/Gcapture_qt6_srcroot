#include "dshow_raw_renderer.h"
#include "../core/frame_converter.h"
#include "../core/logging.h"

#include <algorithm>
#include <cstring>
#include <mfapi.h>
#include <chrono>
#include <atomic>

namespace
{

    static DWORD makeFourcc(char a, char b, char c, char d)
    {
        return (static_cast<DWORD>(static_cast<unsigned char>(a))      ) |
               (static_cast<DWORD>(static_cast<unsigned char>(b)) <<  8) |
               (static_cast<DWORD>(static_cast<unsigned char>(c)) << 16) |
               (static_cast<DWORD>(static_cast<unsigned char>(d)) << 24);
    }

    static bool isDShowSubtypeFourcc(const GUID &g, DWORD fourcc)
    {
        return g.Data1 == fourcc &&
               g.Data2 == 0x0000 &&
               g.Data3 == 0x0010 &&
               g.Data4[0] == 0x80 &&
               g.Data4[1] == 0x00 &&
               g.Data4[2] == 0x00 &&
               g.Data4[3] == 0xAA &&
               g.Data4[4] == 0x00 &&
               g.Data4[5] == 0x38 &&
               g.Data4[6] == 0x9B &&
               g.Data4[7] == 0x71;
    }

    static bool isHDYC(const GUID &g) { return isDShowSubtypeFourcc(g, makeFourcc('H', 'D', 'Y', 'C')); }
    static bool isUYVY(const GUID &g) { return isDShowSubtypeFourcc(g, makeFourcc('U', 'Y', 'V', 'Y')); }
    static bool isV210(const GUID &g) { return isDShowSubtypeFourcc(g, makeFourcc('v', '2', '1', '0')); }
    static bool isR210(const GUID &g) { return isDShowSubtypeFourcc(g, makeFourcc('r', '2', '1', '0')); }

    static inline uint8_t clampByteLocal(int vv)
    {
        if (vv < 0) return 0;
        if (vv > 255) return 255;
        return static_cast<uint8_t>(vv);
    }

    // limited-range YUV -> RGB. HDYC is UYVY byte layout with HD/BT.709 colorimetry.
    static inline void yuvToRgb601(int y, int u, int v, uint8_t &b, uint8_t &g, uint8_t &r)
    {
        const int c = y - 16;
        const int d = u - 128;
        const int e = v - 128;
        const int rr = (298 * c + 409 * e + 128) >> 8;
        const int gg = (298 * c - 100 * d - 208 * e + 128) >> 8;
        const int bb = (298 * c + 516 * d + 128) >> 8;
        r = clampByteLocal(rr);
        g = clampByteLocal(gg);
        b = clampByteLocal(bb);
    }

    static inline void yuvToRgb709(int y, int u, int v, uint8_t &b, uint8_t &g, uint8_t &r)
    {
        const int c = y - 16;
        const int d = u - 128;
        const int e = v - 128;
        const int rr = (298 * c + 459 * e + 128) >> 8;
        const int gg = (298 * c -  55 * d - 136 * e + 128) >> 8;
        const int bb = (298 * c + 541 * d + 128) >> 8;
        r = clampByteLocal(rr);
        g = clampByteLocal(gg);
        b = clampByteLocal(bb);
    }

    struct ArgbStats
    {
        int minR = 255, minG = 255, minB = 255;
        int maxR = 0, maxG = 0, maxB = 0;
        uint64_t nonBlack = 0;
    };

    static ArgbStats computeArgbStats(const std::vector<uint8_t> &argb)
    {
        ArgbStats st;
        if (argb.empty())
        {
            st.minR = st.minG = st.minB = 0;
            return st;
        }
        const size_t pixels = argb.size() / 4;
        for (size_t i = 0; i < pixels; ++i)
        {
            const uint8_t b = argb[i * 4 + 0];
            const uint8_t g = argb[i * 4 + 1];
            const uint8_t r = argb[i * 4 + 2];
            st.minR = (std::min)(st.minR, static_cast<int>(r));
            st.minG = (std::min)(st.minG, static_cast<int>(g));
            st.minB = (std::min)(st.minB, static_cast<int>(b));
            st.maxR = (std::max)(st.maxR, static_cast<int>(r));
            st.maxG = (std::max)(st.maxG, static_cast<int>(g));
            st.maxB = (std::max)(st.maxB, static_cast<int>(b));
            if (r || g || b)
                ++st.nonBlack;
        }
        return st;
    }
}

const char *DShowRawRenderer::subtypeName(const GUID &g)
{
    if (g == MEDIASUBTYPE_NV12 || g == MFVideoFormat_NV12) return "NV12";
    if (g == MFVideoFormat_P010) return "P010";
    if (g == MEDIASUBTYPE_YUY2 || g == MFVideoFormat_YUY2) return "YUY2";
    if (g == MEDIASUBTYPE_Y210 || g == MFVideoFormat_Y210) return "Y210";
    if (g == MEDIASUBTYPE_MJPG) return "MJPG";
    if (isHDYC(g)) return "HDYC";
    if (isUYVY(g)) return "UYVY";
    if (isV210(g)) return "v210";
    if (isR210(g)) return "r210";
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
    lastSamplePtsNs_ = 0;
    runtimeFpsAvg_ = 0.0;
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
        lastSamplePtsNs_ = 0;
        runtimeFpsAvg_ = 0.0;
    }
}

bool DShowRawRenderer::isSupportedSubtype() const
{
    return subtype_ == MEDIASUBTYPE_NV12 || subtype_ == MFVideoFormat_P010 || subtype_ == MEDIASUBTYPE_YUY2 || subtype_ == MEDIASUBTYPE_Y210 ||
           isV210(subtype_) ||
           isHDYC(subtype_) || isUYVY(subtype_) ||
           subtype_ == MEDIASUBTYPE_RGB24 || subtype_ == MEDIASUBTYPE_RGB32 || subtype_ == MEDIASUBTYPE_ARGB32;
}

GUID DShowRawRenderer::negotiatedFormat() const
{
    if (subtype_ == MEDIASUBTYPE_NV12) return MEDIASUBTYPE_NV12;
    if (subtype_ == MFVideoFormat_P010) return MFVideoFormat_P010;
    if (subtype_ == MEDIASUBTYPE_YUY2) return MEDIASUBTYPE_YUY2;
    if (isHDYC(subtype_) || isUYVY(subtype_)) return subtype_;
    if (subtype_ == MEDIASUBTYPE_Y210) return MEDIASUBTYPE_Y210;
    if (isV210(subtype_)) return subtype_;
    if (subtype_ == MEDIASUBTYPE_RGB24) return MEDIASUBTYPE_RGB24;
    if (subtype_ == MEDIASUBTYPE_RGB32) return MEDIASUBTYPE_RGB32;
    if (subtype_ == MEDIASUBTYPE_ARGB32) return MEDIASUBTYPE_ARGB32;
    return MEDIASUBTYPE_NULL;
}

bool DShowRawRenderer::negotiatedInfo(GUID &subtype, int &width, int &height, int &fpsNum, int &fpsDen) const
{
    std::lock_guard<std::mutex> lock(sampleMtx_);
    if (subtype_ == MEDIASUBTYPE_NULL || width_ <= 0 || height_ <= 0)
        return false;
    subtype = subtype_;
    width = width_;
    height = height_;
    fpsNum = fpsNum_;
    fpsDen = fpsDen_;
    return true;
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

    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const uint64_t nowNs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    if (lastSamplePtsNs_ != 0 && nowNs > lastSamplePtsNs_)
    {
        const double fpsNow = 1e9 / double(nowNs - lastSamplePtsNs_);
        if (fpsNow > 0.0)
            runtimeFpsAvg_ = (runtimeFpsAvg_ <= 0.0) ? fpsNow : (runtimeFpsAvg_ * 0.9 + fpsNow * 0.1);
    }
    lastSamplePtsNs_ = nowNs;
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
    else if (subtype_ == MEDIASUBTYPE_YUY2 || isHDYC(subtype_) || isUYVY(subtype_))
    {
        stride = width_ * 2;
    }
    else if (subtype_ == MEDIASUBTYPE_Y210)
    {
        stride = width_ * 4;
    }
    else if (isV210(subtype_))
    {
        stride = ((width_ + 5) / 6) * 16;
    }
    else if (subtype_ == MFVideoFormat_P010)
    {
        // P010 is 4:2:0, 16 bits per sample; one luma row is width * 2 bytes.
        stride = width_ * 2;
    }
    else if (subtype_ == MEDIASUBTYPE_RGB24)
    {
        stride = width_ * 3;
    }
    else if (subtype_ == MEDIASUBTYPE_RGB32 || subtype_ == MEDIASUBTYPE_ARGB32)
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
    if (isHDYC(subtype) || isUYVY(subtype))
    {
        uyvyToArgb(raw.data(), w, h, stride, out, stride);
        return true;
    }
    if (subtype == MEDIASUBTYPE_Y210)
    {
        y210ToArgb(raw.data(), w, h, stride, out, stride);
        return true;
    }
    if (isV210(subtype))
    {
        v210ToArgb(raw.data(), w, h, stride, out, stride);
        return true;
    }
    if (subtype == MEDIASUBTYPE_RGB24)
    {
        rgb24ToArgb(raw.data(), w, h, stride, out, stride);
        return true;
    }
    if (subtype == MEDIASUBTYPE_RGB32 || subtype == MEDIASUBTYPE_ARGB32)
    {
        bgraToArgb(raw.data(), w, h, stride, out, stride);
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
            yuvToRgb601(Y, U, V, b, g, r);
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
            yuvToRgb601(y0, u, v, b, g, r);
            uint8_t *p0 = dstRow + static_cast<size_t>(x) * 4;
            p0[0] = b; p0[1] = g; p0[2] = r; p0[3] = 255;

            if (x + 1 < width)
            {
                yuvToRgb601(y1, u, v, b, g, r);
                uint8_t *p1 = dstRow + static_cast<size_t>(x + 1) * 4;
                p1[0] = b; p1[1] = g; p1[2] = r; p1[3] = 255;
            }
        }
    }
}


void DShowRawRenderer::uyvyToArgb(const uint8_t *src, int width, int height, int srcStride, std::vector<uint8_t> &dst, int &dstStride)
{
    dstStride = width * 4;
    dst.resize(static_cast<size_t>(dstStride) * static_cast<size_t>(height));

    // UYVY and HDYC share byte order: U0 Y0 V0 Y1.
    // HDYC should use BT.709. For plain UYVY, use BT.709 for HD sizes and BT.601 for SD sizes.
    const bool use709 = (width >= 1280 || height >= 720);

    for (int y = 0; y < height; ++y)
    {
        const uint8_t *srcRow = src + static_cast<size_t>(y) * srcStride;
        uint8_t *dstRow = dst.data() + static_cast<size_t>(y) * dstStride;
        for (int x = 0; x < width; x += 2)
        {
            const uint8_t u  = srcRow[x * 2 + 0];
            const uint8_t y0 = srcRow[x * 2 + 1];
            const uint8_t v  = srcRow[x * 2 + 2];
            const uint8_t y1 = srcRow[x * 2 + 3];

            uint8_t b = 0, g = 0, r = 0;
            if (use709) yuvToRgb709(y0, u, v, b, g, r);
            else        yuvToRgb601(y0, u, v, b, g, r);
            uint8_t *p0 = dstRow + static_cast<size_t>(x) * 4;
            p0[0] = b; p0[1] = g; p0[2] = r; p0[3] = 255;

            if (x + 1 < width)
            {
                if (use709) yuvToRgb709(y1, u, v, b, g, r);
                else        yuvToRgb601(y1, u, v, b, g, r);
                uint8_t *p1 = dstRow + static_cast<size_t>(x + 1) * 4;
                p1[0] = b; p1[1] = g; p1[2] = r; p1[3] = 255;
            }
        }
    }

    static std::atomic<unsigned> s_uyvyConvertLogs{0};
    const unsigned n = ++s_uyvyConvertLogs;
    if (n <= 6)
    {
        const ArgbStats st = computeArgbStats(dst);
        char msg[384] = {};
        sprintf_s(msg,
                  "[DShow][Convert] UYVY/HDYC->ARGB byteOrder=U0Y0V0Y1 matrix=%s size=%dx%d srcStride=%d dstStride=%d R=%d..%d G=%d..%d B=%d..%d nonBlack=%llu",
                  use709 ? "BT.709" : "BT.601",
                  width, height, srcStride, dstStride,
                  st.minR, st.maxR, st.minG, st.maxG, st.minB, st.maxB,
                  static_cast<unsigned long long>(st.nonBlack));
        gcap_log_debug(msg);
    }
}

void DShowRawRenderer::y210ToArgb(const uint8_t *src, int width, int height, int srcStride, std::vector<uint8_t> &dst, int &dstStride)
{
    dstStride = width * 4;
    dst.resize(static_cast<size_t>(dstStride) * static_cast<size_t>(height));

    gcap::y210_to_argb(src, width, height, srcStride, dst.data(), dstStride);
}

void DShowRawRenderer::v210ToArgb(const uint8_t *src, int width, int height, int srcStride, std::vector<uint8_t> &dst, int &dstStride)
{
    dstStride = width * 4;
    dst.resize(static_cast<size_t>(dstStride) * static_cast<size_t>(height));

    gcap::v210_to_argb(src, width, height, srcStride, dst.data(), dstStride);
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



void DShowRawRenderer::rgb24ToArgb(const uint8_t *src, int width, int height, int srcStride, std::vector<uint8_t> &dst, int &dstStride)
{
    dstStride = width * 4;
    dst.resize(static_cast<size_t>(dstStride) * static_cast<size_t>(height));

    // DShow RGB24 samples are commonly delivered bottom-up. Flip rows here so
    // preview/callback ARGB becomes top-down for Qt / D3D upload.
    for (int y = 0; y < height; ++y)
    {
        const uint8_t *srcRow = src + static_cast<size_t>(height - 1 - y) * static_cast<size_t>(srcStride);
        uint8_t *dstRow = dst.data() + static_cast<size_t>(y) * static_cast<size_t>(dstStride);
        for (int x = 0; x < width; ++x)
        {
            const uint8_t *s = srcRow + static_cast<size_t>(x) * 3;
            uint8_t *d = dstRow + static_cast<size_t>(x) * 4;
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = 255;
        }
    }
}

void DShowRawRenderer::bgraToArgb(const uint8_t *src, int width, int height, int srcStride, std::vector<uint8_t> &dst, int &dstStride)
{
    dstStride = width * 4;
    dst.resize(static_cast<size_t>(dstStride) * static_cast<size_t>(height));

    // DShow RGB32/ARGB32 samples are commonly delivered as bottom-up DIB rows.
    // Keep preview/callback output top-down, same as rgb24ToArgb().
    for (int y = 0; y < height; ++y)
    {
        const uint8_t *srcRow = src + static_cast<size_t>(height - 1 - y) * static_cast<size_t>(srcStride);
        uint8_t *dstRow = dst.data() + static_cast<size_t>(y) * static_cast<size_t>(dstStride);
        std::memcpy(dstRow, srcRow, static_cast<size_t>(dstStride));
    }
}

double DShowRawRenderer::runtimeFpsAvg() const
{
    std::lock_guard<std::mutex> lock(sampleMtx_);
    return runtimeFpsAvg_;
}
