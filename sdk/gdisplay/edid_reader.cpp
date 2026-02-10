#include "edid_reader.h"

#include <windows.h> //為了 OutputDebugString / registry
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <QCoreApplication>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryFile>
#include <QDir>

#include <QRegularExpression>
#include <algorithm>

#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

struct EdidDecodedInfo
{
    bool hasInfo = false;

    QString nativeResolution; // 例如 "3840x2160 @ 60.00 Hz"
    int maxRefreshHz = 0;     // 例如 120

    // 統一後的格式字串（最後由 hasRgb444 / hasYCbCrXXX 組出來）
    QString colorFormats; // 例如 "RGB 4:4:4, YCbCr 4:4:4, YCbCr 4:2:2"

    int bitsPerColor = 0; // 例如 8 / 10 / 12 (每個 component)

    // 個別格式 flag（parseEdidDecodeText 會先算這些，再組成 colorFormats）
    bool hasRgb444 = false;
    bool hasYCbCr444 = false;
    bool hasYCbCr422 = false;
    bool hasYCbCr420 = false;

    bool isSrgb = false;        // Default (sRGB) color space.
    bool hdr = false;           // 是否看到 HDR 相關標記
    bool hasNvidiaVsdb = false; // NVIDIA Vendor-Specific Data Block
    bool deepColor10 = false;   // 支援 10-bit Deep Color
    bool deepColor12 = false;   // 支援 12-bit Deep Color
    bool hasBt2020 = false;     // 有 BT2020RGB / BT2020YCC
};

