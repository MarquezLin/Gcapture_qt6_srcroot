#ifndef RAWFRAME_H
#define RAWFRAME_H

#include <QByteArray>
#include <QImage>
#include <QString>

enum class RawPixelFormat
{
    Yvyu,
    Y210,
    Bgra8,
    Rgba8,
    Abgr2101010
};

struct RawPixelSample
{
    bool valid = false;
    int x = 0;
    int y = 0;
    quint64 byteOffset = 0;
    quint32 packedValue = 0;
    quint16 yValue = 0;
    quint16 uValue = 0;
    quint16 vValue = 0;
    quint16 rValue = 0;
    quint16 gValue = 0;
    quint16 bValue = 0;
    quint16 aValue = 0;
    quint16 storedWords[4] = {};
};

class RawFrame
{
public:
    bool load(const QString &path, int width, int height, int strideBytes,
              RawPixelFormat format, QString *error);
    bool isValid() const;
    RawPixelSample pixel(int x, int y) const;
    QImage makePreview() const;
    QString pixelText(int x, int y, bool hexadecimal = false) const;
    QString cellText(int x, int y, bool hexadecimal = false) const;

    static QString formatName(RawPixelFormat format);
    static int minimumStride(int width, RawPixelFormat format);

    QString path;
    QByteArray bytes;
    int width = 0;
    int height = 0;
    int strideBytes = 0;
    RawPixelFormat format = RawPixelFormat::Yvyu;
};

#endif
