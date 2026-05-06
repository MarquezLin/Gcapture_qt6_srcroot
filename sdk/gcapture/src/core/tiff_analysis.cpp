#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "gcapture.h"
#include "logging.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <propidl.h>
using Microsoft::WRL::ComPtr;
#endif

namespace
{
static void copy_text(char *dst, size_t dstSize, const char *src)
{
    if (!dst || dstSize == 0)
        return;
    if (!src)
        src = "";
    std::snprintf(dst, dstSize, "%s", src);
}

static void append_csv_u16(char *dst, size_t dstSize, const std::vector<uint16_t> &values)
{
    if (!dst || dstSize == 0)
        return;
    dst[0] = '\0';
    size_t used = 0;
    for (size_t i = 0; i < values.size(); ++i)
    {
        char tmp[32];
        std::snprintf(tmp, sizeof(tmp), "%s%u", (i == 0 ? "" : ","), unsigned(values[i]));
        const size_t n = std::strlen(tmp);
        if (used + n + 1 >= dstSize)
        {
            const char *tail = "...";
            if (used + 4 < dstSize)
                std::strncat(dst, tail, dstSize - used - 1);
            break;
        }
        std::memcpy(dst + used, tmp, n + 1);
        used += n;
    }
}

static int bit_width64(uint64_t v)
{
    int w = 0;
    while (v)
    {
        ++w;
        v >>= 1;
    }
    return w > 0 ? w : 1;
}

static int ceil_log2_64(uint64_t v)
{
    if (v <= 1)
        return 1;
    return bit_width64(v - 1);
}

static int trailing_zeros16(uint16_t v)
{
    if (v == 0)
        return 16;
    int n = 0;
    while (n < 16 && (((v >> n) & 1u) == 0u))
        ++n;
    return n;
}

struct SampleAnalysis
{
    uint64_t minValue = (std::numeric_limits<uint64_t>::max)();
    uint64_t maxValue = 0;
    uint64_t uniqueValueCount = 0;
    int effectiveBits = 0;
    bool shift10 = false;
    bool expanded8 = false;
};

static SampleAnalysis analyze_samples16(const std::vector<uint16_t> &samples, int storedBits)
{
    SampleAnalysis r;
    if (samples.empty() || storedBits <= 0)
    {
        r.minValue = 0;
        r.effectiveBits = 0;
        return r;
    }

    std::set<uint16_t> unique;
    bool allEqualHighLow = true;
    bool anyNonZero = false;
    int minTrailingZeros = 16;

    for (uint16_t v : samples)
    {
        r.minValue = std::min<uint64_t>(r.minValue, v);
        r.maxValue = std::max<uint64_t>(r.maxValue, v);
        unique.insert(v);
        if (((v >> 8) & 0xFFu) != (v & 0xFFu))
            allEqualHighLow = false;
        if (v != 0)
        {
            anyNonZero = true;
            minTrailingZeros = std::min(minTrailingZeros, trailing_zeros16(v));
        }
    }

    r.uniqueValueCount = static_cast<uint64_t>(unique.size());

    int effective = storedBits;
    effective = std::min(effective, bit_width64(r.maxValue));
    effective = std::min(effective, ceil_log2_64(r.uniqueValueCount));

    if (storedBits >= 16 && allEqualHighLow)
    {
        effective = std::min(effective, 8);
        r.expanded8 = true;
    }

    if (anyNonZero && minTrailingZeros > 0 && minTrailingZeros < storedBits)
    {
        effective = std::min(effective, storedBits - minTrailingZeros);
        if (storedBits - minTrailingZeros == 10)
            r.shift10 = true;
    }

    if (r.maxValue <= 1023)
        effective = std::min(effective, 10);
    if (r.maxValue <= 4095)
        effective = std::min(effective, 12);

    r.effectiveBits = std::max(1, std::min(effective, storedBits));
    if (r.minValue == (std::numeric_limits<uint64_t>::max)())
        r.minValue = 0;
    return r;
}

struct RampAxisStats
{
    int uniqueCount = 0;
    int pos = 0;
    int neg = 0;
    int zero = 0;
    double monotonicRatio = 0.0;
};

static RampAxisStats calc_ramp_axis_stats(const std::vector<uint16_t> &axis)
{
    RampAxisStats s;
    std::set<uint16_t> unique;
    for (uint16_t v : axis)
        unique.insert(v);
    for (size_t i = 1; i < axis.size(); ++i)
    {
        const int d = int(axis[i]) - int(axis[i - 1]);
        if (d > 0) ++s.pos;
        else if (d < 0) ++s.neg;
        else ++s.zero;
    }
    s.uniqueCount = static_cast<int>(unique.size());
    const int nonZero = s.pos + s.neg;
    s.monotonicRatio = nonZero > 0 ? double(std::max(s.pos, s.neg)) / double(nonZero) : 0.0;
    return s;
}

static std::string format_ramp_axis_stats(const RampAxisStats &s)
{
    char buf[128];
    std::snprintf(buf, sizeof(buf), "axisUnique=%d monotonicRatio=%.3f zeroSteps=%d", s.uniqueCount, s.monotonicRatio, s.zero);
    return buf;
}

static bool looks_like_gray_ramp(const std::vector<uint16_t> &axis, std::string &reason)
{
    if (axis.size() < 32)
    {
        reason = "axis too short";
        return false;
    }
    const RampAxisStats s = calc_ramp_axis_stats(axis);
    reason = format_ramp_axis_stats(s);
    const bool monotonic = s.monotonicRatio >= 0.98;
    const bool enoughUnique = s.uniqueCount >= std::min<int>(int(axis.size()) / 2, 512);
    return monotonic && enoughUnique;
}

static bool looks_like_visual_gray_ramp(const std::vector<uint16_t> &axis, std::string &reason)
{
    if (axis.size() < 32)
    {
        reason = "axis too short";
        return false;
    }
    const RampAxisStats s = calc_ramp_axis_stats(axis);
    reason = format_ramp_axis_stats(s);
    const bool trending = s.monotonicRatio >= 0.78;
    const bool enoughUnique = s.uniqueCount >= std::min<int>(int(axis.size()) / 4, 256);
    return trending && enoughUnique;
}

static std::vector<uint16_t> extract_gray_axis_row(const std::vector<uint16_t> &gray, int width, int height)
{
    std::vector<uint16_t> out;
    if (width <= 0 || height <= 0 || gray.size() < static_cast<size_t>(width) * static_cast<size_t>(height))
        return out;
    const int y = height / 2;
    out.reserve(width);
    const size_t base = static_cast<size_t>(y) * static_cast<size_t>(width);
    for (int x = 0; x < width; ++x)
        out.push_back(gray[base + static_cast<size_t>(x)]);
    return out;
}

static std::vector<uint16_t> extract_gray_axis_col(const std::vector<uint16_t> &gray, int width, int height)
{
    std::vector<uint16_t> out;
    if (width <= 0 || height <= 0 || gray.size() < static_cast<size_t>(width) * static_cast<size_t>(height))
        return out;
    const int x = width / 2;
    out.reserve(height);
    for (int y = 0; y < height; ++y)
        out.push_back(gray[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)]);
    return out;
}