// ---- 基本 EDID 解析：從 raw bytes 抽出一些好讀的資訊 ----
static QString summarizeEdidBasic(const QByteArray &raw)
{
    if (raw.size() < 128)
        return QObject::tr("EDID too short (%1 bytes)").arg(raw.size());

    const unsigned char *d = reinterpret_cast<const unsigned char *>(raw.constData());

    // Manufacturer ID (3 letters)
    quint16 man = (quint16(d[8]) << 8) | d[9];
    QChar m1 = QChar('A' + ((man >> 10) & 0x1F) - 1);
    QChar m2 = QChar('A' + ((man >> 5) & 0x1F) - 1);
    QChar m3 = QChar('A' + (man & 0x1F) - 1);
    QString manufacturer = QString("%1%2%3").arg(m1).arg(m2).arg(m3);

    // Product code (little endian)
    quint16 prodCode = quint16(d[10]) | (quint16(d[11]) << 8);

    // Serial number (little endian)
    quint32 serial = quint32(d[12]) |
                     (quint32(d[13]) << 8) |
                     (quint32(d[14]) << 16) |
                     (quint32(d[15]) << 24);

    // Week / year of manufacture
    int week = d[16];
    int year = 1990 + d[17];

    // EDID version / revision
    int ver = d[18];
    int rev = d[19];

    // Physical size (cm)
    int hcm = d[21];
    int vcm = d[22];

    // Extension block count
    int extCount = d[126];

    // 嘗試從 detailed descriptors 找螢幕名稱 (00 00 00 FC)
    QString monitorName;
    for (int desc = 0; desc < 4; ++desc)
    {
        int off = 54 + desc * 18; // 0x36
        if (off + 18 > raw.size())
            break;

        if (d[off + 0] == 0x00 && d[off + 1] == 0x00 &&
            d[off + 2] == 0x00 && d[off + 3] == 0xFC)
        {
            QByteArray name(reinterpret_cast<const char *>(d + off + 5), 13);
            monitorName = QString::fromLatin1(name).trimmed();
            break;
        }
    }

    // CEA-861 / CTA extension 的一些旗標（色彩格式、audio…）
    QString ctaInfo;
    if (extCount > 0 && raw.size() >= 128 * (extCount + 1))
    {
        const unsigned char *ext = d + 128;
        if (ext[0] == 0x02) // CEA-861 / CTA tag
        {
            int revExt = ext[1];
            unsigned char flags = ext[3];

            // Byte 3 (CTA-861 header) bits：
            // bit7: underscan
            // bit6: basic audio support
            // bit5: YCbCr 4:4:4 support
            // bit4: YCbCr 4:2:2 support
            // bit3-0: native detailed timing count
            bool underscan = (flags & 0x80) != 0;
            bool basicAudio = (flags & 0x40) != 0;
            bool y444 = (flags & 0x20) != 0;
            bool y422 = (flags & 0x10) != 0;
            int nativeCount = flags & 0x0F;

            ctaInfo = QObject::tr(
                          "CTA-861 rev %1, native DTDs: %2, "
                          "YCbCr 4:2:2 %3, 4:4:4 %4, basic audio %5, underscan %6")
                          .arg(revExt)
                          .arg(nativeCount)
                          .arg(y422 ? "yes" : "no")
                          .arg(y444 ? "yes" : "no")
                          .arg(basicAudio ? "yes" : "no")
                          .arg(underscan ? "yes" : "no");
        }
    }

    // 這裡開始用 HTML（方式 A）：欄位名稱加粗，整塊字體放大一點
    QStringList lines;
    lines << QObject::tr("<b>Manufacturer:</b> %1").arg(manufacturer);
    lines << QObject::tr("<b>Product code:</b> 0x%1")
                 .arg(prodCode, 4, 16, QLatin1Char('0'))
                 .toUpper();
    lines << QObject::tr("<b>Serial:</b> 0x%1")
                 .arg(qulonglong(serial), 8, 16, QLatin1Char('0'))
                 .toUpper();
    lines << QObject::tr("<b>Manufactured:</b> week %1, year %2")
                 .arg(week)
                 .arg(year);
    lines << QObject::tr("<b>EDID version:</b> %1.%2")
                 .arg(ver)
                 .arg(rev);

    if (hcm > 0 && vcm > 0)
        lines << QObject::tr("<b>Screen size:</b> %1 cm x %2 cm")
                     .arg(hcm)
                     .arg(vcm);

    lines << QObject::tr("<b>Extension blocks:</b> %1").arg(extCount);

    if (!monitorName.isEmpty())
        lines << QObject::tr("<b>Monitor name:</b> %1").arg(monitorName);

    // if (!ctaInfo.isEmpty())
    //     lines << QObject::tr("<b>CTA summary:</b> %1").arg(ctaInfo);

    // 方式 A：整塊包 div 放大字體 + 粗體
    QString html =
        "<div style='font-size:13pt; font-weight:bold;'>" + lines.join("<br>") + "</div>";

    return html;
}

