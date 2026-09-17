#include "dicomframe.h"

#include <dcmtk/dcmdata/dctk.h>
#include <dcmtk/dcmdata/dcxfer.h>
#include <dcmtk/dcmdata/dcrledrg.h>
#include <dcmtk/dcmimgle/dcmimage.h>
#include <dcmtk/dcmjpeg/djdecode.h>
#include <dcmtk/dcmjpls/djdecode.h>

#include <QFile>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace
{
class DicomDecoderRegistry
{
public:
    DicomDecoderRegistry()
    {
        DcmRLEDecoderRegistration::registerCodecs();
        DJDecoderRegistration::registerCodecs();
        DJLSDecoderRegistration::registerCodecs();
    }

    ~DicomDecoderRegistry()
    {
        DJLSDecoderRegistration::cleanup();
        DJDecoderRegistration::cleanup();
        DcmRLEDecoderRegistration::cleanup();
    }
};

DicomDecoderRegistry &decoderRegistry()
{
    static DicomDecoderRegistry registry;
    return registry;
}
}

void DicomFrame::clear()
{
    path.clear();

    width = 0;
    height = 0;

    samplesPerPixel = 0;
    planarConfiguration = 0;

    bitsAllocated = 0;
    bitsStored = 0;
    highBit = 0;

    pixelRepresentation = 0;
    frameCount = 0;

    photometricInterpretation.clear();
    transferSyntaxUid.clear();

    rescaleSlope = 1.0;
    rescaleIntercept = 0.0;
    hasRescale = false;

    windowCenter = 0.0;
    windowWidth = 0.0;
    hasWindow = false;

    rawSamples_.clear();
    frameIndex_ = 0;
    lastDisplayImage_ = {};
}