static std::vector<uint16_t> convert_axis_to_logical10(const std::vector<uint16_t> &axis, const gcap_tiff_analysis_t &report, std::string &rule)
{
    std::vector<uint16_t> out;
    out.reserve(axis.size());
    if (report.values_look_shifted_10bit)
    {
        rule = "logical10 = raw16 >> 6 (looks like shifted 10-bit in 16-bit container)";
        for (uint16_t v : axis)
            out.push_back(static_cast<uint16_t>(v >> 6));
        return out;
    }
    if (report.max_value <= 1023)
    {
        rule = "logical10 = raw16 (already within 0..1023)";
        return axis;
    }
    rule = "logical10 ~= round(raw16 * 1023 / 65535) (scaled estimate)";
    for (uint16_t v : axis)
        out.push_back(static_cast<uint16_t>((uint32_t(v) * 1023u + 32767u) / 65535u));
    return out;
}

static void fill_sampled_row_dump(const std::vector<uint16_t> &rowAxis, int height, const char *sourceName, gcap_tiff_analysis_t &report)
{
    report.sampled_row_y = height > 0 ? height / 2 : -1;
    copy_text(report.sampled_row_source, sizeof(report.sampled_row_source), sourceName);
    append_csv_u16(report.sampled_row_raw16_csv, sizeof(report.sampled_row_raw16_csv), rowAxis);
    std::string rule;
    const auto logical10 = convert_axis_to_logical10(rowAxis, report, rule);
    copy_text(report.sampled_row_logical10_rule, sizeof(report.sampled_row_logical10_rule), rule.c_str());
    append_csv_u16(report.sampled_row_logical10_csv, sizeof(report.sampled_row_logical10_csv), logical10);
}