static EdidDecodedInfo parseEdidDecodeText(const QString &decoded)
{
    EdidDecodedInfo info;
    if (decoded.isEmpty())
        return info;

    info.hasInfo = true;

    // 1) Native 解析度：找 DTD 1
    //    範例: "    DTD 1:  3440x1440   59.973 Hz ..."
    QRegularExpression reNative(
        R"(^\s*DTD\s*1:\s*(\d+)x(\d+)\s+([0-9.]+)\s*Hz)",
        QRegularExpression::MultilineOption);
    auto m = reNative.match(decoded);
    if (m.hasMatch())
    {
        int w = m.captured(1).toInt();
        int h = m.captured(2).toInt();
        double hz = m.captured(3).toDouble();
        info.nativeResolution = QString("%1x%2 @ %3 Hz")
                                    .arg(w)
                                    .arg(h)
                                    .arg(hz, 0, 'f', 2);
    }

    // 2) 最大更新率：從 Display Range Limits 這行抓 V 範圍上限
    //    範例: "Monitor ranges (Bare Limits): 30-120 Hz V, 73-180 kHz H, ..."
    QRegularExpression reRange(
        R"(Monitor ranges.*:\s*(\d+)-(\d+)\s*Hz\s*V)",
        QRegularExpression::MultilineOption);
    m = reRange.match(decoded);
    if (m.hasMatch())
    {
        info.maxRefreshHz = m.captured(2).toInt(); // 取上限，例如 120
    }

    // 3) Bits per primary color channel
    //    範例: "Bits per primary color channel: 8"
    QRegularExpression reBpc(
        R"(Bits per primary color channel:\s*(\d+))");
    m = reBpc.match(decoded);
    if (m.hasMatch())
    {
        info.bitsPerColor = m.captured(1).toInt();
    }

    // 4) 支援的色彩格式（統一整理：RGB / YCbCr 4:4:4 / 4:2:2 / 4:2:0）
    //    先從 "Supported color formats:" 這行解析，再結合其它 block 資訊
    QRegularExpression reFmt(
        R"(Supported color formats:\s*(.+))");
    m = reFmt.match(decoded);
    if (m.hasMatch())
    {
        const QString line = m.captured(1).trimmed();
        const QString lower = line.toLower();

        if (lower.contains("rgb 4:4:4"))
            info.hasRgb444 = true;
        if (lower.contains("ycbcr 4:4:4"))
            info.hasYCbCr444 = true;
        if (lower.contains("ycbcr 4:2:2"))
            info.hasYCbCr422 = true;
        if (lower.contains("ycbcr 4:2:0"))
            info.hasYCbCr420 = true;
    }

    // 4-1) HDMI 1.4/2.0/2.1 風格的 "Supports YCbCr ..." / 4:2:0 capability map
    if (decoded.contains("Supports YCbCr 4:4:4", Qt::CaseInsensitive))
        info.hasYCbCr444 = true;
    if (decoded.contains("Supports YCbCr 4:2:2", Qt::CaseInsensitive))
        info.hasYCbCr422 = true;
    if (decoded.contains("YCbCr 4:2:0 Capability Map Data Block", Qt::CaseInsensitive) ||
        decoded.contains("YCbCr 4:2:0 capability map", Qt::CaseInsensitive))
    {
        info.hasYCbCr420 = true;
    }

    // 4-2) Base block: "RGB color display" 幾乎代表 RGB 4:4:4 一定支援
    if (decoded.contains("RGB color display", Qt::CaseInsensitive))
        info.hasRgb444 = true;

    // 4-3) 最後組成統一的 colorFormats 字串（給 UI / log 用）
    {
        QStringList fmts;
        if (info.hasRgb444)
            fmts << "RGB 4:4:4";
        if (info.hasYCbCr444)
            fmts << "YCbCr 4:4:4";
        if (info.hasYCbCr422)
            fmts << "YCbCr 4:2:2";
        if (info.hasYCbCr420)
            fmts << "YCbCr 4:2:0";

        info.colorFormats = fmts.join(", ");
    }

    // 5) sRGB / BT.709
    if (decoded.contains("Default (sRGB) color space is primary color space"))
        info.isSrgb = true;

    // 6) HDR 判斷（看有沒有 HDR / EOTF / ST2084 / HLG 字樣）
    if (decoded.contains("HDR Static Metadata", Qt::CaseInsensitive) ||
        decoded.contains("HDR static metadata", Qt::CaseInsensitive) ||
        decoded.contains("Electro optical transfer function", Qt::CaseInsensitive) ||
        decoded.contains("SMPTE ST2084", Qt::CaseInsensitive) ||
        decoded.contains("SMPTE ST 2084", Qt::CaseInsensitive) ||
        decoded.contains("HLG", Qt::CaseInsensitive))
    {
        info.hdr = true;
    }

    // 7) NVIDIA Vendor-Specific Data Block（大多數是 G-SYNC / G-SYNC Compatible 類型）
    if (decoded.contains("Vendor-Specific Data Block (NVIDIA)", Qt::CaseInsensitive))
        info.hasNvidiaVsdb = true;

    // 8) Deep Color 支援
    //    兩種常見寫法：
    //      - "DC_30bit" / "DC_36bit" (HDMI VSDB)
    //      - "Supports 10-bits/component Deep Color ..." (HDMI Forum VSDB)
    if (decoded.contains("DC_30bit", Qt::CaseInsensitive) ||
        decoded.contains("Supports 10-bits/component Deep Color", Qt::CaseInsensitive))
    {
        info.deepColor10 = true;
    }
    if (decoded.contains("DC_36bit", Qt::CaseInsensitive) ||
        decoded.contains("Supports 12-bits/component Deep Color", Qt::CaseInsensitive))
    {
        info.deepColor12 = true;
    }

    // 9) BT.2020 色彩空間（HDR 廣色域）
    if (decoded.contains("BT2020RGB", Qt::CaseInsensitive) ||
        decoded.contains("BT2020YCC", Qt::CaseInsensitive))
    {
        info.hasBt2020 = true;
    }

    // 10) 如果 base block 沒寫 "Bits per primary color channel"，用 Deep Color 粗略推一個
    if (info.bitsPerColor == 0)
    {
        if (info.deepColor12)
            info.bitsPerColor = 12;
        else if (info.deepColor10)
            info.bitsPerColor = 10;
        // 否則就留 0 代表「未知」，顯示端再決定要不要當作 8-bit
    }

    return info;
}