bool DicomFrame::load(const QString &filePath,
                      QString *error)
{
    clear();
    (void)decoderRegistry();

    const QByteArray nativePath =
        QFile::encodeName(filePath);

    DcmFileFormat fileFormat;

    OFCondition status =
        fileFormat.loadFile(nativePath.constData());

    if (status.bad())
    {
        if (error)
        {
            *error =
                QStringLiteral("Failed to load DICOM: %1")
                    .arg(QString::fromLatin1(status.text()));
        }

        return false;
    }

    DcmDataset *dataset =
        fileFormat.getDataset();

    if (!dataset)
    {
        if (error)
            *error = QStringLiteral("DICOM dataset is null.");

        return false;
    }

    //
    // Transfer Syntax
    //
    if (DcmMetaInfo *meta =
            fileFormat.getMetaInfo())
    {
        OFString value;

        if (meta->findAndGetOFString(
                    DCM_TransferSyntaxUID,
                    value)
                .good())
        {
            transferSyntaxUid =
                QString::fromLatin1(value.c_str());
        }
    }

    DcmXfer xfer(dataset->getOriginalXfer());

    if (xfer.isEncapsulated())
    {
        status = dataset->chooseRepresentation(EXS_LittleEndianExplicit, nullptr);
        if (status.bad() || !dataset->canWriteXfer(EXS_LittleEndianExplicit))
        {
            if (error)
            {
                *error = QStringLiteral(
                             "No decoder is available for this compressed DICOM.\n"
                             "Transfer Syntax: %1\n"
                             "This build supports RLE, JPEG and JPEG-LS, but not JPEG 2000.")
                             .arg(transferSyntaxUid);
            }
            return false;
        }
    }

    Uint16 rows = 0;
    Uint16 columns = 0;
    Uint16 spp = 0;
    Uint16 planar = 0;

    Uint16 allocated = 0;
    Uint16 stored = 0;
    Uint16 hb = 0;
    Uint16 representation = 0;

    if (dataset->findAndGetUint16(
                   DCM_Rows, rows)
            .bad() ||
        dataset->findAndGetUint16(
                   DCM_Columns, columns)
            .bad())
    {
        if (error)
            *error = QStringLiteral(
                "Missing Rows / Columns.");

        return false;
    }

    if (dataset->findAndGetUint16(
                   DCM_SamplesPerPixel, spp)
            .bad())
    {
        if (error)
            *error = QStringLiteral(
                "Missing SamplesPerPixel.");

        return false;
    }

    if (dataset->findAndGetUint16(
                   DCM_BitsAllocated, allocated)
            .bad() ||
        dataset->findAndGetUint16(
                   DCM_BitsStored, stored)
            .bad() ||
        dataset->findAndGetUint16(
                   DCM_HighBit, hb)
            .bad() ||
        dataset->findAndGetUint16(
                   DCM_PixelRepresentation,
                   representation)
            .bad())
    {
        if (error)
            *error = QStringLiteral(
                "Missing DICOM bit-depth information.");

        return false;
    }

    OFString photo;

    if (dataset->findAndGetOFString(
                   DCM_PhotometricInterpretation,
                   photo)
            .bad())
    {
        if (error)
            *error = QStringLiteral(
                "Missing PhotometricInterpretation.");

        return false;
    }

    width = static_cast<int>(columns);
    height = static_cast<int>(rows);

    samplesPerPixel =
        static_cast<int>(spp);

    if (samplesPerPixel > 1 &&
        dataset->findAndGetUint16(DCM_PlanarConfiguration, planar).good())
        planarConfiguration = static_cast<int>(planar);

    bitsAllocated =
        static_cast<int>(allocated);

    bitsStored =
        static_cast<int>(stored);

    highBit =
        static_cast<int>(hb);

    pixelRepresentation =
        static_cast<int>(representation);

    photometricInterpretation =
        QString::fromLatin1(photo.c_str());

    //
    // Number Of Frames
    //
    frameCount = 1;

    OFString frameString;

    if (dataset->findAndGetOFString(
                   DCM_NumberOfFrames,
                   frameString)
            .good())
    {
        bool ok = false;

        const int count =
            QString::fromLatin1(
                frameString.c_str())
                .toInt(&ok);

        if (ok && count > 0)
            frameCount = count;
    }

    if (samplesPerPixel != 1 && samplesPerPixel != 3)
    {
        if (error)
        {
            *error =
                QStringLiteral(
                    "Only one- or three-sample DICOM pixels are supported.");
        }

        return false;
    }

    const QStringList supportedPhotometric = {
        QStringLiteral("MONOCHROME1"),
        QStringLiteral("MONOCHROME2"),
        QStringLiteral("RGB"),
        QStringLiteral("YBR_FULL"),
        QStringLiteral("YBR_FULL_422"),
        QStringLiteral("PALETTE COLOR")};
    if (!supportedPhotometric.contains(photometricInterpretation))
    {
        if (error)
        {
            *error =
                QStringLiteral(
                    "Unsupported PhotometricInterpretation: %1")
                    .arg(photometricInterpretation);
        }

        return false;
    }

    if (bitsAllocated != 8 &&
        bitsAllocated != 16)
    {
        if (error)
        {
            *error =
                QStringLiteral(
                    "Only 8-bit or 16-bit containers "
                    "are supported. BitsAllocated = %1")
                    .arg(bitsAllocated);
        }

        return false;
    }

    if (bitsStored <= 0 ||
        bitsStored > bitsAllocated)
    {
        if (error)
        {
            *error =
                QStringLiteral(
                    "Invalid BitsStored = %1")
                    .arg(bitsStored);
        }

        return false;
    }

    if (highBit < bitsStored - 1 ||
        highBit >= bitsAllocated)
    {
        if (error)
        {
            *error =
                QStringLiteral(
                    "Invalid HighBit = %1")
                    .arg(highBit);
        }

        return false;
    }

    //
    // Rescale Slope / Intercept
    //
    Float64 slope = 1.0;
    Float64 intercept = 0.0;

    const bool hasSlope =
        dataset->findAndGetFloat64(
                   DCM_RescaleSlope,
                   slope)
            .good();

    const bool hasIntercept =
        dataset->findAndGetFloat64(
                   DCM_RescaleIntercept,
                   intercept)
            .good();

    if (hasSlope)
        rescaleSlope = slope;

    if (hasIntercept)
        rescaleIntercept = intercept;

    hasRescale =
        hasSlope || hasIntercept;

    //
    // 第一組 Window Center / Width
    //
    Float64 wc = 0.0;
    Float64 ww = 0.0;

    if (dataset->findAndGetFloat64(
                   DCM_WindowCenter,
                   wc,
                   0)
            .good() &&
        dataset->findAndGetFloat64(
                   DCM_WindowWidth,
                   ww,
                   0)
            .good() &&
        ww > 0.0)
    {
        windowCenter = wc;
        windowWidth = ww;
        hasWindow = true;
    }

    //
    // 讀原始 PixelData
    //
    const qsizetype pixelsPerFrame =
        static_cast<qsizetype>(width) * static_cast<qsizetype>(height);
    const bool subsampledYbr =
        photometricInterpretation == QStringLiteral("YBR_FULL_422");
    const qsizetype samplesPerFrame = subsampledYbr
        ? static_cast<qsizetype>(height) * ((static_cast<qsizetype>(width) + 1) / 2) * 4
        : pixelsPerFrame * static_cast<qsizetype>(samplesPerPixel);
    const qsizetype expectedPixels = samplesPerFrame * static_cast<qsizetype>(frameCount);

    rawSamples_.resize(expectedPixels);

    if (bitsAllocated == 8)
    {
        const Uint8 *pixelData = nullptr;

        unsigned long count = 0;

        status =
            dataset->findAndGetUint8Array(
                DCM_PixelData,
                pixelData,
                &count);

        if (status.bad() ||
            !pixelData ||
            count <
                static_cast<unsigned long>(
                    expectedPixels))
        {
            if (error)
                *error =
                    QStringLiteral(
                        "Failed to read 8-bit PixelData.");

            clear();
            return false;
        }

        for (qsizetype i = 0;
             i < expectedPixels;
             ++i)
        {
            rawSamples_[i] =
                static_cast<quint16>(
                    pixelData[i]);
        }
    }
    else
    {
        const Uint16 *pixelData = nullptr;

        unsigned long count = 0;

        status =
            dataset->findAndGetUint16Array(
                DCM_PixelData,
                pixelData,
                &count);

        if (status.bad() ||
            !pixelData ||
            count <
                static_cast<unsigned long>(
                    expectedPixels))
        {
            if (error)
                *error =
                    QStringLiteral(
                        "Failed to read 16-bit PixelData.");

            clear();
            return false;
        }

        for (qsizetype i = 0;
             i < expectedPixels;
             ++i)
        {
            rawSamples_[i] =
                static_cast<quint16>(
                    pixelData[i]);
        }
    }

    path = filePath;

    return true;
}

