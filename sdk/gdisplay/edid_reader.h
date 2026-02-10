#pragma once

#include <QByteArray>
#include <QString>

#ifdef _WIN32
#include <windows.h>
#endif

#ifndef GDISPLAY_API
#ifdef _WIN32
#ifdef GDISPLAY_BUILD
#define GDISPLAY_API __declspec(dllexport)
#else
#define GDISPLAY_API __declspec(dllimport)
#endif
#else
#define GDISPLAY_API
#endif
#endif

struct EdidResult
{
    bool ok = false;    // 是否成功取得並解碼
    QString sourceName; // 例如 \\.\DISPLAY1
    QByteArray raw;     // 原始 EDID bytes
    QString decoded;    // edid-decode 輸出的文字
    QString summary;    // 解析後的重點摘要（給 UI 用）
    QString error;      // 失敗時的錯誤訊息
};

struct EdidSummary
{
    // 文字版 summary（跟你現在 UI 用法一樣）
    QString basicText;     // 廠商、年分、螢幕大小、EDID version、Monitor name...
    QString highLevelText; // resolution / Hz / HDR / colorspace / NVIDIA / Deep Color...

    // === 解析後可直接程式使用的欄位（parseEdidDecodeText + summarizeEdid 整合到這裡） ===

    // 解析度相關
    QString nativeResolution; // 例如 "3840x2160 @ 60.00 Hz"
    int maxRefreshHz = 0;     // 例如 120 / 60

    // 色彩格式 / 位元深度
    QString colorFormats;     // 例如 "RGB 4:4:4, YCbCr 4:4:4, YCbCr 4:2:2"
    int bitsPerColor = 0;     // 每個 component 的 bit 數：8 / 10 / 12
    bool deepColor10 = false; // Supports 10-bits/component Deep Color
    bool deepColor12 = false; // Supports 12-bits/component Deep Color

    // 色彩空間 / HDR
    bool isSrgb = false;    // Default (sRGB) color space is primary
    bool hasBt2020 = false; // BT2020RGB / BT2020YCC
    bool hdr = false;       // 有 HDR Static Metadata / ST2084 / HLG

    // Vendor block
    bool hasNvidiaVsdb = false; // Vendor-Specific Data Block (NVIDIA)
};

/**
 * @brief 依照指定視窗目前所在的螢幕，取得 EDID 並呼叫 edid-decode 解碼。
 *
 * @param hwnd 目前主視窗的 HWND
 * @return EdidResult 結果（含 raw + decoded 或錯誤訊息）
 */
GDISPLAY_API EdidResult readEdidForWindow(HWND hwnd);

/**
 * @brief 依 raw EDID 與 edid-decode 輸出產生可讀的摘要。
 */
GDISPLAY_API EdidSummary summarizeEdid(const QByteArray &raw, const QString &decoded);