GDISPLAY_API EdidSummary summarizeEdid(const QByteArray &raw, const QString &decoded)
{
    EdidSummary out;

    // basicText：用 raw 解析 (廠商 / 年份 / 尺寸 / 螢幕名稱 / CTA flag…)
    out.basicText = summarizeEdidBasic(raw);

    // highLevelText：用 edid-decode 的文字解析 (native res / Hz / HDR / colorspace…)
    EdidDecodedInfo dinfo = parseEdidDecodeText(decoded);

    if (dinfo.hasInfo)
    {
        out.nativeResolution = dinfo.nativeResolution;
        out.maxRefreshHz = dinfo.maxRefreshHz;
        out.colorFormats = dinfo.colorFormats;
        out.bitsPerColor = dinfo.bitsPerColor;
        out.isSrgb = dinfo.isSrgb;
        out.hdr = dinfo.hdr;
        out.hasNvidiaVsdb = dinfo.hasNvidiaVsdb;
        out.deepColor10 = dinfo.deepColor10;
        out.deepColor12 = dinfo.deepColor12;
        out.hasBt2020 = dinfo.hasBt2020;

        QStringList hl;

        // 標題整行加粗
        hl << "<b>=== High-level summary (from edid-decode) ===</b>";

        // ---- Timing / 基本影像資訊 ----
        if (!dinfo.nativeResolution.isEmpty())
        {
            hl << QStringLiteral("<b>Native resolution:</b> %1")
                      .arg(dinfo.nativeResolution);
        }

        if (dinfo.maxRefreshHz > 0)
        {
            hl << QStringLiteral("<b>Max refresh rate:</b> %1 Hz")
                      .arg(dinfo.maxRefreshHz);
        }

        // ---- Color / HDR section（OBS / NVIDIA 風格） ----

        // 1) Pixel format（類似 OBS / NVIDIA 的「Color format」）
        QString pixelFormats = dinfo.colorFormats;
        if (pixelFormats.isEmpty())
            pixelFormats = "Unknown";

        hl << QStringLiteral("<b>Pixel formats supported:</b> %1")
                  .arg(pixelFormats);

        // 2) Bit depth（類似「Output color depth」）
        QString depthText;
        if (dinfo.bitsPerColor > 0)
        {
            depthText = QStringLiteral("%1-bit per channel")
                            .arg(dinfo.bitsPerColor);
        }
        else
        {
            QStringList dc;
            if (dinfo.deepColor10)
                dc << "10-bit";
            if (dinfo.deepColor12)
                dc << "12-bit";
            if (!dc.isEmpty())
                depthText = QStringLiteral("Deep Color (%1)").arg(dc.join(", "));
        }
        if (depthText.isEmpty())
            depthText = "Unknown";

        hl << QStringLiteral("<b>Color depth:</b> %1").arg(depthText);

        // 3) 面板色域 / Colorspace / Gamut（類似 NVIDIA「Color space」)
        QString gamutText;
        if (dinfo.isSrgb && !dinfo.hasBt2020)
        {
            gamutText = "sRGB / BT.709";
        }
        else if (!dinfo.isSrgb && dinfo.hasBt2020)
        {
            gamutText = "Wide gamut (BT.2020)";
        }
        else if (dinfo.isSrgb && dinfo.hasBt2020)
        {
            gamutText = "sRGB + BT.2020 (mixed)";
        }
        else
        {
            gamutText = "Non-sRGB / unknown gamut";
        }
        hl << QStringLiteral("<b>Panel color gamut:</b> %1").arg(gamutText);

        // 4) Colorimetry（補充：從 EDID 看到的色彩空間宣告）
        if (dinfo.hasBt2020)
            hl << "<b>Colorimetry (EDID):</b> BT.2020 (wide gamut)";
        else if (dinfo.isSrgb)
            hl << "<b>Colorimetry (EDID):</b> sRGB / BT.709";
        else
            hl << "<b>Colorimetry (EDID):</b> Unknown";

        // 5) HDR（類似 OBS「Color space: 709 / 2100 PQ」概念）
        QString hdrText = dinfo.hdr ? "Yes" : "No";
        hl << QStringLiteral("<b>HDR support:</b> %1").arg(hdrText);

        // 6) Deep Color 支援摘要（獨立列出，方便 debug）
        QStringList dcFlags;
        if (dinfo.deepColor10)
            dcFlags << "10-bit";
        if (dinfo.deepColor12)
            dcFlags << "12-bit";

        QString dcLine;
        if (dcFlags.isEmpty())
            dcLine = "None / not declared";
        else
            dcLine = dcFlags.join(", ");

        hl << QStringLiteral("<b>Deep Color support:</b> %1").arg(dcLine);

        // 7) Vendor / GPU 特定資訊（例如 NVIDIA VSDB）
        hl << QStringLiteral("<b>NVIDIA vendor block:</b> %1")
                  .arg(dinfo.hasNvidiaVsdb ? "present" : "not present");

        // 用 <br> 當換行，給 QTextEdit::setHtml() 使用
        // out.highLevelText = hl.join("<br>");
        out.highLevelText =
            "<div style='font-size:14pt; font-weight:bold;'>" + hl.join("<br>") + "</div>";
    }

    return out;
}