bool DicomFrame::isValid() const
{
    if (width <= 0 ||
        height <= 0)
    {
        return false;
    }

    return frameCount > 0 && frameIndex_ >= 0 && frameIndex_ < frameCount &&
           !rawSamples_.isEmpty();
}

bool DicomFrame::isMonochrome() const
{
    return photometricInterpretation == QStringLiteral("MONOCHROME1") ||
           photometricInterpretation == QStringLiteral("MONOCHROME2");
}

bool DicomFrame::setFrameIndex(int frameIndex)
{
    if (frameIndex < 0 || frameIndex >= frameCount)
        return false;
    frameIndex_ = frameIndex;
    lastDisplayImage_ = {};
    return true;
}

qint32 DicomFrame::decodeStoredSample(
    quint16 raw) const
{
    //
    // BitsStored = 12
    // HighBit    = 11
    //
    // lowBit = 0
    //
    const int lowBit =
        highBit + 1 - bitsStored;

    quint32 value =
        static_cast<quint32>(raw);

    value >>= lowBit;

    quint32 mask;

    if (bitsStored >= 16)
    {
        mask = 0xffffu;
    }
    else
    {
        mask =
            (1u << bitsStored) - 1u;
    }

    value &= mask;

    //
    // Signed Pixel
    //
    if (pixelRepresentation == 1)
    {
        const quint32 signBit =
            1u << (bitsStored - 1);

        if (value & signBit)
        {
            value |= ~mask;
        }
    }

    return static_cast<qint32>(value);
}

