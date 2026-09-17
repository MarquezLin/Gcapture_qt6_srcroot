#ifndef DICOMFRAME_H
#define DICOMFRAME_H

#include <QImage>
#include <QString>
#include <QVector>
#include <QtGlobal>

struct DicomPixelSample
{
    bool valid = false;

    int x = 0;
    int y = 0;

    // DICOM container 裡原始值
    quint16 storedWord = 0;

    // 依 BitsStored / HighBit / PixelRepresentation
    // 解出的真正原始 Pixel Value
    qint32 storedValue = 0;

    // 套用 Rescale Slope / Intercept 後的值
    // 例如 CT 可能會變成 HU
    double rescaledValue = 0.0;
};

class DicomFrame
{
public:
    bool load(const QString &filePath,
              QString *error = nullptr);

    void clear();

    bool isValid() const;

    DicomPixelSample pixel(int x,
                           int y) const;

    // 使用 DICOM 檔案內建的 VOI Window / VOI LUT
    QImage makeDisplayImage() const;

    // 使用使用者指定的 Window Center / Width
    QImage makeDisplayImage(double windowCenter,
                            double windowWidth) const;

    QString summary() const;

    QString pixelText(int x,
                      int y,
                      bool hexadecimal = false) const;

    QString cellText(int x,
                     int y,
                     bool hexadecimal = false) const;

public:
    QString path;

    int width = 0;
    int height = 0;

    int samplesPerPixel = 0;

    int bitsAllocated = 0;
    int bitsStored = 0;
    int highBit = 0;

    // 0 = unsigned
    // 1 = signed
    int pixelRepresentation = 0;

    int frameCount = 0;

    QString photometricInterpretation;
    QString transferSyntaxUid;

    // Modality transformation
    double rescaleSlope = 1.0;
    double rescaleIntercept = 0.0;
    bool hasRescale = false;

    // 第一組 WC / WW
    double windowCenter = 0.0;
    double windowWidth = 0.0;
    bool hasWindow = false;

private:
    qint32 decodeStoredSample(quint16 raw) const;

    QImage renderDisplay(bool useCustomWindow,
                         double wc,
                         double ww) const;

    QVector<quint16> rawSamples_;
};

#endif