// ---------- 小工具：debug 輸出 ----------

static void dbgA(const std::string &s)
{
    std::string msg = "[EDID] " + s + "\n";
    OutputDebugStringA(msg.c_str());
}

static void dbgW(const std::wstring &s)
{
    std::wstring msg = L"[EDID] ";
    msg += s;
    msg += L"\n";
    OutputDebugStringW(msg.c_str());
}

static QString stripEdidDecodeHexHeader(const QString &decoded)
{
    QString s = decoded;

    // 找到 "edid-decode (hex):" 這一行
    int idx = s.indexOf("edid-decode (hex):");
    if (idx < 0)
        return s; // 沒找到就原樣回傳

    // 從這一行開始找第一個空白行（兩個換行）
    idx = s.indexOf("\n\n", idx);
    if (idx < 0)
        return s;

    // 只保留空白行之後的內容（也就是 hex 之後真正的解析文字）
    return s.mid(idx + 2);
}

// 取得對應 HMONITOR 的 DXGI Output 的 DeviceName（\\.\DISPLAY1）
static bool getDxgiDeviceNameForMonitor(HMONITOR hmon, std::wstring &deviceName)
{
    deviceName.clear();

    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    {
        dbgA("CreateDXGIFactory1 failed");
        return false;
    }

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

            if (desc.Monitor != hmon)
                continue;

            // 找到對應螢幕
            deviceName = desc.DeviceName; // 例如 L"\\\\.\\DISPLAY1"
            dbgW(L"DXGI DeviceName = " + deviceName);
            return true;
        }
    }

    dbgA("getDxgiDeviceNameForMonitor: no output matched this HMONITOR");
    return false;
}