static void fill_ramp_report(const std::vector<uint16_t> &gray, int width, int height, bool rgbNearlyEqual, gcap_tiff_analysis_t &report)
{
    const auto rowAxis = extract_gray_axis_row(gray, width, height);
    const auto colAxis = extract_gray_axis_col(gray, width, height);
    fill_sampled_row_dump(rowAxis, height, rgbNearlyEqual ? "rgba64 gray-average" : "rgba64 gray-average (non-gray RGB)", report);

    std::string rowReason, colReason, rowVisualReason, colVisualReason;
    const bool rowStrict = looks_like_gray_ramp(rowAxis, rowReason);
    const bool colStrict = looks_like_gray_ramp(colAxis, colReason);
    const bool rowVisual = looks_like_visual_gray_ramp(rowAxis, rowVisualReason);
    const bool colVisual = looks_like_visual_gray_ramp(colAxis, colVisualReason);

    report.likely_ten_bit_content = (report.effective_bit_depth >= 10 && rgbNearlyEqual && !report.values_look_8bit_expanded) ? 1 : 0;
    report.strict_ten_bit_ramp = (report.likely_ten_bit_content && (rowStrict || colStrict)) ? 1 : 0;
    report.visual_ten_bit_ramp_candidate = (report.likely_ten_bit_content && (rowVisual || colVisual)) ? 1 : 0;
    report.likely_ten_bit_ramp = report.visual_ten_bit_ramp_candidate;

    char tmp[512];
    if (rowStrict)
        std::snprintf(tmp, sizeof(tmp), "row strict ramp; grayRGB=%s; %s", rgbNearlyEqual ? "yes" : "no", rowReason.c_str());
    else if (colStrict)
        std::snprintf(tmp, sizeof(tmp), "column strict ramp; grayRGB=%s; %s", rgbNearlyEqual ? "yes" : "no", colReason.c_str());
    else
        std::snprintf(tmp, sizeof(tmp), "not strict enough; grayRGB=%s; row=%s; col=%s", rgbNearlyEqual ? "yes" : "no", rowReason.c_str(), colReason.c_str());
    copy_text(report.strict_ramp_reason, sizeof(report.strict_ramp_reason), tmp);

    if (rowVisual)
        std::snprintf(tmp, sizeof(tmp), "row visual ramp candidate; grayRGB=%s; %s", rgbNearlyEqual ? "yes" : "no", rowVisualReason.c_str());
    else if (colVisual)
        std::snprintf(tmp, sizeof(tmp), "column visual ramp candidate; grayRGB=%s; %s", rgbNearlyEqual ? "yes" : "no", colVisualReason.c_str());
    else
        std::snprintf(tmp, sizeof(tmp), "not smooth/trending enough; grayRGB=%s; row=%s; col=%s", rgbNearlyEqual ? "yes" : "no", rowVisualReason.c_str(), colVisualReason.c_str());
    copy_text(report.visual_ramp_reason, sizeof(report.visual_ramp_reason), tmp);

    std::snprintf(tmp, sizeof(tmp), "strict=%s; visual=%s", report.strict_ramp_reason, report.visual_ramp_reason);
    copy_text(report.ramp_reason, sizeof(report.ramp_reason), tmp);
}

#ifdef _WIN32
struct ScopedCoInit
{
    HRESULT hr = E_FAIL;
    bool needUninit = false;
    ScopedCoInit()
    {
        hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        needUninit = (hr == S_OK || hr == S_FALSE);
    }
    ~ScopedCoInit()
    {
        if (needUninit)
            CoUninitialize();
    }
};