DicomPixelSample DicomFrame::pixel(
    int x,
    int y) const
{
    DicomPixelSample sample;

    if (!isValid() ||
        x < 0 ||
        y < 0 ||
        x >= width ||
        y >= height)
    {
        return sample;
    }

    const QVector<quint16> raw = rawComponents(x, y);
    if (raw.isEmpty())
        return sample;

    sample.valid = true;

    sample.x = x;
    sample.y = y;

    sample.storedWord = raw.front();
    sample.storedValue = decodeStoredSample(raw.front());

    if (isMonochrome())
        sample.componentNames = {QStringLiteral("Gray")};
    else if (photometricInterpretation.startsWith(QStringLiteral("YBR")))
        sample.componentNames = {QStringLiteral("Y"), QStringLiteral("Cb"), QStringLiteral("Cr")};
    else if (photometricInterpretation == QStringLiteral("PALETTE COLOR"))
        sample.componentNames = {QStringLiteral("Index")};
    else
        sample.componentNames = {QStringLiteral("R"), QStringLiteral("G"), QStringLiteral("B")};

    for (quint16 word : raw)
        sample.storedComponents.push_back(decodeStoredSample(word));

    //
    // 這裡只代表 Rescale Slope/Intercept 路徑。
    // 正式 Display pipeline 則完全交給 DicomImage。
    //
    sample.rescaledValue =
        static_cast<double>(sample.storedValue) *
            rescaleSlope +
        rescaleIntercept;

    if (!lastDisplayImage_.isNull())
    {
        if (lastDisplayImage_.format() == QImage::Format_Grayscale16)
        {
            const auto *row = reinterpret_cast<const quint16 *>(lastDisplayImage_.constScanLine(y));
            sample.displayedComponents = {row[x]};
        }
        else if (lastDisplayImage_.format() == QImage::Format_RGBA64)
        {
            const auto *row = reinterpret_cast<const QRgba64 *>(lastDisplayImage_.constScanLine(y));
            const QRgba64 color = row[x];
            sample.displayedComponents = {color.red(), color.green(), color.blue()};
        }
    }

    return sample;
}

QVector<quint16> DicomFrame::rawComponents(int x, int y) const
{
    const qsizetype pixelCount = static_cast<qsizetype>(width) * height;
    const qsizetype pixelIndex = static_cast<qsizetype>(y) * width + x;
    const bool ybr422 = photometricInterpretation == QStringLiteral("YBR_FULL_422");
    const qsizetype samplesPerFrame = ybr422
        ? static_cast<qsizetype>(height) * ((static_cast<qsizetype>(width) + 1) / 2) * 4
        : pixelCount * samplesPerPixel;
    const qsizetype frameBase = static_cast<qsizetype>(frameIndex_) * samplesPerFrame;

    if (samplesPerPixel == 1)
    {
        const qsizetype index = frameBase + pixelIndex;
        return index < rawSamples_.size() ? QVector<quint16>{rawSamples_[index]} : QVector<quint16>{};
    }

    if (ybr422)
    {
        const qsizetype groupsPerRow = (static_cast<qsizetype>(width) + 1) / 2;
        const qsizetype group = frameBase +
            (static_cast<qsizetype>(y) * groupsPerRow + x / 2) * 4;
        if (group + 3 >= rawSamples_.size())
            return {};
        return {rawSamples_[group + (pixelIndex & 1)], rawSamples_[group + 2], rawSamples_[group + 3]};
    }

    QVector<quint16> result;
    result.reserve(3);
    if (planarConfiguration == 1)
    {
        for (int component = 0; component < 3; ++component)
        {
            const qsizetype index = frameBase + component * pixelCount + pixelIndex;
            if (index >= rawSamples_.size())
                return {};
            result.push_back(rawSamples_[index]);
        }
    }
    else
    {
        const qsizetype base = frameBase + pixelIndex * 3;
        if (base + 2 >= rawSamples_.size())
            return {};
        result = {rawSamples_[base], rawSamples_[base + 1], rawSamples_[base + 2]};
    }
    return result;
}

QImage DicomFrame::makeDisplayImage() const
{
    return renderDisplay(
        false,
        0.0,
        0.0);
}

QImage DicomFrame::makeDisplayImage(
    double wc,
    double ww) const
{
    if (ww <= 0.0)
        return {};

    return renderDisplay(
        true,
        wc,
        ww);
}