// 依據 DXGI DeviceName (\\.\DISPLAY1) 找到對應「螢幕」的 DeviceID
static bool findDisplayDeviceIdForDeviceName(const std::wstring &deviceName, std::wstring &deviceId)
{
    deviceId.clear();

    DISPLAY_DEVICEW adapter;
    ZeroMemory(&adapter, sizeof(adapter));
    adapter.cb = sizeof(adapter);

    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &adapter, 0); ++i)
    {
        std::wstring adName = adapter.DeviceName; // e.g. \\.\DISPLAY1, \\.\DISPLAY2
        std::wstring adId = adapter.DeviceID;
        dbgW(L"EnumDisplayDevices adapter: Name=" + adName + L"  ID=" + adId);

        if (deviceName != adapter.DeviceName)
        {
            ZeroMemory(&adapter, sizeof(adapter));
            adapter.cb = sizeof(adapter);
            continue;
        }

        // 找到對應顯示卡 → 再列舉底下的螢幕
        DISPLAY_DEVICEW monitor;
        ZeroMemory(&monitor, sizeof(monitor));
        monitor.cb = sizeof(monitor);

        for (DWORD j = 0; EnumDisplayDevicesW(adapter.DeviceName, j, &monitor, 0); ++j)
        {
            std::wstring monName = monitor.DeviceName;
            std::wstring monId = monitor.DeviceID;
            wchar_t buf[256];
            swprintf(buf, 256, L"  Monitor %lu: Name=%s  ID=%s  Flags=0x%08X",
                     j, monName.c_str(), monId.c_str(), monitor.StateFlags);
            dbgW(buf);

            if (!(monitor.StateFlags & DISPLAY_DEVICE_ACTIVE))
            {
                ZeroMemory(&monitor, sizeof(monitor));
                monitor.cb = sizeof(monitor);
                continue;
            }

            deviceId = monitor.DeviceID; // 例如 L"DISPLAY\\ACR0617\\5&272c5422&0&UID4352"
            dbgW(L"Chosen Monitor DeviceID = " + deviceId);
            return true;
        }

        ZeroMemory(&adapter, sizeof(adapter));
        adapter.cb = sizeof(adapter);
    }

    dbgA("findDisplayDeviceIdForDeviceName: no ACTIVE monitor found");
    return false;
}

