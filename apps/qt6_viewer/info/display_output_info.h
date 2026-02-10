#pragma once
#include <QString>
#include <QStringList>
#include <QSize>

struct DisplayOutputInfo
{
    // ---- Video (what you render / show) ----
    struct Video
    {
        QSize size;       // img.size()
        double fps = 0.0; // avgFps_
    } video;

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
            WinMFCpu = 0,
            WinMFGpu = 1,
            DirectShow = 2
        };

        Path path = Path::Unknown;
        QString adapterName; // "NVIDIA ..."
        int adapterIndex = -1;

        // 你若想也可以加 negotiated pixfmt / bitdepth 等
        // QString notes;
    } pipe;
};

// formatter（專門負責顯示字串）
QString formatDisplayOutputInfo(const DisplayOutputInfo &info);