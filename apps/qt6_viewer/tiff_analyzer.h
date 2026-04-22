#ifndef TIFF_ANALYZER_H
#define TIFF_ANALYZER_H

#include <QString>
#include <QImage>
#include <QtGlobal>
#include <QByteArray>

struct TiffBitDepthReport
{
    bool ok = false;
    QString path;
    QString error;

    int width = 0;
    int height = 0;
    int channels = 0;
    int bitsPerSample = 0;
    int samplesPerPixel = 0;
    int storedBitDepth = 0;
    int effectiveBitDepth = 0;

    QString pixelFormatName;
    QString photometric;

    quint64 minValue = 0;
    quint64 maxValue = 0;
    quint64 uniqueValueCount = 0;

    bool likelyTenBitRamp = false;
    bool strictTenBitRamp = false;
    bool visualTenBitRampCandidate = false;
    bool likelyTenBitContent = false;
    QString rampReason;
    QString strictRampReason;
    QString visualRampReason;

    bool valuesLookShifted10Bit = false;
    bool valuesLook8BitExpanded = false;

    QByteArray previewRgba64;
    int previewStrideBytes = 0;

    int sampledRowY = -1;
    QString sampledRowSource;
    QString sampledRowLogical10Rule;
    QString sampledRowRaw16Csv;
    QString sampledRowLogical10Csv;
};

class TiffAnalyzer
{
public:
    static TiffBitDepthReport analyzeFile(const QString &path);
    static QString formatReportText(const TiffBitDepthReport &report);
};

#endif // TIFF_ANALYZER_H