// 從 DeviceID 取出 hwid (= PNPID)，然後到 Enum\DISPLAY / Enum\MONITOR 底下掃子 key 找 EDID
static bool readEdidFromRegistryUsingDeviceId(const std::wstring &deviceId, std::vector<uint8_t> &edid)
{
    edid.clear();

    dbgW(L"readEdidFromRegistryUsingDeviceId: DeviceID = " + deviceId);

    // 先從 DeviceID 裡抓出 hwid：DISPLAY\GSM5CBB\xxx or MONITOR\GSM5CBB\xxx
    const std::wstring prefixes[] = {L"DISPLAY\\", L"MONITOR\\"};

    std::wstring after;
    for (const auto &p : prefixes)
    {
        auto pos = deviceId.find(p);
        if (pos != std::wstring::npos)
        {
            after = deviceId.substr(pos + p.size());
            break;
        }
    }
    if (after.empty())
    {
        dbgA("DeviceID does not contain DISPLAY\\ or MONITOR\\ prefix");
        return false;
    }

    auto slashPos = after.find(L'\\');
    if (slashPos == std::wstring::npos)
    {
        dbgA("DeviceID format unexpected (no second backslash after prefix)");
        return false;
    }

    std::wstring hwid = after.substr(0, slashPos); // 例如 GSM5CBB
    dbgW(L"Parsed hwid = " + hwid);

    // 小工具：在某個 Enum 根 (DISPLAY 或 MONITOR) 底下掃所有 instance
    auto tryFromEnumRoot = [&](const std::wstring &enumRoot) -> bool
    {
        wchar_t hwPath[512];
        swprintf(hwPath, 512, L"%s\\%s", enumRoot.c_str(), hwid.c_str());
        dbgW(L"tryFromEnumRoot: hwPath = " + std::wstring(hwPath));

        HKEY hHw = nullptr;
        LONG r = RegOpenKeyExW(HKEY_LOCAL_MACHINE, hwPath, 0, KEY_READ, &hHw);
        if (r != ERROR_SUCCESS)
        {
            wchar_t buf[256];
            swprintf(buf, 256, L"RegOpenKeyExW(hwid) failed, error=%ld", r);
            dbgW(buf);
            return false;
        }

        DWORD index = 0;
        bool ok = false;
        while (true)
        {
            wchar_t subName[256];
            DWORD subLen = 256;
            FILETIME ft{};
            r = RegEnumKeyExW(hHw, index, subName, &subLen, nullptr, nullptr, nullptr, &ft);
            if (r == ERROR_NO_MORE_ITEMS)
                break;
            if (r != ERROR_SUCCESS)
            {
                wchar_t buf[256];
                swprintf(buf, 256, L"RegEnumKeyExW failed at index=%lu, error=%ld", index, r);
                dbgW(buf);
                ++index;
                continue;
            }

            wchar_t devParamPath[512];
            swprintf(devParamPath, 512, L"%s\\%s\\%s\\Device Parameters",
                     enumRoot.c_str(), hwid.c_str(), subName);
            dbgW(L"  Trying instance path: " + std::wstring(devParamPath));

            HKEY hKey = nullptr;
            r = RegOpenKeyExW(HKEY_LOCAL_MACHINE, devParamPath, 0, KEY_READ, &hKey);
            if (r != ERROR_SUCCESS)
            {
                ++index;
                continue;
            }

            DWORD type = 0;
            DWORD size = 0;
            r = RegQueryValueExW(hKey, L"EDID", nullptr, &type, nullptr, &size);
            if (r == ERROR_SUCCESS && type == REG_BINARY && size > 0)
            {
                edid.resize(size);
                r = RegQueryValueExW(hKey, L"EDID", nullptr, nullptr, edid.data(), &size);
                RegCloseKey(hKey);

                if (r == ERROR_SUCCESS)
                {
                    wchar_t buf[256];
                    swprintf(buf, 256, L"  EDID found at instance %s, size=%u bytes", subName, size);
                    dbgW(buf);

                    ok = true;
                    break;
                }
                else
                {
                    edid.clear();
                }
            }
            else
            {
                RegCloseKey(hKey);
            }

            ++index;
        }

        RegCloseKey(hHw);
        return ok;
    };

    // 先試 Enum\DISPLAY\GSM5CBB\*
    if (tryFromEnumRoot(L"SYSTEM\\CurrentControlSet\\Enum\\DISPLAY"))
        return true;

    // 再試 Enum\MONITOR\GSM5CBB\* 當作 fallback
    if (tryFromEnumRoot(L"SYSTEM\\CurrentControlSet\\Enum\\MONITOR"))
        return true;

    dbgA("No EDID found under Enum\\DISPLAY or Enum\\MONITOR for this hwid");
    return false;
}

