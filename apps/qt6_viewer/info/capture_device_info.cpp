#include "capture_device_info.h"

static QString pixFmtToString(gcap_pixfmt_t f)
{
    switch (f)
    {
    case GCAP_FMT_NV12:
        return "NV12";
    case GCAP_FMT_YUY2:
        return "YUY2";
    case GCAP_FMT_P010:
        return "P010";
    case GCAP_FMT_ARGB:
        return "ARGB";
    case GCAP_FMT_V210:
        return "V210";
    case GCAP_FMT_R210:
        return "R210";
    default:
        return "Unknown";
    }
}

QString formatCaptureDeviceInfo(const CaptureDeviceInfo &info,
                                double fallbackFps)
{
    QStringList lines;

    lines << "Input Signal";
    lines << "────────────";

    if (info.hasSignal())
    {
        double fps = (info.signal.fps_den > 0)
                         ? double(info.signal.fps_num) / info.signal.fps_den
                         : fallbackFps;

        lines << QString("Resolution    : %1 x %2")
                     .arg(info.signal.width)
                     .arg(info.signal.height);
        // lines << QString("Frame Rate    : %1 fps")
        //              .arg(fps, 0, 'f', 2);
        lines << QString("Pixel Format  : %1")
                     .arg(pixFmtToString(info.signal.pixfmt));
        lines << QString("Bit Depth     : %1-bit")
                     .arg(info.signal.bit_depth);
    }
    else
    {
        lines << "Signal         : (unknown)";
    }

    lines << "";
    lines << "Device Properties";
    lines << "────────────";

    lines << QString("Driver Version : %1").arg(info.driverVersion.isEmpty() ? "(unknown)" : info.driverVersion);
    lines << QString("Firmware Ver.  : %1").arg(info.firmwareVersion.isEmpty() ? "(unknown)" : info.firmwareVersion);
    lines << QString("Serial Number  : %1").arg(info.serialNumber.isEmpty() ? "(unknown)" : info.serialNumber);

    return lines.join("\n");
}
