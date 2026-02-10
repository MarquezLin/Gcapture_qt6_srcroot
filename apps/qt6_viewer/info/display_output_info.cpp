#include "display_output_info.h"

static QString pipelineToString(const DisplayOutputInfo::Pipeline &p)
{
    using Path = DisplayOutputInfo::Pipeline::Path;
    switch (p.path)
    {
    case Path::WinMFGpu:
        return QStringLiteral("NV12→RGBA: GPU path (WinMF GPU), Adapter: %1 (index %2)")
            .arg(p.adapterName.isEmpty() ? QStringLiteral("(default)") : p.adapterName)
            .arg(p.adapterIndex);
    case Path::WinMFCpu:
        return QStringLiteral("NV12→RGBA: CPU path (WinMF CPU)");
    case Path::DirectShow:
        return QStringLiteral("NV12→RGBA: N/A (DirectShow backend)");
    default:
        return QStringLiteral("NV12→RGBA: (unknown)");
    }
}

QString formatDisplayOutputInfo(const DisplayOutputInfo &info)
{
    QStringList lines;

    // 1) Video
    if (info.video.size.isValid())
    {
        lines << QString("Video Format: %1x%2 @ %3 fps (Device Output)")
                     .arg(info.video.size.width())
                     .arg(info.video.size.height())
                     .arg(info.video.fps, 0, 'f', 2);
    }
    else
    {
        lines << "Video Format: (unknown)";
    }

    // 2) Desktop
    if (info.desktop.size.isValid())
    {
        lines << QString("Desktop: %1x%2 @ %3 Hz, %4 bpp")
                     .arg(info.desktop.size.width())
                     .arg(info.desktop.size.height())
                     .arg(info.desktop.hz, 0, 'f', 2)
                     .arg(info.desktop.bpp);
    }
    else
    {
        lines << "Desktop: (unknown)";
    }

    // 3) Output color
    if (info.color.valid)
    {
        lines << QString("Output Color Depth: %1 bpc").arg(info.color.bitsPerColor);
        lines << QString("Output Color Format: %1").arg(info.color.colorFormat);
        lines << QString("Output Dynamic Range: %1").arg(info.color.dynamicRange);
        lines << QString("Color Space / HDR: %1").arg(info.color.colorSpaceHdr);
    }
    else
    {
        lines << "Output Color Info: unavailable";
    }

    // 4) Pipeline
    lines << pipelineToString(info.pipe);

    return lines.join('\n');
}