QImage DicomFrame::renderDisplay(
    bool useCustomWindow,
    double wc,
    double ww) const
{
    if (!isValid())
        return {};

    // Some DCMTK builds fail to produce a 16-bit output buffer for native
    // (uncompressed) YBR_FULL_422 even though the same dataset is valid.  The
    // stored layout is Y1 Y2 Cb Cr for each horizontal pair, so convert this
    // one well-defined native representation directly and keep the renderer's
    // high-precision RGBA16 contract.
    if (photometricInterpretation == QStringLiteral("YBR_FULL_422"))
    {
        QImage result(width, height, QImage::Format_RGBA64);
        if (result.isNull())
            return {};

        const double sampleMax = bitsStored >= 16
            ? 65535.0
            : static_cast<double>((1u << bitsStored) - 1u);
        if (sampleMax <= 0.0)
            return {};
        const double chromaCenter = static_cast<double>(1u << (bitsStored - 1));

        const auto toDisplay16 = [sampleMax](double value) -> quint16 {
            value = std::clamp(value, 0.0, sampleMax);
            return static_cast<quint16>(std::lround(value * 65535.0 / sampleMax));
        };

        for (int y = 0; y < height; ++y)
        {
            auto *dst = reinterpret_cast<QRgba64 *>(result.scanLine(y));
            for (int x = 0; x < width; ++x)
            {
                const QVector<quint16> components = rawComponents(x, y);
                if (components.size() != 3)
                    return {};

                const double yy = static_cast<double>(decodeStoredSample(components[0]));
                const double cb = static_cast<double>(decodeStoredSample(components[1])) - chromaCenter;
                const double cr = static_cast<double>(decodeStoredSample(components[2])) - chromaCenter;
                const quint16 r = toDisplay16(yy + 1.402 * cr);
                const quint16 g = toDisplay16(yy - 0.344136 * cb - 0.714136 * cr);
                const quint16 b = toDisplay16(yy + 1.772 * cb);
                dst[x] = QRgba64::fromRgba64(r, g, b, 65535);
            }
        }

        lastDisplayImage_ = result;
        return lastDisplayImage_;
    }

    const QByteArray nativePath =
        QFile::encodeName(path);

    const OFFilename dcmtkPath(
        nativePath.constData());

    //
    // DicomImage 負責正式 DICOM display pipeline：
    //
    // Pixel Data
    //   ↓
    // Modality LUT / Rescale
    //   ↓
    // VOI LUT / Window
    //   ↓
    // Presentation
    //
    DicomImage image(
        dcmtkPath,
        0,
        static_cast<unsigned long>(frameIndex_),
        1);

    if (image.getStatus() != EIS_Normal)
        return {};

    if (useCustomWindow && image.isMonochrome())
    {
        //
        // 使用 UI 指定的 WC / WW
        //
        if (!image.setWindow(
                wc,
                ww))
        {
            return {};
        }
    }
    else if (image.isMonochrome())
    {
        //
        // 優先使用 DICOM 原本的 Window Center / Width
        //
        if (image.getWindowCount() > 0)
        {
            if (!image.setWindow(0))
                return {};
        }
        //
        // 沒有 WC/WW，但有 VOI LUT
        //
        else if (image.getVoiLutCount() > 0)
        {
            if (!image.setVoiLut(0))
                return {};
        }
        //
        // DICOM 本身沒有指定 VOI
        //
        else
        {
            if (!image.setMinMaxWindow())
                return {};
        }
    }

    const int outputWidth =
        static_cast<int>(
            image.getWidth());

    const int outputHeight =
        static_cast<int>(
            image.getHeight());

    //
    // 重點：
    //
    // 不要求 DCMTK 輸出 8-bit。
    // 我們要求 16-bit display buffer。
    //
    const void *output =
        image.getOutputData(
            16,
            0,
            0);

    if (!output)
        return {};

    const unsigned long outputBytes = image.getOutputDataSize(16);
    const uint64_t pixelCount = static_cast<uint64_t>(outputWidth) *
                                static_cast<uint64_t>(outputHeight);
    const uint64_t requiredBytes = pixelCount * (image.isMonochrome() ? 2u : 6u);
    if (outputBytes < requiredBytes)
        return {};

    if (image.isMonochrome())
    {
        QImage result(
            reinterpret_cast<const uchar *>(output),
            outputWidth,
            outputHeight,
            outputWidth * static_cast<int>(sizeof(quint16)),
            QImage::Format_Grayscale16);
        lastDisplayImage_ = result.copy();
        return lastDisplayImage_;
    }

    // DCMTK planar=0 returns RGBRGB... with unsigned 16-bit samples.
    // Expand to RGBA16 because D3D11 has a native four-component UNORM format.
    QImage result(outputWidth, outputHeight, QImage::Format_RGBA64);
    if (result.isNull())
        return {};
    const auto *rgb = reinterpret_cast<const quint16 *>(output);
    for (int y = 0; y < outputHeight; ++y)
    {
        auto *dst = reinterpret_cast<QRgba64 *>(result.scanLine(y));
        for (int x = 0; x < outputWidth; ++x)
        {
            const qsizetype index = (static_cast<qsizetype>(y) * outputWidth + x) * 3;
            dst[x] = QRgba64::fromRgba64(rgb[index], rgb[index + 1], rgb[index + 2], 65535);
        }
    }
    lastDisplayImage_ = result;
    return lastDisplayImage_;
}

