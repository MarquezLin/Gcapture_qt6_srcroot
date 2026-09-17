#include "dicomframe.h"

#include <dcmtk/dcmdata/dctk.h>
#include <dcmtk/dcmdata/dcxfer.h>
#include <dcmtk/dcmimgle/dcmimage.h>

#include <QFile>

void DicomFrame::clear()
{
    path.clear();

    width = 0;
    height = 0;

    samplesPerPixel = 0;

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
}

bool DicomFrame::load(const QString &filePath,
                      QString *error)
{
    clear();

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

    //
    // 第一版先不處理 compressed PixelData
    //
    DcmXfer xfer(dataset->getOriginalXfer());

    if (xfer.isEncapsulated())
    {
        if (error)
        {
            *error =
                QStringLiteral(
                    "Compressed DICOM is not supported yet.\n"
                    "Transfer Syntax: %1")
                    .arg(transferSyntaxUid);
        }

        return false;
    }

    Uint16 rows = 0;
    Uint16 columns = 0;
    Uint16 spp = 0;

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

    //
    // 第一版只支援 single frame grayscale
    //
    if (frameCount != 1)
    {
        if (error)
        {
            *error =
                QStringLiteral(
                    "Multi-frame DICOM is not supported yet. "
                    "Frames = %1")
                    .arg(frameCount);
        }

        return false;
    }

    if (samplesPerPixel != 1)
    {
        if (error)
        {
            *error =
                QStringLiteral(
                    "Only grayscale DICOM is supported yet.");
        }

        return false;
    }

    if (photometricInterpretation !=
            QStringLiteral("MONOCHROME1") &&
        photometricInterpretation !=
            QStringLiteral("MONOCHROME2"))
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
    const qsizetype expectedPixels =
        static_cast<qsizetype>(width) *
        static_cast<qsizetype>(height);

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

    const qsizetype expected =
        static_cast<qsizetype>(width) *
        static_cast<qsizetype>(height);

    return rawSamples_.size() >= expected;
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

    const qsizetype index =
        static_cast<qsizetype>(y) *
            width +
        x;

    const quint16 raw =
        rawSamples_[index];

    const qint32 stored =
        decodeStoredSample(raw);

    sample.valid = true;

    sample.x = x;
    sample.y = y;

    sample.storedWord = raw;
    sample.storedValue = stored;

    //
    // 這裡只代表 Rescale Slope/Intercept 路徑。
    // 正式 Display pipeline 則完全交給 DicomImage。
    //
    sample.rescaledValue =
        static_cast<double>(stored) *
            rescaleSlope +
        rescaleIntercept;

    return sample;
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
        0,
        1);

    if (image.getStatus() != EIS_Normal)
        return {};

    if (useCustomWindow)
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
    else
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

    //
    // DCMTK 管理 output buffer，
    // 所以 QImage 最後一定 copy()。
    //
    QImage result(
        reinterpret_cast<const uchar *>(
            output),
        outputWidth,
        outputHeight,
        outputWidth *
            static_cast<int>(
                sizeof(quint16)),
        QImage::Format_Grayscale16);

    return result.copy();
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
            "BitsAllocated: %4\n"
            "BitsStored: %5\n"
            "HighBit: %6\n"
            "PixelRepresentation: %7 (%8)\n"
            "RescaleSlope: %9\n"
            "RescaleIntercept: %10\n")
            .arg(width)
            .arg(height)
            .arg(photometricInterpretation)
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
            "Frames: %1\n"
            "TransferSyntax: %2")
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

    QString wordText;

    if (hexadecimal)
    {
        wordText =
            QStringLiteral("0x%1")
                .arg(s.storedWord,
                     4,
                     16,
                     QLatin1Char('0'))
                .toUpper();
    }
    else
    {
        wordText =
            QString::number(s.storedWord);
    }

    return QStringLiteral(
               "Pixel (%1,%2) | "
               "StoredWord=%3 | "
               "StoredValue=%4 | "
               "RescaledValue=%5")
        .arg(x)
        .arg(y)
        .arg(wordText)
        .arg(s.storedValue)
        .arg(s.rescaledValue, 0, 'f', 3);
}

QString DicomFrame::cellText(
    int x,
    int y,
    bool hexadecimal) const
{
    const DicomPixelSample s = pixel(x, y);

    if (!s.valid)
        return {};

    if (hexadecimal)
    {
        return QStringLiteral("0x%1")
            .arg(s.storedWord,
                 4,
                 16,
                 QLatin1Char('0'))
            .toUpper();
    }

    return QString::number(s.storedValue);
}