#include "tiff_analyzer.h"

#include <QFileInfo>
#include <QSet>
#include <QStringList>
#include <limits>
#include <QVector>

#ifdef _WIN32
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#pragma comment(lib, "windowscodecs.lib")
using Microsoft::WRL::ComPtr;
#endif

namespace
{
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

static QString wicPixelFormatName(const WICPixelFormatGUID &fmt)
{
    if (IsEqualGUID(fmt, GUID_WICPixelFormat16bppGray)) return QStringLiteral("16bppGray");
    if (IsEqualGUID(fmt, GUID_WICPixelFormat8bppGray)) return QStringLiteral("8bppGray");
    if (IsEqualGUID(fmt, GUID_WICPixelFormat24bppBGR)) return QStringLiteral("24bppBGR");
    if (IsEqualGUID(fmt, GUID_WICPixelFormat24bppRGB)) return QStringLiteral("24bppRGB");
    if (IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA)) return QStringLiteral("32bppBGRA");
    if (IsEqualGUID(fmt, GUID_WICPixelFormat32bppRGBA)) return QStringLiteral("32bppRGBA");
    if (IsEqualGUID(fmt, GUID_WICPixelFormat48bppRGB)) return QStringLiteral("48bppRGB");
    if (IsEqualGUID(fmt, GUID_WICPixelFormat64bppRGBA)) return QStringLiteral("64bppRGBA");
    if (IsEqualGUID(fmt, GUID_WICPixelFormat64bppBGRA)) return QStringLiteral("64bppBGRA");
    if (IsEqualGUID(fmt, GUID_WICPixelFormat48bppBGR)) return QStringLiteral("48bppBGR");
    return QStringLiteral("Unknown WIC format");
}

static QString photometricName(USHORT v)
{
    switch (v)
    {
    case 0: return QStringLiteral("WhiteIsZero");
    case 1: return QStringLiteral("BlackIsZero");
    case 2: return QStringLiteral("RGB");
    case 3: return QStringLiteral("Palette");
    case 5: return QStringLiteral("CMYK");
    case 6: return QStringLiteral("YCbCr");
    default: return QStringLiteral("Unknown (%1)").arg(v);
    }
}

static bool metadataUShort(IWICMetadataQueryReader *reader, const wchar_t *query, USHORT &out)
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

static int bitWidth64(quint64 v)
{
    int w = 0;
    while (v)
    {
        ++w;
        v >>= 1;
    }
    return w > 0 ? w : 1;
}

static int ceilLog2_64(quint64 v)
{
    if (v <= 1)
        return 1;
    return bitWidth64(v - 1);
}

static int trailingZeros16(quint16 v)
{
    if (v == 0)
        return 16;
    int n = 0;
    while (((v >> n) & 1u) == 0u && n < 16)
        ++n;
    return n;
}

struct SampleAnalysis
{
    quint64 minValue = (std::numeric_limits<quint64>::max)();
    quint64 maxValue = 0;
    quint64 uniqueValueCount = 0;
    int effectiveBits = 0;
    bool shift10 = false;
    bool expanded8 = false;
};