QString DicomFrame::summary() const
{
    if (!isValid())
    {
        return QStringLiteral(
            "Invalid DICOM frame");
    }

    QString text =
        QStringLiteral(
            "DICOM\n"
            "Size: %1 x %2\n"
            "Photometric: %3\n"
            "SamplesPerPixel: %4\n"
            "PlanarConfiguration: %5\n"
            "BitsAllocated: %6\n"
            "BitsStored: %7\n"
            "HighBit: %8\n"
            "PixelRepresentation: %9 (%10)\n"
            "RescaleSlope: %11\n"
            "RescaleIntercept: %12\n")
            .arg(width)
            .arg(height)
            .arg(photometricInterpretation)
            .arg(samplesPerPixel)
            .arg(planarConfiguration)
            .arg(bitsAllocated)
            .arg(bitsStored)
            .arg(highBit)
            .arg(pixelRepresentation)
            .arg(
                pixelRepresentation == 0
                    ? QStringLiteral("Unsigned")
                    : QStringLiteral("Signed"))
            .arg(rescaleSlope)
            .arg(rescaleIntercept);

    if (hasWindow)
    {
        text +=
            QStringLiteral(
                "WindowCenter: %1\n"
                "WindowWidth: %2\n")
                .arg(windowCenter)
                .arg(windowWidth);
    }
    else
    {
        text +=
            QStringLiteral(
                "WindowCenter/Width: Not specified\n");
    }

    text +=
        QStringLiteral(
            "Frame: %1 / %2\n"
            "TransferSyntax: %3")
            .arg(frameIndex_ + 1)
            .arg(frameCount)
            .arg(transferSyntaxUid);

    return text;
}

QString DicomFrame::pixelText(
    int x,
    int y,
    bool hexadecimal) const
{
    const DicomPixelSample s = pixel(x, y);

    if (!s.valid)
        return QStringLiteral("Invalid pixel");

    QStringList stored;
    for (int i = 0; i < s.storedComponents.size(); ++i)
    {
        const QString value = hexadecimal
            ? QStringLiteral("0x%1").arg(static_cast<quint32>(s.storedComponents[i]) & 0xffffu,
                                         4, 16, QLatin1Char('0')).toUpper()
            : QString::number(s.storedComponents[i]);
        stored << QStringLiteral("%1=%2").arg(s.componentNames.value(i), value);
    }

    QString text = QStringLiteral("Pixel (%1,%2) | Stored %3")
                       .arg(x).arg(y).arg(stored.join(QStringLiteral(" ")));
    if (isMonochrome())
        text += QStringLiteral(" | Rescaled=%1").arg(s.rescaledValue, 0, 'f', 3);
    if (!s.displayedComponents.isEmpty())
    {
        if (s.displayedComponents.size() == 1)
            text += QStringLiteral(" | Display Gray16=%1").arg(s.displayedComponents[0]);
        else
            text += QStringLiteral(" | Display RGB16=%1,%2,%3")
                        .arg(s.displayedComponents[0]).arg(s.displayedComponents[1])
                        .arg(s.displayedComponents[2]);
    }
    return text;
}

QString DicomFrame::cellText(
    int x,
    int y,
    bool hexadecimal) const
{
    const DicomPixelSample s = pixel(x, y);

    if (!s.valid)
        return {};

    QStringList values;
    for (qint32 value : s.storedComponents)
    {
        values << (hexadecimal
            ? QStringLiteral("0x%1").arg(static_cast<quint32>(value) & 0xffffu,
                                         4, 16, QLatin1Char('0')).toUpper()
            : QString::number(value));
    }
    return values.join(QStringLiteral("/"));
}
