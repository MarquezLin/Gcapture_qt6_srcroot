#pragma once
#include <QString>
#include <dxgi1_6.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

#ifdef _WIN32
#ifdef GDISPLAY_BUILD
#define GDISPLAY_API __declspec(dllexport)
#else
#define GDISPLAY_API __declspec(dllimport)
#endif
#else
#define GDISPLAY_API
#endif

struct GpuDisplayInfo
{
    bool valid = false;

    UINT bitsPerColor = 0; // 8 / 10 / 12 bpc
    DXGI_COLOR_SPACE_TYPE cs = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;

    QString colorSpaceStr;   // RGB / YCbCr
    QString dynamicRangeStr; // Full / Limited
    QString gamutStr;        // BT.709 / BT.2020 / HDR10 / HLG
};

// ----------------------------
// 取得主螢幕的 GPU 顯示輸出資訊
// ----------------------------
GDISPLAY_API GpuDisplayInfo queryPrimaryDisplayInfo();

// 依據 HMONITOR 查對應螢幕的資訊
GDISPLAY_API GpuDisplayInfo queryDisplayInfoForMonitor(HMONITOR hmon);