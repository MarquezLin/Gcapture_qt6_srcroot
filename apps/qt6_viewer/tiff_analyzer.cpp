#include "tiff_analyzer.h"

#include <QFileInfo>
#include <QStringList>
#include <QVector>
#include <cstring>
#include <string>

#include "gcapture.h"

namespace
{
static QString fromUtf8Field(const char *s)
{
    return QString::fromUtf8(s ? s : "");
}

static std::string toUtf8Path(const QString &path)
{
    const QByteArray u8 = path.toUtf8();
    return std::string(u8.constData(), static_cast<size_t>(u8.size()));
}
}

TiffBitDepthReport TiffAnalyzer::analyzeFile(const QString &path)
{
    TiffBitDepthReport report;
    report.path = path;

    const std::string pathUtf8 = toUtf8Path(path);
    gcap_tiff_analysis_t sdk{};
    const gcap_status_t st = gcap_analyze_tiff(pathUtf8.c_str(), &sdk);
    if (st != GCAP_OK || !sdk.ok)
    {
        report.error = !fromUtf8Field(sdk.error).isEmpty()
                           ? fromUtf8Field(sdk.error)
                           : QStringLiteral("gcap_analyze_tiff failed: %1").arg(QString::fromUtf8(gcap_strerror(st)));
        return report;
    }

    report.ok = true;
    report.width = sdk.width;
    report.height = sdk.height;
    report.channels = sdk.channels;
    report.bitsPerSample = sdk.bits_per_sample;
    report.samplesPerPixel = sdk.samples_per_pixel;
    report.storedBitDepth = sdk.stored_bit_depth;
    report.effectiveBitDepth = sdk.effective_bit_depth;
    report.pixelFormatName = fromUtf8Field(sdk.pixel_format_name);
    report.photometric = fromUtf8Field(sdk.photometric);
    report.minValue = sdk.min_value;
    report.maxValue = sdk.max_value;
    report.uniqueValueCount = sdk.unique_value_count;
    report.likelyTenBitRamp = sdk.likely_ten_bit_ramp != 0;
    report.strictTenBitRamp = sdk.strict_ten_bit_ramp != 0;
    report.visualTenBitRampCandidate = sdk.visual_ten_bit_ramp_candidate != 0;
    report.likelyTenBitContent = sdk.likely_ten_bit_content != 0;
    report.valuesLookShifted10Bit = sdk.values_look_shifted_10bit != 0;
    report.valuesLook8BitExpanded = sdk.values_look_8bit_expanded != 0;
    report.rampReason = fromUtf8Field(sdk.ramp_reason);
    report.strictRampReason = fromUtf8Field(sdk.strict_ramp_reason);
    report.visualRampReason = fromUtf8Field(sdk.visual_ramp_reason);
    report.sampledRowY = sdk.sampled_row_y;
    report.sampledRowSource = fromUtf8Field(sdk.sampled_row_source);
    report.sampledRowLogical10Rule = fromUtf8Field(sdk.sampled_row_logical10_rule);
    report.sampledRowRaw16Csv = fromUtf8Field(sdk.sampled_row_raw16_csv);
    report.sampledRowLogical10Csv = fromUtf8Field(sdk.sampled_row_logical10_csv);

    int w = 0;
    int h = 0;
    int stride = 0;
    size_t required = 0;
    if (gcap_read_tiff_preview_rgba64(pathUtf8.c_str(), nullptr, 0, &w, &h, &stride, &required) == GCAP_OK &&
        w == report.width && h == report.height && stride > 0 && required > 0)
    {
        QByteArray preview;
        preview.resize(static_cast<int>(required));
        if (gcap_read_tiff_preview_rgba64(pathUtf8.c_str(), preview.data(), required, &w, &h, &stride, &required) == GCAP_OK)
        {
            report.previewRgba64 = preview;
            report.previewStrideBytes = stride;
        }
    }

    return report;
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
    lines << QStringLiteral("Analyzer: gcapture SDK / WIC");
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
    lines << QStringLiteral("Likely 10-bit content: %1").arg(r.likelyTenBitContent ? QStringLiteral("Yes") : QStringLiteral("No"));
    lines << QStringLiteral("Strict 10-bit ramp: %1").arg(r.strictTenBitRamp ? QStringLiteral("Yes") : QStringLiteral("No"));
    lines << QStringLiteral("Visual 10-bit ramp candidate: %1").arg(r.visualTenBitRampCandidate ? QStringLiteral("Yes") : QStringLiteral("No"));
    lines << QStringLiteral("10-bit ramp: %1").arg(r.likelyTenBitRamp ? QStringLiteral("Yes") : QStringLiteral("No"));
    lines << QStringLiteral("Ramp reason: %1").arg(r.rampReason);
    if (r.sampledRowY >= 0 && !r.sampledRowRaw16Csv.isEmpty())
    {
        lines << QString();
        lines << QStringLiteral("Sampled center row y=%1 (%2)").arg(r.sampledRowY).arg(r.sampledRowSource);
        lines << QStringLiteral("Logical 10-bit range should be 0..1023 (1024 levels), not 0..1024.");
        if (!r.sampledRowLogical10Rule.isEmpty())
            lines << QStringLiteral("Logical10 rule: %1").arg(r.sampledRowLogical10Rule);
        lines << QStringLiteral("Center row raw16 CSV:");
        lines << r.sampledRowRaw16Csv;
        lines << QStringLiteral("Center row logical10 CSV:");
        lines << r.sampledRowLogical10Csv;
    }
    return lines.join('\n');
}