// 呼叫外部 edid-decode.exe，把 EDID 寫到暫存檔後用檔案路徑呼叫，避免 stdin 在 Windows 被截斷
static QString runEdidDecode(const QByteArray &raw, QString &errorOut)
{
    errorOut.clear();

    // 預設假設 edid-decode.exe 跟 qt6_viewer.exe 在同一個資料夾
    QString exePath = QCoreApplication::applicationDirPath() + "/edid-decode.exe";

    if (!QFileInfo::exists(exePath))
    {
        errorOut = QObject::tr("edid-decode.exe not found in application directory.\n"
                               "Please copy edid-decode.exe next to qt6_viewer.exe.");
        dbgA("edid-decode.exe not found next to qt6_viewer.exe");
        return {};
    }

    dbgA(std::string("Running edid-decode.exe at: ") + exePath.toLocal8Bit().constData());

    // 建一個暫存檔，把 EDID 寫進去
    QTemporaryFile tempFile(QDir::tempPath() + "/edidXXXXXX.bin");
    tempFile.setAutoRemove(true);

    if (!tempFile.open())
    {
        errorOut = QObject::tr("Failed to create temporary file for EDID");
        dbgA("Failed to create temporary file for EDID");
        return {};
    }

    if (tempFile.write(raw) != raw.size())
    {
        errorOut = QObject::tr("Failed to write EDID to temporary file");
        dbgA("Failed to write EDID to temporary file");
        return {};
    }

    tempFile.flush();
    QString tempPath = tempFile.fileName();
    dbgA(std::string("Temporary EDID file: ") + tempPath.toLocal8Bit().constData());

    // 用檔案路徑呼叫：edid-decode.exe <tempPath>
    QProcess proc;
    proc.setProgram(exePath);
    proc.setArguments({tempPath});

    proc.start();
    if (!proc.waitForStarted(2000))
    {
        errorOut = QObject::tr("Failed to start edid-decode.exe");
        dbgA("Failed to start edid-decode.exe");
        return {};
    }

    proc.waitForFinished(5000);

    QString out = QString::fromLocal8Bit(proc.readAllStandardOutput());
    QString err = QString::fromLocal8Bit(proc.readAllStandardError());

    if (!err.isEmpty())
    {
        dbgA(std::string("edid-decode stderr: ") + err.toLocal8Bit().constData());
        out += QObject::tr("\n[stderr]\n") + err;
    }

    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0)
    {
        if (errorOut.isEmpty())
            errorOut = QObject::tr("edid-decode exited with code %1").arg(proc.exitCode());

        char buf[128];
        sprintf_s(buf, "[EDID] edid-decode exitCode=%d", proc.exitCode());
        dbgA(buf);
    }

    out = stripEdidDecodeHexHeader(out);
    return out;
}

GDISPLAY_API EdidResult readEdidForWindow(HWND hwnd)
{
    EdidResult res;

    if (!hwnd)
    {
        res.error = QObject::tr("Invalid window handle");
        dbgA("readEdidForWindow: hwnd is null");
        return res;
    }

    HMONITOR hmon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!hmon)
    {
        res.error = QObject::tr("Failed to get HMONITOR from window");
        dbgA("MonitorFromWindow failed");
        return res;
    }

    dbgA("readEdidForWindow: got HMONITOR, start DXGI lookup");

    std::wstring devName;
    if (!getDxgiDeviceNameForMonitor(hmon, devName))
    {
        res.error = QObject::tr("Failed to get DXGI output for monitor");
        return res;
    }

    res.sourceName = QString::fromWCharArray(devName.c_str());

    std::wstring devId;
    if (!findDisplayDeviceIdForDeviceName(devName, devId))
    {
        res.error = QObject::tr("Failed to find DISPLAY_DEVICE for %1").arg(res.sourceName);
        return res;
    }

    std::vector<uint8_t> edidVec;
    if (!readEdidFromRegistryUsingDeviceId(devId, edidVec))
    {
        res.error = QObject::tr("Failed to read EDID from registry");
        return res;
    }

    res.raw = QByteArray(reinterpret_cast<const char *>(edidVec.data()),
                         static_cast<int>(edidVec.size()));

    QString decodeErr;
    res.decoded = runEdidDecode(res.raw, decodeErr);

    if (!decodeErr.isEmpty() && res.decoded.isEmpty())
    {
        res.error = decodeErr;
        res.ok = false;
        return res;
    }

    res.ok = true;
    return res;
}
