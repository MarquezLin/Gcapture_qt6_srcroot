#include "display_output_info.h"

static QString pipelineToString(const DisplayOutputInfo::Pipeline &p)
{
    using Path = DisplayOutputInfo::Pipeline::Path;
    switch (p.path)
    {
    case Path::DirectShow:
        return QStringLiteral("DirectShow preview path");
    case Path::Gvfg:
        return QStringLiteral("GVFG preview path");
    default:
        return QStringLiteral("Preview path: unknown");
    }
}

static QString previewGpuToString(const DisplayOutputInfo::Pipeline &p)
{
    using Path = DisplayOutputInfo::Pipeline::Path;

    if (!p.adapterName.isEmpty())
    {
        if (p.adapterIndex >= 0)
            return QStringLiteral("Preview GPU: %1 (DXGI index %2)").arg(p.adapterName).arg(p.adapterIndex);
        return QStringLiteral("Preview GPU: %1").arg(p.adapterName);
    }

    switch (p.path)
    {
    case Path::Gvfg:
        return QStringLiteral("Preview GPU: not reported by GVFG preview helper");
    case Path::DirectShow:
        return QStringLiteral("Preview GPU: not reported by active DirectShow renderer");
    default:
        return QStringLiteral("Preview GPU: unknown");
    }
}

QString formatDisplayOutputInfo(const DisplayOutputInfo &info)
{
    QStringList lines;

    // 1) Desktop
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

    // 2) Output color
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

    // 3) Pipeline
    lines << pipelineToString(info.pipe);
    lines << QString("Preview Render Format: %1")
                 .arg(info.pipe.renderFormat.isEmpty() ? QStringLiteral("(unknown)") : info.pipe.renderFormat);
    lines << previewGpuToString(info.pipe);

    return lines.join('\n');
}