static SampleAnalysis analyzeSamples16(const QVector<quint16> &samples, int storedBits)
{
    SampleAnalysis r;
    if (samples.isEmpty() || storedBits <= 0)
    {
        r.minValue = 0;
        return r;
    }

    QSet<quint16> unique;
    unique.reserve(qMin(samples.size(), 8192));
    bool allEqualHighLow = true;
    bool anyNonZero = false;
    int minTrailingZeros = 16;

    for (quint16 v : samples)
    {
        r.minValue = qMin(r.minValue, quint64(v));
        r.maxValue = qMax(r.maxValue, quint64(v));
        unique.insert(v);
        if (((v >> 8) & 0xFFu) != (v & 0xFFu))
            allEqualHighLow = false;
        if (v != 0)
        {
            anyNonZero = true;
            minTrailingZeros = qMin(minTrailingZeros, trailingZeros16(v));
        }
    }

    r.uniqueValueCount = static_cast<quint64>(unique.size());

    int effective = storedBits;
    effective = qMin(effective, bitWidth64(r.maxValue));
    effective = qMin(effective, ceilLog2_64(r.uniqueValueCount));

    if (storedBits >= 16 && allEqualHighLow)
    {
        effective = qMin(effective, 8);
        r.expanded8 = true;
    }

    if (anyNonZero && minTrailingZeros > 0 && minTrailingZeros < storedBits)
    {
        effective = qMin(effective, storedBits - minTrailingZeros);
        if (storedBits - minTrailingZeros == 10)
            r.shift10 = true;
    }

    if (r.maxValue <= 1023)
        effective = qMin(effective, 10);

    if (r.maxValue <= 4095)
        effective = qMin(effective, 12);

    r.effectiveBits = qBound(1, effective, storedBits);
    if (r.minValue == (std::numeric_limits<quint64>::max)())
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

static RampAxisStats calcRampAxisStats(const QVector<quint16> &axis)
{
    RampAxisStats s;
    QSet<quint16> unique;
    unique.reserve(qMin(axis.size(), 4096));
    for (int i = 0; i < axis.size(); ++i)
        unique.insert(axis[i]);

    for (int i = 1; i < axis.size(); ++i)
    {
        const int d = int(axis[i]) - int(axis[i - 1]);
        if (d > 0) ++s.pos;
        else if (d < 0) ++s.neg;
        else ++s.zero;
    }

    s.uniqueCount = unique.size();
    const int nonZero = s.pos + s.neg;
    s.monotonicRatio = (nonZero > 0) ? (double(qMax(s.pos, s.neg)) / double(nonZero)) : 0.0;
    return s;
}

static QString formatRampAxisStats(const RampAxisStats &s)
{
    return QStringLiteral("axisUnique=%1 monotonicRatio=%2 zeroSteps=%3")
        .arg(s.uniqueCount)
        .arg(s.monotonicRatio, 0, 'f', 3)
        .arg(s.zero);
}

static bool looksLikeGrayRamp(const QVector<quint16> &axis, QString *reason)
{
    if (axis.size() < 32)
    {
        if (reason) *reason = QStringLiteral("axis too short");
        return false;
    }

    const RampAxisStats s = calcRampAxisStats(axis);
    const bool monotonic = s.monotonicRatio >= 0.98;
    const bool enoughUnique = s.uniqueCount >= qMin(axis.size() / 2, 512);
    if (reason)
        *reason = formatRampAxisStats(s);
    return monotonic && enoughUnique;
}

static bool looksLikeVisualGrayRamp(const QVector<quint16> &axis, QString *reason)
{
    if (axis.size() < 32)
    {
        if (reason) *reason = QStringLiteral("axis too short");
        return false;
    }

    const RampAxisStats s = calcRampAxisStats(axis);
    const bool trending = s.monotonicRatio >= 0.78;
    const bool enoughUnique = s.uniqueCount >= qMin(axis.size() / 4, 256);
    if (reason)
        *reason = formatRampAxisStats(s);
    return trending && enoughUnique;
}

static QVector<quint16> extractGrayAxisRow(const QVector<quint16> &gray, int width, int height)
{
    QVector<quint16> out;
    if (width <= 0 || height <= 0 || gray.size() < width * height)
        return out;
    const int y = height / 2;
    out.reserve(width);
    const int base = y * width;
    for (int x = 0; x < width; ++x)
        out.push_back(gray[base + x]);
    return out;
}

static QVector<quint16> extractGrayAxisCol(const QVector<quint16> &gray, int width, int height)
{
    QVector<quint16> out;
    if (width <= 0 || height <= 0 || gray.size() < width * height)
        return out;
    const int x = width / 2;
    out.reserve(height);
    for (int y = 0; y < height; ++y)
        out.push_back(gray[y * width + x]);
    return out;
}

static bool analyzeGray16Frame(IWICBitmapSource *src, UINT width, UINT height, TiffBitDepthReport &report)
{
    const UINT stride = width * 2u;
    QVector<quint8> buf(static_cast<int>(stride * height));
    if (FAILED(src->CopyPixels(nullptr, stride, static_cast<UINT>(buf.size()), buf.data())))
        return false;

    QVector<quint16> samples;
    samples.reserve(static_cast<int>(width * height));
    const quint16 *p = reinterpret_cast<const quint16 *>(buf.constData());
    for (UINT i = 0; i < width * height; ++i)
        samples.push_back(p[i]);

    const SampleAnalysis sa = analyzeSamples16(samples, report.storedBitDepth > 0 ? report.storedBitDepth : 16);
    report.minValue = sa.minValue;
    report.maxValue = sa.maxValue;
    report.uniqueValueCount = sa.uniqueValueCount;
    report.effectiveBitDepth = sa.effectiveBits;
    report.valuesLookShifted10Bit = sa.shift10;
    report.valuesLook8BitExpanded = sa.expanded8;

    const QVector<quint16> rowAxis = extractGrayAxisRow(samples, int(width), int(height));
    const QVector<quint16> colAxis = extractGrayAxisCol(samples, int(width), int(height));

    QString rowReason;
    QString colReason;
    const bool rowStrict = looksLikeGrayRamp(rowAxis, &rowReason);
    const bool colStrict = looksLikeGrayRamp(colAxis, &colReason);

    QString rowVisualReason;
    QString colVisualReason;
    const bool rowVisual = looksLikeVisualGrayRamp(rowAxis, &rowVisualReason);
    const bool colVisual = looksLikeVisualGrayRamp(colAxis, &colVisualReason);

    report.likelyTenBitContent = (report.effectiveBitDepth >= 10) && !report.valuesLook8BitExpanded;
    report.strictTenBitRamp = report.likelyTenBitContent && (rowStrict || colStrict);
    report.visualTenBitRampCandidate = report.likelyTenBitContent && (rowVisual || colVisual);
    report.likelyTenBitRamp = report.visualTenBitRampCandidate;

    if (rowStrict)
        report.strictRampReason = QStringLiteral("row strict ramp; %1").arg(rowReason);
    else if (colStrict)
        report.strictRampReason = QStringLiteral("column strict ramp; %1").arg(colReason);
    else
        report.strictRampReason = QStringLiteral("not strict enough; row=%1; col=%2").arg(rowReason, colReason);

    if (rowVisual)
        report.visualRampReason = QStringLiteral("row visual ramp candidate; %1").arg(rowVisualReason);
    else if (colVisual)
        report.visualRampReason = QStringLiteral("column visual ramp candidate; %1").arg(colVisualReason);
    else
        report.visualRampReason = QStringLiteral("not smooth/trending enough; row=%1; col=%2").arg(rowVisualReason, colVisualReason);

    report.rampReason = QStringLiteral("strict=%1; visual=%2")
                            .arg(report.strictRampReason, report.visualRampReason);
    return true;
}

static bool analyzeRgba64Frame(IWICBitmapSource *src, UINT width, UINT height, TiffBitDepthReport &report)
{
    const UINT stride = width * 8u;
    QVector<quint8> buf(static_cast<int>(stride * height));
    if (FAILED(src->CopyPixels(nullptr, stride, static_cast<UINT>(buf.size()), buf.data())))
        return false;

    QVector<quint16> allSamples;
    allSamples.reserve(static_cast<int>(width * height * 3u));
    QVector<quint16> gray;
    gray.reserve(static_cast<int>(width * height));

    const quint16 *p = reinterpret_cast<const quint16 *>(buf.constData());
    bool rgbNearlyEqual = true;
    for (UINT i = 0; i < width * height; ++i)
    {
        const quint16 r = p[i * 4 + 0];
        const quint16 g = p[i * 4 + 1];
        const quint16 b = p[i * 4 + 2];
        allSamples.push_back(r);
        allSamples.push_back(g);
        allSamples.push_back(b);
        gray.push_back(static_cast<quint16>((quint32(r) + quint32(g) + quint32(b)) / 3u));
        if (!(qAbs(int(r) - int(g)) <= 2 && qAbs(int(r) - int(b)) <= 2 && qAbs(int(g) - int(b)) <= 2))
            rgbNearlyEqual = false;
    }

    const SampleAnalysis sa = analyzeSamples16(allSamples, report.storedBitDepth > 0 ? report.storedBitDepth : 16);
    report.minValue = sa.minValue;
    report.maxValue = sa.maxValue;
    report.uniqueValueCount = sa.uniqueValueCount;
    report.effectiveBitDepth = sa.effectiveBits;
    report.valuesLookShifted10Bit = sa.shift10;
    report.valuesLook8BitExpanded = sa.expanded8;

    const QVector<quint16> rowAxis = extractGrayAxisRow(gray, int(width), int(height));
    const QVector<quint16> colAxis = extractGrayAxisCol(gray, int(width), int(height));

    QString rowReason;
    QString colReason;
    const bool rowStrict = looksLikeGrayRamp(rowAxis, &rowReason);
    const bool colStrict = looksLikeGrayRamp(colAxis, &colReason);

    QString rowVisualReason;
    QString colVisualReason;
    const bool rowVisual = looksLikeVisualGrayRamp(rowAxis, &rowVisualReason);
    const bool colVisual = looksLikeVisualGrayRamp(colAxis, &colVisualReason);

    report.likelyTenBitContent = (report.effectiveBitDepth >= 10) && rgbNearlyEqual && !report.valuesLook8BitExpanded;
    report.strictTenBitRamp = report.likelyTenBitContent && (rowStrict || colStrict);
    report.visualTenBitRampCandidate = report.likelyTenBitContent && (rowVisual || colVisual);
    report.likelyTenBitRamp = report.visualTenBitRampCandidate;

    if (rowStrict)
        report.strictRampReason = QStringLiteral("row strict ramp; grayRGB=%1; %2")
                                      .arg(rgbNearlyEqual ? QStringLiteral("yes") : QStringLiteral("no"), rowReason);
    else if (colStrict)
        report.strictRampReason = QStringLiteral("column strict ramp; grayRGB=%1; %2")
                                      .arg(rgbNearlyEqual ? QStringLiteral("yes") : QStringLiteral("no"), colReason);
    else
        report.strictRampReason = QStringLiteral("not strict enough; grayRGB=%1; row=%2; col=%3")
                                      .arg(rgbNearlyEqual ? QStringLiteral("yes") : QStringLiteral("no"), rowReason, colReason);

    if (rowVisual)
        report.visualRampReason = QStringLiteral("row visual ramp candidate; grayRGB=%1; %2")
                                      .arg(rgbNearlyEqual ? QStringLiteral("yes") : QStringLiteral("no"), rowVisualReason);
    else if (colVisual)
        report.visualRampReason = QStringLiteral("column visual ramp candidate; grayRGB=%1; %2")
                                      .arg(rgbNearlyEqual ? QStringLiteral("yes") : QStringLiteral("no"), colVisualReason);
    else
        report.visualRampReason = QStringLiteral("not smooth/trending enough; grayRGB=%1; row=%2; col=%3")
                                      .arg(rgbNearlyEqual ? QStringLiteral("yes") : QStringLiteral("no"), rowVisualReason, colVisualReason);

    report.rampReason = QStringLiteral("strict=%1; visual=%2")
                            .arg(report.strictRampReason, report.visualRampReason);
    return true;
}
#endif
} // namespace