static std::wstring utf8_to_wide(const char *s)
{
    if (!s || !*s)
        return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (len <= 0)
        return {};
    std::wstring out(static_cast<size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), len);
    return out;
}

static const char *wic_pixel_format_name(const WICPixelFormatGUID &fmt)
{
    if (IsEqualGUID(fmt, GUID_WICPixelFormat16bppGray)) return "16bppGray";
    if (IsEqualGUID(fmt, GUID_WICPixelFormat8bppGray)) return "8bppGray";
    if (IsEqualGUID(fmt, GUID_WICPixelFormat24bppBGR)) return "24bppBGR";
    if (IsEqualGUID(fmt, GUID_WICPixelFormat24bppRGB)) return "24bppRGB";
    if (IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA)) return "32bppBGRA";
    if (IsEqualGUID(fmt, GUID_WICPixelFormat32bppRGBA)) return "32bppRGBA";
    if (IsEqualGUID(fmt, GUID_WICPixelFormat48bppRGB)) return "48bppRGB";
    if (IsEqualGUID(fmt, GUID_WICPixelFormat48bppBGR)) return "48bppBGR";
    if (IsEqualGUID(fmt, GUID_WICPixelFormat64bppRGBA)) return "64bppRGBA";
    if (IsEqualGUID(fmt, GUID_WICPixelFormat64bppBGRA)) return "64bppBGRA";
    return "Unknown WIC format";
}

static const char *photometric_name(USHORT v)
{
    switch (v)
    {
    case 0: return "WhiteIsZero";
    case 1: return "BlackIsZero";
    case 2: return "RGB";
    case 3: return "Palette";
    case 5: return "CMYK";
    case 6: return "YCbCr";
    default: return "Unknown";
    }
}

static bool metadata_ushort(IWICMetadataQueryReader *reader, const wchar_t *query, USHORT &out)
{
    if (!reader)
        return false;
    PROPVARIANT pv;
    PropVariantInit(&pv);
    const HRESULT hr = reader->GetMetadataByName(query, &pv);
    if (FAILED(hr))
    {
        PropVariantClear(&pv);
        return false;
    }
    bool ok = false;
    if (pv.vt == VT_UI2)
    {
        out = pv.uiVal;
        ok = true;
    }
    else if (pv.vt == (VT_VECTOR | VT_UI2) && pv.caui.pElems && pv.caui.cElems > 0)
    {
        out = pv.caui.pElems[0];
        ok = true;
    }
    PropVariantClear(&pv);
    return ok;
}

static gcap_status_t open_wic_frame(const char *path_utf8,
                                    ComPtr<IWICImagingFactory> &factory,
                                    ComPtr<IWICBitmapFrameDecode> &frame,
                                    gcap_tiff_analysis_t *report)
{
    const std::wstring path = utf8_to_wide(path_utf8);
    if (path.empty())
        return GCAP_EINVAL;

    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory)
    {
        if (report) copy_text(report->error, sizeof(report->error), "Create WIC factory failed");
        return GCAP_EIO;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr) || !decoder)
    {
        if (report) copy_text(report->error, sizeof(report->error), "Open TIFF failed");
        return GCAP_EIO;
    }

    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr) || !frame)
    {
        if (report) copy_text(report->error, sizeof(report->error), "Read TIFF frame failed");
        return GCAP_EIO;
    }
    return GCAP_OK;
}

static bool copy_gray16_samples(IWICBitmapSource *src, UINT width, UINT height, std::vector<uint16_t> &samples, std::vector<uint8_t> *rgba64)
{
    const UINT stride = width * 2u;
    std::vector<uint8_t> buf(static_cast<size_t>(stride) * height);
    if (FAILED(src->CopyPixels(nullptr, stride, static_cast<UINT>(buf.size()), buf.data())))
        return false;

    const size_t pixelCount = static_cast<size_t>(width) * height;
    samples.resize(pixelCount);
    const uint16_t *p = reinterpret_cast<const uint16_t *>(buf.data());
    if (rgba64)
        rgba64->resize(pixelCount * 8u);
    uint16_t *rgba = rgba64 ? reinterpret_cast<uint16_t *>(rgba64->data()) : nullptr;
    for (size_t i = 0; i < pixelCount; ++i)
    {
        const uint16_t v = p[i];
        samples[i] = v;
        if (rgba)
        {
            rgba[i * 4 + 0] = v;
            rgba[i * 4 + 1] = v;
            rgba[i * 4 + 2] = v;
            rgba[i * 4 + 3] = 65535u;
        }
    }
    return true;
}

