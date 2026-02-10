#include "display_info.h"
#include <QString>

static void dbgW(const std::wstring &s)
{
    std::wstring msg = L"[Display_info] ";
    msg += s;
    msg += L"\n";
    OutputDebugStringW(msg.c_str());
}

#ifdef GCAP_ENABLE_NVAPI
#include "nvapi.h"

static bool g_nvapiTried = false;
static bool g_nvapiAvailable = false;

static bool ensureNvapi()
{
    if (g_nvapiTried)
        return g_nvapiAvailable;

    g_nvapiTried = true;
    NvAPI_Status st = NvAPI_Initialize();
    g_nvapiAvailable = (st == NVAPI_OK);
    return g_nvapiAvailable;
}

// 利用 NVAPI 把「線上的輸出格式」補上（只在 NVIDIA 上會成功）
static void fillNvapiColorInfo(const wchar_t *deviceName, GpuDisplayInfo &info)
{
    if (!ensureNvapi())
        return;

    // DXGI 給的名稱是 L"\\\\.\\DISPLAY1" 這種，要轉成 ANSI 給 NVAPI
    char ansiName[64] = {};
    WideCharToMultiByte(CP_ACP, 0, deviceName, -1,
                        ansiName, sizeof(ansiName), nullptr, nullptr);

    NvU32 displayId = 0;
    if (NvAPI_DISP_GetDisplayIdByDisplayName(ansiName, &displayId) != NVAPI_OK)
        return;

    NV_COLOR_DATA color{};
    color.version = NV_COLOR_DATA_VER; // nvapi.h 裡面的 macro
    color.size = sizeof(color);
    color.cmd = NV_COLOR_CMD_GET; // 表示「讀取目前設定」

    if (NvAPI_Disp_ColorControl(displayId, &color) != NVAPI_OK)
        return;

    auto &d = color.data; // 裡面有 colorFormat / dynamicRange / bpc ...

    // ---- 用 NVAPI 覆寫 BitsPerColor ----
    switch (d.bpc)
    {
    case NV_BPC_6:
        info.bitsPerColor = 6;
        break;
    case NV_BPC_8:
        info.bitsPerColor = 8;
        break;
    case NV_BPC_10:
        info.bitsPerColor = 10;
        break;
    case NV_BPC_12:
        info.bitsPerColor = 12;
        break;
    default:
        break;
    }

    // ---- 線上的輸出色彩格式 (RGB / YCbCr422 / 444 / 420) ----
    switch (d.colorFormat)
    {
    case NV_COLOR_FORMAT_RGB:
        info.colorSpaceStr = "RGB";
        break;
    case NV_COLOR_FORMAT_YUV422:
        info.colorSpaceStr = "YCbCr 4:2:2";
        break;
    case NV_COLOR_FORMAT_YUV444:
        info.colorSpaceStr = "YCbCr 4:4:4";
        break;
    case NV_COLOR_FORMAT_YUV420:
        info.colorSpaceStr = "YCbCr 4:2:0";
        break;
    default:
        break; // 保留 DXGI 的字串
    }

    // ---- Full / Limited 範圍 ----
    switch (d.dynamicRange)
    {
    case NV_DYNAMIC_RANGE_VESA: // PC range
        info.dynamicRangeStr = "Full";
        break;
    case NV_DYNAMIC_RANGE_CEA: // Video range
        info.dynamicRangeStr = "Limited";
        break;
    default:
        break;
    }
}
#endif // GCAP_ENABLE_NVAPI

static void fillColorInfoFromDesc(const DXGI_OUTPUT_DESC1 &desc, GpuDisplayInfo &info)
{
    info.bitsPerColor = desc.BitsPerColor;
    info.cs = desc.ColorSpace;

    // ---- 色彩空間解析 ----
    switch (desc.ColorSpace)
    {
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
        info.colorSpaceStr = "RGB";
        info.dynamicRangeStr = "Full";
        info.gamutStr = "SDR (BT.709)";
        break;

    case DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709:
        info.colorSpaceStr = "RGB";
        info.dynamicRangeStr = "Limited";
        info.gamutStr = "SDR (BT.709)";
        break;

    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
        info.colorSpaceStr = "RGB";
        info.dynamicRangeStr = "Full";
        info.gamutStr = "HDR10 (PQ, BT.2020)";
        break;

    case DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020:
        info.colorSpaceStr = "RGB";
        info.dynamicRangeStr = "Limited";
        info.gamutStr = "HDR10 (PQ, BT.2020)";
        break;

    case DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709:
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709:
        info.colorSpaceStr = "YCbCr";
        info.dynamicRangeStr = (desc.ColorSpace == DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709)
                                   ? "Full"
                                   : "Limited";
        info.gamutStr = "SDR (BT.709)";
        break;

    case DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020:
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020:
        info.colorSpaceStr = "YCbCr";
        info.dynamicRangeStr = (desc.ColorSpace == DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020)
                                   ? "Full"
                                   : "Limited";
        info.gamutStr = "HDR10 (PQ, BT.2020)";
        break;

    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
        info.colorSpaceStr = "RGB";
        info.dynamicRangeStr = "Full";
        info.gamutStr = "SDR (BT.709, 10-bit)";
        break;

    default:
        info.colorSpaceStr = "Unknown";
        info.dynamicRangeStr = "";
        info.gamutStr = "";
        break;
    }
}

GDISPLAY_API GpuDisplayInfo queryDisplayInfoForMonitor(HMONITOR hmon)
{
    GpuDisplayInfo info;

    if (!hmon)
        return info;

    // 建立 DXGI Factory
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return info;

    for (UINT adapterIndex = 0;; ++adapterIndex)
    {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(adapterIndex, &adapter) == DXGI_ERROR_NOT_FOUND)
            break;

        for (UINT outputIndex = 0;; ++outputIndex)
        {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(outputIndex, &output) == DXGI_ERROR_NOT_FOUND)
                break;

            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc)))
                continue;

            // 只找跟傳進來的 HMONITOR 相同的那一顆螢幕
            if (desc.Monitor != hmon)
                continue;

            ComPtr<IDXGIOutput6> output6;
            if (FAILED(output.As(&output6)))
                return info;

            DXGI_OUTPUT_DESC1 desc1{};
            if (FAILED(output6->GetDesc1(&desc1)))
                return info;

            info.valid = true;

            // 先用 DXGI 取得 BitsPerColor / DXGI ColorSpace
            // dbgW(L"dxgi api");
            fillColorInfoFromDesc(desc1, info);

#ifdef GCAP_ENABLE_NVAPI
            // 如果是 NVIDIA 顯卡，再用 NVAPI 取得「實際輸出格式」，覆寫掉部分欄位
            // dbgW(L"Nvidia api");
            fillNvapiColorInfo(desc.DeviceName, info);
#endif
            return info; // 找到就回傳
        }
    }

    return info; // 沒找到對應的螢幕
}

GDISPLAY_API GpuDisplayInfo queryPrimaryDisplayInfo()
{
    HMONITOR hmon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    return queryDisplayInfoForMonitor(hmon);
}
