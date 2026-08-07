#include "rawframe.h"

#include <QFile>
#include <QtEndian>
#include <algorithm>

namespace
{
quint16 readLe16(const char *p)
{
    return qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(p));
}

quint32 readLe32(const char *p)
{
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(p));
}

int clamp8(int value)
{
    return std::clamp(value, 0, 255);
}

QRgb yuvToRgb(int y, int u, int v, int bits)
{
    const int shift = std::max(0, bits - 8);
    y >>= shift;
    u >>= shift;
    v >>= shift;
    const int c = std::max(0, y - 16);
    const int d = u - 128;
    const int e = v - 128;
    return qRgb(clamp8((298 * c + 409 * e + 128) >> 8),
                clamp8((298 * c - 100 * d - 208 * e + 128) >> 8),
                clamp8((298 * c + 516 * d + 128) >> 8));
}

QString number(quint32 value, bool hex, int digits = 0)
{
    if (!hex)
        return QString::number(value);
    const QString hexValue = QStringLiteral("%1")
                                 .arg(value, digits, 16, QLatin1Char('0'))
                                 .toUpper();
    return QStringLiteral("0x%1").arg(hexValue);
}
}

bool RawFrame::load(const QString &filePath, int w, int h, int stride,
                    RawPixelFormat pixelFormat, QString *error)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
    {
        if (error)
            *error = file.errorString();
        return false;
    }

    const int minStride = minimumStride(w, pixelFormat);
    if (w <= 0 || h <= 0 || stride < minStride)
    {
        if (error)
            *error = QStringLiteral("Invalid dimensions/stride. Minimum stride is %1 bytes.").arg(minStride);
        return false;
    }

    const qint64 required = qint64(stride) * h;
    if (file.size() < required)
    {
        if (error)
            *error = QStringLiteral("RAW file is too small: %1 bytes, expected at least %2.")
                         .arg(file.size()).arg(required);
        return false;
    }

    bytes = file.read(required);
    path = filePath;
    width = w;
    height = h;
    strideBytes = stride;
    format = pixelFormat;
    return bytes.size() == required;
}

bool RawFrame::isValid() const
{
    return width > 0 && height > 0 && strideBytes >= minimumStride(width, format) &&
           bytes.size() >= qint64(strideBytes) * height;
}

int RawFrame::minimumStride(int width, RawPixelFormat format)
{
    switch (format)
    {
    case RawPixelFormat::Yvyu: return ((width + 1) / 2) * 4;
    case RawPixelFormat::Y210: return ((width + 1) / 2) * 8;
    case RawPixelFormat::Bgra8:
    case RawPixelFormat::Rgba8:
    case RawPixelFormat::Abgr2101010: return width * 4;
    }
    return 0;
}

QString RawFrame::formatName(RawPixelFormat format)
{
    switch (format)
    {
    case RawPixelFormat::Yvyu: return QStringLiteral("YVYU");
    case RawPixelFormat::Y210: return QStringLiteral("Y210");
    case RawPixelFormat::Bgra8: return QStringLiteral("BGRA8");
    case RawPixelFormat::Rgba8: return QStringLiteral("RGBA8");
    case RawPixelFormat::Abgr2101010: return QStringLiteral("ABGR2101010");
    }
    return QStringLiteral("Unknown");
}