static bool copy_rgba64_samples(IWICBitmapSource *src, UINT width, UINT height,
                                std::vector<uint16_t> &allSamples,
                                std::vector<uint16_t> &gray,
                                bool &rgbNearlyEqual,
                                std::vector<uint8_t> *rgba64)
{
    const UINT stride = width * 8u;
    std::vector<uint8_t> buf(static_cast<size_t>(stride) * height);
    if (FAILED(src->CopyPixels(nullptr, stride, static_cast<UINT>(buf.size()), buf.data())))
        return false;

    const size_t pixelCount = static_cast<size_t>(width) * height;
    allSamples.reserve(pixelCount * 3u);
    gray.reserve(pixelCount);
    rgbNearlyEqual = true;
    if (rgba64)
        *rgba64 = buf;

    const uint16_t *p = reinterpret_cast<const uint16_t *>(buf.data());
    for (size_t i = 0; i < pixelCount; ++i)
    {
        const uint16_t r = p[i * 4 + 0];
        const uint16_t g = p[i * 4 + 1];
        const uint16_t b = p[i * 4 + 2];
        allSamples.push_back(r);
        allSamples.push_back(g);
        allSamples.push_back(b);
        gray.push_back(static_cast<uint16_t>((uint32_t(r) + uint32_t(g) + uint32_t(b)) / 3u));
        if (!(std::abs(int(r) - int(g)) <= 2 && std::abs(int(r) - int(b)) <= 2 && std::abs(int(g) - int(b)) <= 2))
            rgbNearlyEqual = false;
    }
    return true;
}
#endif
} // namespace

