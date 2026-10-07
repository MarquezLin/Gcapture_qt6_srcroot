#pragma once
#include <QString>
#include <QStringList>
#include <QSize>

struct DisplayOutputInfo
{
    // ---- Desktop / Screen (Qt QScreen) ----
    struct Desktop
    {
        QSize size;      // scr->size()
        double hz = 0.0; // scr->refreshRate()
        int bpp = 0;     // scr->depth()
    } desktop;

    // ---- Output color / HDR (DXGI/NVAPI from monitor) ----
    struct OutputColor
    {
        bool valid = false;
        int bitsPerColor = 0;  // bpc
        QString colorFormat;   // RGB / YCbCr 4:4:4 ...
        QString dynamicRange;  // Full / Limited
        QString colorSpaceHdr; // SDR (BT.709) / HDR10 (BT.2020) ...
    } color;

    // ---- Pipeline / Backend description (runtime state) ----
    struct Pipeline
    {
        enum class Path
        {
            Unknown = -1,
            DirectShow = 0,
            Gvfg = 1
        };

        Path path = Path::Unknown;
        QString adapterName; // Actual preview-processing GPU when reported.
        int adapterIndex = -1;
        QString renderFormat; // Actual format used by the preview renderer.
    } pipe;
};

// formatter（專門負責顯示字串）
QString formatDisplayOutputInfo(const DisplayOutputInfo &info);