TiffBitDepthReport TiffAnalyzer::analyzeFile(const QString &path)
{
    TiffBitDepthReport report;
    report.path = path;

#ifndef _WIN32
    report.error = QStringLiteral("TIFF analyzer is only implemented on Windows/WIC in this build.");
    return report;
#else
    ScopedCoInit co;
    if (FAILED(co.hr) && co.hr != RPC_E_CHANGED_MODE)
    {
        report.error = QStringLiteral("CoInitializeEx failed: 0x%1").arg(qulonglong(co.hr), 0, 16);
        return report;
    }

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory)
    {
        report.error = QStringLiteral("Create WIC factory failed: 0x%1").arg(qulonglong(hr), 0, 16);
        return report;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromFilename(reinterpret_cast<LPCWSTR>(path.utf16()), nullptr, GENERIC_READ,
                                            WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr) || !decoder)
    {
        report.error = QStringLiteral("Open TIFF failed: 0x%1").arg(qulonglong(hr), 0, 16);
        return report;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr) || !frame)
    {
        report.error = QStringLiteral("Read TIFF frame failed: 0x%1").arg(qulonglong(hr), 0, 16);
        return report;
    }

    UINT width = 0, height = 0;
    frame->GetSize(&width, &height);
    report.width = static_cast<int>(width);
    report.height = static_cast<int>(height);

    WICPixelFormatGUID pf = {};
    frame->GetPixelFormat(&pf);
    report.pixelFormatName = wicPixelFormatName(pf);

    ComPtr<IWICComponentInfo> componentInfo;
    if (SUCCEEDED(factory->CreateComponentInfo(pf, &componentInfo)) && componentInfo)
    {
        ComPtr<IWICPixelFormatInfo2> pfi;
        if (SUCCEEDED(componentInfo.As(&pfi)) && pfi)
        {
            UINT channelCount = 0;
            UINT bitsPerPixel = 0;
            if (SUCCEEDED(pfi->GetChannelCount(&channelCount)))
                report.channels = static_cast<int>(channelCount);
            if (SUCCEEDED(pfi->GetBitsPerPixel(&bitsPerPixel)) && report.channels > 0)
                report.bitsPerSample = static_cast<int>(bitsPerPixel / channelCount);
        }
    }

    ComPtr<IWICMetadataQueryReader> meta;
    if (SUCCEEDED(frame->GetMetadataQueryReader(&meta)) && meta)
    {
        USHORT bitsPerSample = 0;
        if (metadataUShort(meta.Get(), L"/ifd/{ushort=258}", bitsPerSample))
            report.bitsPerSample = static_cast<int>(bitsPerSample);

        USHORT spp = 0;
        if (metadataUShort(meta.Get(), L"/ifd/{ushort=277}", spp))
            report.samplesPerPixel = static_cast<int>(spp);

        USHORT photo = 0;
        if (metadataUShort(meta.Get(), L"/ifd/{ushort=262}", photo))
            report.photometric = photometricName(photo);
    }

    if (report.samplesPerPixel <= 0)
        report.samplesPerPixel = report.channels;
    if (report.bitsPerSample <= 0 && report.channels > 0)
        report.bitsPerSample = 16;
    report.storedBitDepth = report.bitsPerSample;

    bool analyzed = false;
    if (IsEqualGUID(pf, GUID_WICPixelFormat16bppGray))
    {
        analyzed = analyzeGray16Frame(frame.Get(), width, height, report);
    }
    else
    {
        ComPtr<IWICFormatConverter> conv;
        if (SUCCEEDED(factory->CreateFormatConverter(&conv)) && conv)
        {
            if (IsEqualGUID(pf, GUID_WICPixelFormat8bppGray))
            {
                hr = conv->Initialize(frame.Get(), GUID_WICPixelFormat16bppGray,
                                      WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
                if (SUCCEEDED(hr))
                {
                    report.pixelFormatName += QStringLiteral(" -> 16bppGray");
                    analyzed = analyzeGray16Frame(conv.Get(), width, height, report);
                }
            }
            else
            {
                hr = conv->Initialize(frame.Get(), GUID_WICPixelFormat64bppRGBA,
                                      WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
                if (SUCCEEDED(hr))
                {
                    report.pixelFormatName += QStringLiteral(" -> 64bppRGBA");
                    analyzed = analyzeRgba64Frame(conv.Get(), width, height, report);
                }
            }
        }
    }

    if (!analyzed)
    {
        report.error = QStringLiteral("Unsupported TIFF pixel format for analysis: %1").arg(report.pixelFormatName);
        return report;
    }

    report.ok = true;
    if (report.photometric.isEmpty())
        report.photometric = QStringLiteral("Unknown");
    return report;
#endif
}

QString TiffAnalyzer::formatReportText(const TiffBitDepthReport &r)
{
    QStringList lines;
    lines << QStringLiteral("File: %1").arg(QFileInfo(r.path).fileName());
    lines << QStringLiteral("Path: %1").arg(r.path);
    if (!r.ok)
    {
        lines << QStringLiteral("Status: Failed");
        lines << QStringLiteral("Error: %1").arg(r.error);
        return lines.join('\n');
    }

    lines << QStringLiteral("Status: OK");
    lines << QStringLiteral("Size: %1 x %2").arg(r.width).arg(r.height);
    lines << QStringLiteral("Pixel format: %1").arg(r.pixelFormatName);
    lines << QStringLiteral("Photometric: %1").arg(r.photometric);
    lines << QStringLiteral("Samples per pixel: %1").arg(r.samplesPerPixel);
    lines << QStringLiteral("Bits per sample (stored): %1").arg(r.bitsPerSample);
    lines << QStringLiteral("Stored bit depth: %1-bit").arg(r.storedBitDepth);
    lines << QStringLiteral("Effective bit depth: %1-bit").arg(r.effectiveBitDepth);
    lines << QStringLiteral("Min / Max: %1 / %2").arg(r.minValue).arg(r.maxValue);
    lines << QStringLiteral("Unique values: %1").arg(r.uniqueValueCount);
    lines << QStringLiteral("Looks like shifted 10-bit in 16-bit container: %1").arg(r.valuesLookShifted10Bit ? QStringLiteral("Yes") : QStringLiteral("No"));
    lines << QStringLiteral("Looks like expanded 8-bit: %1").arg(r.valuesLook8BitExpanded ? QStringLiteral("Yes") : QStringLiteral("No"));
    lines << QStringLiteral("10-bit ramp: %1").arg(r.likelyTenBitRamp ? QStringLiteral("Yes") : QStringLiteral("No"));
    lines << QStringLiteral("Ramp reason: %1").arg(r.rampReason);
    return lines.join('\n');
}