extern "C" GCAP_API gcap_status_t gcap_analyze_tiff(const char *path_utf8, gcap_tiff_analysis_t *out)
{
    if (!path_utf8 || !*path_utf8 || !out)
        return GCAP_EINVAL;
    std::memset(out, 0, sizeof(*out));
    copy_text(out->path, sizeof(out->path), path_utf8);
    out->sampled_row_y = -1;

#ifndef _WIN32
    copy_text(out->error, sizeof(out->error), "TIFF analyzer is only implemented on Windows/WIC in this build.");
    return GCAP_ENOTSUP;
#else
    ScopedCoInit co;
    if (FAILED(co.hr) && co.hr != RPC_E_CHANGED_MODE)
    {
        copy_text(out->error, sizeof(out->error), "CoInitializeEx failed");
        return GCAP_EIO;
    }

    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapFrameDecode> frame;
    gcap_status_t st = open_wic_frame(path_utf8, factory, frame, out);
    if (st != GCAP_OK)
        return st;

    UINT width = 0, height = 0;
    frame->GetSize(&width, &height);
    out->width = static_cast<int>(width);
    out->height = static_cast<int>(height);
    out->preview_stride_bytes = out->width * 8;
    out->preview_size_bytes = static_cast<size_t>(out->preview_stride_bytes) * static_cast<size_t>(out->height);

    WICPixelFormatGUID pf = {};
    frame->GetPixelFormat(&pf);
    copy_text(out->pixel_format_name, sizeof(out->pixel_format_name), wic_pixel_format_name(pf));

    ComPtr<IWICComponentInfo> componentInfo;
    if (SUCCEEDED(factory->CreateComponentInfo(pf, &componentInfo)) && componentInfo)
    {
        ComPtr<IWICPixelFormatInfo2> pfi;
        if (SUCCEEDED(componentInfo.As(&pfi)) && pfi)
        {
            UINT channelCount = 0;
            UINT bitsPerPixel = 0;
            if (SUCCEEDED(pfi->GetChannelCount(&channelCount)))
                out->channels = static_cast<int>(channelCount);
            if (SUCCEEDED(pfi->GetBitsPerPixel(&bitsPerPixel)) && out->channels > 0)
                out->bits_per_sample = static_cast<int>(bitsPerPixel / channelCount);
        }
    }

    ComPtr<IWICMetadataQueryReader> meta;
    if (SUCCEEDED(frame->GetMetadataQueryReader(&meta)) && meta)
    {
        USHORT bitsPerSample = 0;
        if (metadata_ushort(meta.Get(), L"/ifd/{ushort=258}", bitsPerSample))
            out->bits_per_sample = static_cast<int>(bitsPerSample);
        USHORT spp = 0;
        if (metadata_ushort(meta.Get(), L"/ifd/{ushort=277}", spp))
            out->samples_per_pixel = static_cast<int>(spp);
        USHORT photo = 0;
        if (metadata_ushort(meta.Get(), L"/ifd/{ushort=262}", photo))
            copy_text(out->photometric, sizeof(out->photometric), photometric_name(photo));
    }

    if (out->samples_per_pixel <= 0)
        out->samples_per_pixel = out->channels;
    if (out->bits_per_sample <= 0 && out->channels > 0)
        out->bits_per_sample = 16;
    out->stored_bit_depth = out->bits_per_sample;
    if (out->photometric[0] == '\0')
        copy_text(out->photometric, sizeof(out->photometric), "Unknown");

    bool analyzed = false;
    if (IsEqualGUID(pf, GUID_WICPixelFormat16bppGray))
    {
        std::vector<uint16_t> samples;
        if (copy_gray16_samples(frame.Get(), width, height, samples, nullptr))
        {
            const SampleAnalysis sa = analyze_samples16(samples, out->stored_bit_depth > 0 ? out->stored_bit_depth : 16);
            out->min_value = sa.minValue;
            out->max_value = sa.maxValue;
            out->unique_value_count = sa.uniqueValueCount;
            out->effective_bit_depth = sa.effectiveBits;
            out->values_look_shifted_10bit = sa.shift10 ? 1 : 0;
            out->values_look_8bit_expanded = sa.expanded8 ? 1 : 0;
            fill_ramp_report(samples, int(width), int(height), true, *out);
            copy_text(out->sampled_row_source, sizeof(out->sampled_row_source), "gray16");
            analyzed = true;
        }
    }
    else
    {
        ComPtr<IWICFormatConverter> conv;
        if (SUCCEEDED(factory->CreateFormatConverter(&conv)) && conv)
        {
            if (IsEqualGUID(pf, GUID_WICPixelFormat8bppGray))
            {
                HRESULT hr = conv->Initialize(frame.Get(), GUID_WICPixelFormat16bppGray,
                                              WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
                if (SUCCEEDED(hr))
                {
                    std::strncat(out->pixel_format_name, " -> 16bppGray", sizeof(out->pixel_format_name) - std::strlen(out->pixel_format_name) - 1);
                    std::vector<uint16_t> samples;
                    if (copy_gray16_samples(conv.Get(), width, height, samples, nullptr))
                    {
                        const SampleAnalysis sa = analyze_samples16(samples, out->stored_bit_depth > 0 ? out->stored_bit_depth : 16);
                        out->min_value = sa.minValue;
                        out->max_value = sa.maxValue;
                        out->unique_value_count = sa.uniqueValueCount;
                        out->effective_bit_depth = sa.effectiveBits;
                        out->values_look_shifted_10bit = sa.shift10 ? 1 : 0;
                        out->values_look_8bit_expanded = sa.expanded8 ? 1 : 0;
                        fill_ramp_report(samples, int(width), int(height), true, *out);
                        copy_text(out->sampled_row_source, sizeof(out->sampled_row_source), "gray16");
                        analyzed = true;
                    }
                }
            }
            else
            {
                HRESULT hr = conv->Initialize(frame.Get(), GUID_WICPixelFormat64bppRGBA,
                                              WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
                if (SUCCEEDED(hr))
                {
                    std::strncat(out->pixel_format_name, " -> 64bppRGBA", sizeof(out->pixel_format_name) - std::strlen(out->pixel_format_name) - 1);
                    std::vector<uint16_t> allSamples;
                    std::vector<uint16_t> gray;
                    bool rgbNearlyEqual = true;
                    if (copy_rgba64_samples(conv.Get(), width, height, allSamples, gray, rgbNearlyEqual, nullptr))
                    {
                        const SampleAnalysis sa = analyze_samples16(allSamples, out->stored_bit_depth > 0 ? out->stored_bit_depth : 16);
                        out->min_value = sa.minValue;
                        out->max_value = sa.maxValue;
                        out->unique_value_count = sa.uniqueValueCount;
                        out->effective_bit_depth = sa.effectiveBits;
                        out->values_look_shifted_10bit = sa.shift10 ? 1 : 0;
                        out->values_look_8bit_expanded = sa.expanded8 ? 1 : 0;
                        fill_ramp_report(gray, int(width), int(height), rgbNearlyEqual, *out);
                        analyzed = true;
                    }
                }
            }
        }
    }

    if (!analyzed)
    {
        copy_text(out->error, sizeof(out->error), "Unsupported TIFF pixel format for analysis");
        return GCAP_ENOTSUP;
    }

    out->ok = 1;
    gcap::log_printf(GCAP_LOG_DEBUG, "[TIFF] analyze OK: %s %dx%d stored=%d effective=%d", path_utf8, out->width, out->height, out->stored_bit_depth, out->effective_bit_depth);
    return GCAP_OK;
#endif
}

extern "C" GCAP_API gcap_status_t gcap_read_tiff_preview_rgba64(const char *path_utf8,
                                                                  void *dst,
                                                                  size_t dst_size,
                                                                  int *width,
                                                                  int *height,
                                                                  int *stride_bytes,
                                                                  size_t *required_size)
{
    if (!path_utf8 || !*path_utf8)
        return GCAP_EINVAL;
#ifndef _WIN32
    (void)dst; (void)dst_size; (void)width; (void)height; (void)stride_bytes; (void)required_size;
    return GCAP_ENOTSUP;
#else
    ScopedCoInit co;
    if (FAILED(co.hr) && co.hr != RPC_E_CHANGED_MODE)
        return GCAP_EIO;

    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapFrameDecode> frame;
    gcap_status_t st = open_wic_frame(path_utf8, factory, frame, nullptr);
    if (st != GCAP_OK)
        return st;

    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    const int stride = static_cast<int>(w) * 8;
    const size_t required = static_cast<size_t>(stride) * static_cast<size_t>(h);
    if (width) *width = static_cast<int>(w);
    if (height) *height = static_cast<int>(h);
    if (stride_bytes) *stride_bytes = stride;
    if (required_size) *required_size = required;
    if (!dst || dst_size == 0)
        return GCAP_OK;
    if (dst_size < required)
        return GCAP_EINVAL;

    WICPixelFormatGUID pf = {};
    frame->GetPixelFormat(&pf);
    std::vector<uint8_t> rgba64;
    bool ok = false;
    if (IsEqualGUID(pf, GUID_WICPixelFormat16bppGray))
    {
        std::vector<uint16_t> samples;
        ok = copy_gray16_samples(frame.Get(), w, h, samples, &rgba64);
    }
    else
    {
        ComPtr<IWICFormatConverter> conv;
        if (SUCCEEDED(factory->CreateFormatConverter(&conv)) && conv)
        {
            if (IsEqualGUID(pf, GUID_WICPixelFormat8bppGray))
            {
                HRESULT hr = conv->Initialize(frame.Get(), GUID_WICPixelFormat16bppGray,
                                              WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
                if (SUCCEEDED(hr))
                {
                    std::vector<uint16_t> samples;
                    ok = copy_gray16_samples(conv.Get(), w, h, samples, &rgba64);
                }
            }
            else
            {
                HRESULT hr = conv->Initialize(frame.Get(), GUID_WICPixelFormat64bppRGBA,
                                              WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
                if (SUCCEEDED(hr))
                {
                    std::vector<uint16_t> allSamples, gray;
                    bool rgbNearlyEqual = true;
                    ok = copy_rgba64_samples(conv.Get(), w, h, allSamples, gray, rgbNearlyEqual, &rgba64);
                }
            }
        }
    }
    if (!ok || rgba64.size() < required)
        return GCAP_EIO;
    std::memcpy(dst, rgba64.data(), required);
    return GCAP_OK;
#endif
}