RawPixelSample RawFrame::pixel(int x, int y) const
{
    RawPixelSample s;
    if (!isValid() || x < 0 || y < 0 || x >= width || y >= height)
        return s;

    s.valid = true;
    s.x = x;
    s.y = y;
    const char *row = bytes.constData() + qint64(y) * strideBytes;

    if (format == RawPixelFormat::Yvyu)
    {
        const int pairX = x & ~1;
        s.byteOffset = quint64(y) * strideBytes + quint64(pairX / 2) * 4;
        const uchar *p = reinterpret_cast<const uchar *>(row + (pairX / 2) * 4);
        s.yValue = p[(x & 1) ? 2 : 0];
        // GVFG DMA YVYU byte order: Y0, V, Y1, U.
        s.uValue = p[3];
        s.vValue = p[1];
        s.packedValue = quint32(p[0]) | (quint32(p[1]) << 8) |
                        (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
    }
    else if (format == RawPixelFormat::Y210)
    {
        const int pairX = x & ~1;
        s.byteOffset = quint64(y) * strideBytes + quint64(pairX / 2) * 8;
        const char *p = row + (pairX / 2) * 8;
        for (int i = 0; i < 4; ++i)
            s.storedWords[i] = readLe16(p + i * 2);
        s.yValue = s.storedWords[(x & 1) ? 2 : 0] >> 6;
        // Standard Y210 word order: Y0, U, Y1, V. Any temporary FPGA U/V
        // workaround belongs in the GVFG SDK conversion path, not the app.
        s.uValue = s.storedWords[1] >> 6;
        s.vValue = s.storedWords[3] >> 6;
    }
    else
    {
        s.byteOffset = quint64(y) * strideBytes + quint64(x) * 4;
        const char *p = row + x * 4;
        s.packedValue = readLe32(p);
        if (format == RawPixelFormat::Bgra8 || format == RawPixelFormat::Rgba8)
        {
            const uchar *u = reinterpret_cast<const uchar *>(p);
            if (format == RawPixelFormat::Bgra8)
            {
                s.bValue = u[0]; s.gValue = u[1]; s.rValue = u[2]; s.aValue = u[3];
            }
            else
            {
                s.rValue = u[0]; s.gValue = u[1]; s.bValue = u[2]; s.aValue = u[3];
            }
        }
        else
        {
            s.rValue = s.packedValue & 0x3ffu;
            s.gValue = (s.packedValue >> 10) & 0x3ffu;
            s.bValue = (s.packedValue >> 20) & 0x3ffu;
            s.aValue = (s.packedValue >> 30) & 0x3u;
        }
    }
    return s;
}

QImage RawFrame::makePreview() const
{
    if (!isValid())
        return {};
    QImage image(width, height, QImage::Format_ARGB32);
    for (int y = 0; y < height; ++y)
    {
        QRgb *dst = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < width; ++x)
        {
            const RawPixelSample s = pixel(x, y);
            if (format == RawPixelFormat::Yvyu)
                dst[x] = yuvToRgb(s.yValue, s.uValue, s.vValue, 8);
            else if (format == RawPixelFormat::Y210)
                dst[x] = yuvToRgb(s.yValue, s.uValue, s.vValue, 10);
            else if (format == RawPixelFormat::Bgra8 || format == RawPixelFormat::Rgba8)
                dst[x] = qRgba(s.rValue, s.gValue, s.bValue, s.aValue);
            else
                dst[x] = qRgba((s.rValue * 255 + 511) / 1023,
                               (s.gValue * 255 + 511) / 1023,
                               (s.bValue * 255 + 511) / 1023,
                               s.aValue * 85);
        }
    }
    return image;
}

QString RawFrame::cellText(int x, int y, bool hex) const
{
    const RawPixelSample s = pixel(x, y);
    if (!s.valid)
        return {};
    if (format == RawPixelFormat::Yvyu || format == RawPixelFormat::Y210)
        return QStringLiteral("Y %1\nU %2\nV %3")
            .arg(number(s.yValue, hex)).arg(number(s.uValue, hex)).arg(number(s.vValue, hex));
    return QStringLiteral("R %1\nG %2\nB %3\nA %4")
        .arg(number(s.rValue, hex)).arg(number(s.gValue, hex))
        .arg(number(s.bValue, hex)).arg(number(s.aValue, hex));
}

QString RawFrame::pixelText(int x, int y, bool hex) const
{
    const RawPixelSample s = pixel(x, y);
    if (!s.valid)
        return QStringLiteral("Pixel: --");
    QString text = QStringLiteral("Pixel x=%1 y=%2  Format=%3  Offset=%4  ")
                       .arg(x).arg(y).arg(formatName(format))
                       .arg(number(quint32(s.byteOffset), true, 8));
    if (format == RawPixelFormat::Yvyu)
        return text + QStringLiteral("Y=%1 U=%2 V=%3  PairPackedLE=%4")
            .arg(number(s.yValue, hex)).arg(number(s.uValue, hex)).arg(number(s.vValue, hex))
            .arg(number(s.packedValue, true, 8));
    if (format == RawPixelFormat::Y210)
        return text + QStringLiteral("Y10=%1 U10=%2 V10=%3  Padding=%4")
            .arg(number(s.yValue, hex)).arg(number(s.uValue, hex)).arg(number(s.vValue, hex))
            .arg(((s.storedWords[0] | s.storedWords[1] | s.storedWords[2] | s.storedWords[3]) & 0x3f) == 0
                     ? QStringLiteral("OK") : QStringLiteral("NON-ZERO"));
    return text + QStringLiteral("R=%1 G=%2 B=%3 A=%4  PackedLE=%5")
        .arg(number(s.rValue, hex)).arg(number(s.gValue, hex))
        .arg(number(s.bValue, hex)).arg(number(s.aValue, hex))
        .arg(number(s.packedValue, true, 8));
}
