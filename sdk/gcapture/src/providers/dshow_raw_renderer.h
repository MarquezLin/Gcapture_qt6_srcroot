#pragma once

#include <windows.h>
#include <dshow.h>
#include <stdint.h>
#include <vector>
#include <mutex>

class DShowRawRenderer
{
public:
    DShowRawRenderer();
    ~DShowRawRenderer();

    void reset();
    void setNegotiated(const GUID &subtype, int width, int height, int fpsNum, int fpsDen);
    bool isSupportedSubtype() const;
    GUID negotiatedFormat() const;
    const char *negotiatedSubtypeName() const;
    static const char *subtypeName(const GUID &g);

    // DS13 custom raw sink core API:
    // The future DirectShow custom renderer filter will call pushSample() from
    // its IMemInputPin/Receive() path after media type negotiation is complete.
    bool pushSample(const uint8_t *data, size_t bytes, int sampleStride = 0);

    bool hasFrame() const;
    uint64_t sampleCount() const;
    size_t lastSampleBytes() const;
    HANDLE frameReadyEvent() const;
    bool copyLatestRaw(std::vector<uint8_t> &out, int &w, int &h, int &stride, GUID &subtype) const;
    bool copyLatestFrameToArgb(std::vector<uint8_t> &out, int &w, int &h, int &stride) const;
    double runtimeFpsAvg() const;

private:
    static uint8_t clampByte(int v);
    static void nv12ToArgb(const uint8_t *src, int width, int height, int srcStride, std::vector<uint8_t> &dst, int &dstStride);
    static void yuy2ToArgb(const uint8_t *src, int width, int height, int srcStride, std::vector<uint8_t> &dst, int &dstStride);
    static void y210ToArgb(const uint8_t *src, int width, int height, int srcStride, std::vector<uint8_t> &dst, int &dstStride);

private:
    GUID subtype_ = MEDIASUBTYPE_NULL;
    int width_ = 0;
    int height_ = 0;
    int fpsNum_ = 0;
    int fpsDen_ = 0;

    mutable std::mutex sampleMtx_;
    std::vector<uint8_t> latestSample_;
    int latestStride_ = 0;
    uint64_t sampleCount_ = 0;
    size_t lastSampleBytes_ = 0;
    uint64_t lastSamplePtsNs_ = 0;
    double runtimeFpsAvg_ = 0.0;
    HANDLE frameReadyEvent_ = nullptr;
};
