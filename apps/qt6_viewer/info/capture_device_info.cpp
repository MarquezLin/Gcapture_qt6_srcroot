#include "capture_device_info.h"

static QString pixFmtToString(gcap_pixfmt_t f)
{
    switch (f)
    {
    case GCAP_FMT_NV12: return "NV12";
    case GCAP_FMT_YUY2: return "YUY2";
    case GCAP_FMT_P010: return "P010";
    case GCAP_FMT_ARGB: return "ARGB";
    case GCAP_FMT_V210: return "V210";
    case GCAP_FMT_R210: return "R210";
    case GCAP_FMT_Y210: return "Y210";
    default: return "Unknown";
    }
}

static QString sampleFamilyToString(gcap_pixfmt_t f)
{
    switch (f)
    {
    case GCAP_FMT_NV12: return "YUV420";
    case GCAP_FMT_YUY2:
    case GCAP_FMT_Y210:
    case GCAP_FMT_V210: return "YUV422";
    case GCAP_FMT_P010: return "YUV420 10-bit";
    case GCAP_FMT_ARGB:
    case GCAP_FMT_R210: return "RGB";
    default: return "Unknown";
    }
}

static QString formatStatusBlock(const QString &title, const gcap_signal_status_t &s, double fallbackFps, const QString &fmtOverride = QString())
{
    QStringList lines;
    lines << title;
    lines << "────────────";
    if (s.width <= 0 || s.height <= 0)
    {
        lines << "(unknown)";
        return lines.join("\n");
    }

    const double fps = (s.fps_den > 0) ? double(s.fps_num) / double(s.fps_den) : fallbackFps;
    const QString fmt = fmtOverride.isEmpty() ? pixFmtToString(s.pixfmt) : fmtOverride;
    lines << QString("Resolution    : %1 x %2").arg(s.width).arg(s.height);
    lines << QString("Frame Rate    : %1 fps").arg(fps, 0, 'f', 2);
    lines << QString("Sample Format : %1").arg(fmt);
    lines << QString("Sample Family : %1").arg(sampleFamilyToString(s.pixfmt));
    lines << QString("Bit Depth     : %1-bit").arg(s.bit_depth > 0 ? QString::number(s.bit_depth) : QStringLiteral("unknown"));
    return lines.join("\n");
}

QString formatCaptureDeviceInfo(const CaptureDeviceInfo &info, double fallbackFps)
{
    QStringList lines;

    lines << "Basic";
    lines << "────────────";
    lines << QString("Device        : %1").arg(info.deviceName.isEmpty() ? QStringLiteral("(unknown)") : info.deviceName);
    lines << QString("Backend       : %1").arg(info.backendName.isEmpty() ? QStringLiteral("(unknown)") : info.backendName);
    lines << QString("Frame Source  : %1").arg(info.frameSource.isEmpty() ? QStringLiteral("(unknown)") : info.frameSource);
    lines << QString("Path          : %1").arg(info.pathName.isEmpty() ? QStringLiteral("(unknown)") : info.pathName);
    lines << QString("Audio         : %1").arg(info.audioInfo.isEmpty() ? QStringLiteral("(unknown)") : info.audioInfo);

    lines << "";
    lines << formatStatusBlock("Current Capture", info.negotiated, fallbackFps, info.captureFormat);

    lines << "";
    lines << formatStatusBlock("Driver / Pin Probe", info.signalProbe, fallbackFps);

    if (!info.supportedFormats.isEmpty())
    {
        lines << "";
        lines << "Supported Formats";
        lines << "────────────";
        for (const QString &s : info.supportedFormats)
            lines << QString("- %1").arg(s);
    }

    if (!info.propertyPages.isEmpty())
    {
        lines << "";
        lines << "Available Property Pages";
        lines << "────────────";
        for (const QString &s : info.propertyPages)
            lines << QString("- %1").arg(s);
    }

    lines << "";
    lines << "Device Properties";
    lines << "────────────";
    lines << QString("Driver Version : %1").arg(info.driverVersion.isEmpty() ? QStringLiteral("(unknown)") : info.driverVersion);
    lines << QString("Firmware Ver.  : %1").arg(info.firmwareVersion.isEmpty() ? QStringLiteral("(unknown)") : info.firmwareVersion);
    lines << QString("Serial Number  : %1").arg(info.serialNumber.isEmpty() ? QStringLiteral("(unknown)") : info.serialNumber);
    lines << QString("Render Format  : %1").arg(info.renderFormat.isEmpty() ? QStringLiteral("(unknown)") : info.renderFormat);

    lines << "";
    lines << "Notes";
    lines << "────────────";
    lines << "This device does not expose SC0710 vendor signal metadata.";
    lines << "Values shown here are capture/backend formats and DShow probe results.";
    lines << "They are not guaranteed to be true HDMI input signal metadata.";

    return lines.join("\n");
}
