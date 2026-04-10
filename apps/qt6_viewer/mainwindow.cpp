#include "mainwindow.h"
#include "gcapture.h"

#include "./ui_mainwindow.h"
#include <QVBoxLayout>
#include <QPushButton>
#include <QComboBox>
#include <QMessageBox>
#include <QPixmap>
#include <QWindow>
#include <QScreen>
#include <QGuiApplication>
#include <QDir>
#include "display_info.h"
#include <QDialog>
#include <QPlainTextEdit>
#include <QLabel>
#include <QDesktopServices>
#include <QUrl>
#include <vector>
#include "edid_reader.h"
#include "tiffanalysisdialog.h"
#include "tiff_analyzer.h"
#include <QFileDialog>
#include <QFileInfo>
#ifdef _WIN32
#include <dxgi1_2.h>
#include <wrl.h>
using Microsoft::WRL::ComPtr;
#endif

static MainWindow *g_mainWindow = nullptr;

namespace
{
    static inline uint8_t clampByteLocal(int v)
    {
        if (v < 0)
            return 0;
        if (v > 255)
            return 255;
        return static_cast<uint8_t>(v);
    }

    static inline void yuvToRgbLocal(int y, int u, int v, uint8_t &b, uint8_t &g, uint8_t &r)
    {
        const int c = y - 16;
        const int d = u - 128;
        const int e = v - 128;
        const int rr = (298 * c + 409 * e + 128) >> 8;
        const int gg = (298 * c - 100 * d - 208 * e + 128) >> 8;
        const int bb = (298 * c + 516 * d + 128) >> 8;
        r = clampByteLocal(rr);
        g = clampByteLocal(gg);
        b = clampByteLocal(bb);
    }

    static const char *packetFmtName(int fmt)
    {
        switch (fmt)
        {
        case GCAP_FMT_NV12:
            return "NV12";
        case GCAP_FMT_P010:
            return "P010";
        case GCAP_FMT_YUY2:
            return "YUY2";
        case GCAP_FMT_Y210:
            return "Y210";
        case GCAP_FMT_ARGB:
            return "ARGB";
        default:
            return "UNKNOWN";
        }
    }

    static QString formatVideoCapDisplay(const gcap_video_cap_t &cap)
    {
        const double fps = (cap.fps_den > 0) ? (double(cap.fps_num) / double(cap.fps_den)) : 0.0;
        QString text = QStringLiteral("%1 %2x%3")
                           .arg(QString::fromLatin1(packetFmtName(cap.pixfmt)))
                           .arg(cap.width)
                           .arg(cap.height);
        if (fps > 0.0)
            text += QStringLiteral(" %1 fps").arg(fps, 0, 'f', 2);
        if (cap.bit_depth > 0)
            text += QStringLiteral(" (%1-bit)").arg(cap.bit_depth);
        return text;
    }

    static QString formatPropertyPageDisplay(const gcap_property_page_t &page)
    {
        return QStringLiteral("%1 — %2")
            .arg(QString::fromUtf8(page.page_name))
            .arg(page.capture_pin ? QStringLiteral("Capture Pin") : QStringLiteral("Filter"));
    }

    static void fillDeviceCapabilitiesFromSdk(int deviceIndex, CaptureDeviceInfo &info)
    {
        info.supportedFormats.clear();
        info.propertyPages.clear();

        const int capCount = gcap_enum_video_caps(deviceIndex, nullptr, 0);
        if (capCount > 0)
        {
            std::vector<gcap_video_cap_t> caps(static_cast<size_t>(capCount));
            const int written = gcap_enum_video_caps(deviceIndex, caps.data(), static_cast<int>(caps.size()));
            for (int i = 0; i < written; ++i)
                info.supportedFormats << formatVideoCapDisplay(caps[static_cast<size_t>(i)]);
        }

        const int pageCount = gcap_enum_property_pages(deviceIndex, nullptr, 0);
        if (pageCount > 0)
        {
            std::vector<gcap_property_page_t> pages(static_cast<size_t>(pageCount));
            const int written = gcap_enum_property_pages(deviceIndex, pages.data(), static_cast<int>(pages.size()));
            for (int i = 0; i < written; ++i)
                info.propertyPages << formatPropertyPageDisplay(pages[static_cast<size_t>(i)]);
        }
    }


    static QImage framePacketToQImage(const gcap_frame_packet_t &pkt)
    {
        if (pkt.width <= 0 || pkt.height <= 0 || pkt.plane_count <= 0 || !pkt.data[0])
            return {};

        if (pkt.format == GCAP_FMT_ARGB)
        {
            QImage img(reinterpret_cast<const uchar *>(pkt.data[0]), pkt.width, pkt.height, pkt.stride[0], QImage::Format_ARGB32);
            return img.copy();
        }

        QImage img(pkt.width, pkt.height, QImage::Format_ARGB32);
        if (img.isNull())
            return {};

        if (pkt.format == GCAP_FMT_NV12 && pkt.plane_count >= 2 && pkt.data[1])
        {
            const uint8_t *yPlane = reinterpret_cast<const uint8_t *>(pkt.data[0]);
            const uint8_t *uvPlane = reinterpret_cast<const uint8_t *>(pkt.data[1]);
            const int yStride = pkt.stride[0] > 0 ? pkt.stride[0] : pkt.width;
            const int uvStride = pkt.stride[1] > 0 ? pkt.stride[1] : pkt.width;
            for (int y = 0; y < pkt.height; ++y)
            {
                const uint8_t *yRow = yPlane + static_cast<size_t>(y) * yStride;
                const uint8_t *uvRow = uvPlane + static_cast<size_t>(y / 2) * uvStride;
                QRgb *dst = reinterpret_cast<QRgb *>(img.scanLine(y));
                for (int x = 0; x < pkt.width; ++x)
                {
                    const int Y = yRow[x];
                    const int U = uvRow[(x & ~1) + 0];
                    const int V = uvRow[(x & ~1) + 1];
                    uint8_t b = 0, g = 0, r = 0;
                    yuvToRgbLocal(Y, U, V, b, g, r);
                    dst[x] = qRgba(r, g, b, 255);
                }
            }
            return img;
        }

        if (pkt.format == GCAP_FMT_YUY2)
        {
            const uint8_t *src = reinterpret_cast<const uint8_t *>(pkt.data[0]);
            const int srcStride = pkt.stride[0] > 0 ? pkt.stride[0] : (pkt.width * 2);
            for (int y = 0; y < pkt.height; ++y)
            {
                const uint8_t *srcRow = src + static_cast<size_t>(y) * srcStride;
                QRgb *dst = reinterpret_cast<QRgb *>(img.scanLine(y));
                for (int x = 0; x < pkt.width; x += 2)
                {
                    const uint8_t y0 = srcRow[x * 2 + 0];
                    const uint8_t u = srcRow[x * 2 + 1];
                    const uint8_t y1 = srcRow[x * 2 + 2];
                    const uint8_t v = srcRow[x * 2 + 3];
                    uint8_t b = 0, g = 0, r = 0;
                    yuvToRgbLocal(y0, u, v, b, g, r);
                    dst[x] = qRgba(r, g, b, 255);
                    if (x + 1 < pkt.width)
                    {
                        yuvToRgbLocal(y1, u, v, b, g, r);
                        dst[x + 1] = qRgba(r, g, b, 255);
                    }
                }
            }
            return img;
        }

        if (pkt.format == GCAP_FMT_Y210)
        {
            const uint16_t *src = reinterpret_cast<const uint16_t *>(pkt.data[0]);
            const int srcStride = pkt.stride[0] > 0 ? pkt.stride[0] : (pkt.width * 4);
            for (int y = 0; y < pkt.height; ++y)
            {
                const uint16_t *srcRow = reinterpret_cast<const uint16_t *>(reinterpret_cast<const uint8_t *>(src) + static_cast<size_t>(y) * static_cast<size_t>(srcStride));
                QRgb *dst = reinterpret_cast<QRgb *>(img.scanLine(y));
                for (int x = 0; x < pkt.width; x += 2)
                {
                    const int base = x * 2;
                    const int Y0 = ((int)(srcRow[base + 0] & 1023u) * 255 + 511) / 1023;
                    const int U = ((int)(srcRow[base + 1] & 1023u) * 255 + 511) / 1023;
                    const int Y1 = (x + 1 < pkt.width) ? (((int)(srcRow[base + 2] & 1023u) * 255 + 511) / 1023) : Y0;
                    const int V = (x + 1 < pkt.width) ? (((int)(srcRow[base + 3] & 1023u) * 255 + 511) / 1023) : U;
                    uint8_t b = 0, g = 0, r = 0;
                    yuvToRgbLocal(Y0, U, V, b, g, r);
                    dst[x] = qRgba(r, g, b, 255);
                    if (x + 1 < pkt.width)
                    {
                        yuvToRgbLocal(Y1, U, V, b, g, r);
                        dst[x + 1] = qRgba(r, g, b, 255);
                    }
                }
            }
            return img;
        }

        return {};
    }
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    setupRuntimeStatusTimer();
    setupDebugDock();
    setupProcAmpAction();
    setupBackendControls();
    initializeDeviceList();
    initializeGpuList();
    setupConnections();

    g_mainWindow = this;
    logStartupInfo();
}

MainWindow::~MainWindow()
{
    if (g_mainWindow == this)
        g_mainWindow = nullptr;
    delete tiffAnalysisDlg_;
    delete previewWindow_;
    delete ui;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (previewWindow_)
    {
        previewWindow_->close();
    }

    QMainWindow::closeEvent(event);
}

void MainWindow::s_vcb(const gcap_frame_t *f, void *u)
{
    auto *self = static_cast<MainWindow *>(u);
    if (!self || !f || !f->data[0] || f->width <= 0 || f->height <= 0)
        return;
    if (self->usePacketCallback_)
        return;
    self->updateFrameSourceState(f->pts_ns, f->width, f->height, self->lastVideoCallbackPtsNs_);

    // 這裡 f 是 BGRA 一張圖，直接包成 QImage
    QImage img((const uchar *)f->data[0], f->width, f->height, f->stride[0], QImage::Format_ARGB32);
    self->dispatchFrameImage(img);
}

void MainWindow::s_pcb(const gcap_frame_packet_t *pkt, void *u)
{
    auto *self = static_cast<MainWindow *>(u);
    if (!self || !pkt)
        return;

    self->updateFrameSourceState(pkt->pts_ns, pkt->width, pkt->height, self->lastPacketCallbackPtsNs_);
    self->logFramePacketIfNeeded(*pkt);

    if (!self->usePacketCallback_)
        return;

    QImage img = framePacketToQImage(*pkt);
    if (!img.isNull())
        self->dispatchFrameImage(img);
}

void MainWindow::s_ecb(gcap_status_t c, const char *m, void *u)
{
    Q_UNUSED(u);

    const QByteArray ba = m ? QByteArray(m) : QByteArray();

    // ecb_ 訊息可能是 UTF-8，也可能是 Windows ACP（例如含 hr_msg 的中文）
    QString msg = QString::fromUtf8(ba);
    if (msg.contains(QChar(0xFFFD))) // UTF-8 解碼失敗常會出現替代字元
        msg = QString::fromLocal8Bit(ba);

    // === 先進集中 log（UI + 檔案）===
    MainWindow::postLog(
        QStringLiteral("[gcapture][%1] %2").arg(int(c)).arg(msg),
        c != GCAP_OK);

    // === 只有錯誤才跳視窗 ===
    if (c != GCAP_OK && g_mainWindow)
    {
        QMetaObject::invokeMethod(
            g_mainWindow,
            [msg]
            {
                QMessageBox::warning(nullptr, "gcapture", msg);
            },
            Qt::QueuedConnection);
    }
}

void MainWindow::updateRuntimeStatusUi()
{
    if (!ui->statusbar)
        return;

    if (!h_)
    {
        const QString sb = QStringLiteral("Idle");
        if (lastRuntimeStatusText_ != sb)
        {
            ui->statusbar->showMessage(sb);
            lastRuntimeStatusText_ = sb;
        }
        return;
    }

    gcap_runtime_info_t rt{};
    if (gcap_get_runtime_info(h_, &rt) != GCAP_OK)
        return;

    const double negotiatedFps = (rt.negotiated.fps_den > 0) ? (double(rt.negotiated.fps_num) / double(rt.negotiated.fps_den)) : 0.0;
    const double runtimeFps = (rt.runtime_fps > 0.0) ? rt.runtime_fps : avgFps_;
    const QString backend = QString::fromUtf8(rt.backend_name);
    const QString source = QString::fromUtf8(rt.frame_source);

    QString negotiatedFmt;
    if (rt.negotiated_desc[0])
        negotiatedFmt = QString::fromUtf8(rt.negotiated_desc);
    else if (rt.source_format[0])
        negotiatedFmt = QString::fromUtf8(rt.source_format);
    else
        negotiatedFmt = QString::fromUtf8(packetFmtName(rt.negotiated.pixfmt));
    const QString renderFmt = QString::fromUtf8(rt.render_format);
    const auto statusBlock = [](const char *label, const gcap_signal_status_t &s, double fps, const QString &fmt)
    {
        if (s.width <= 0 || s.height <= 0)
            return QStringLiteral("%1 --").arg(QString::fromUtf8(label));
        return QStringLiteral("%1 %2x%3 %4fps %5")
            .arg(QString::fromUtf8(label))
            .arg(s.width)
            .arg(s.height)
            .arg(QString::number(fps, 'f', 2))
            .arg(fmt);
    };
    const QString sb = QStringLiteral("Backend: %1 | Source: %2 | %3 | AppInternal %4 | Runtime %5fps")
                           .arg(backend)
                           .arg(source)
                           .arg(statusBlock("BackendFmt", rt.negotiated, negotiatedFps, negotiatedFmt))
                           .arg(renderFmt.isEmpty() ? QStringLiteral("--") : renderFmt)
                           .arg(runtimeFps > 0.0 ? QString::number(runtimeFps, 'f', 2) : QStringLiteral("--"));
    if (lastRuntimeStatusText_ != sb)
    {
        ui->statusbar->showMessage(sb);
        lastRuntimeStatusText_ = sb;
    }
}

void MainWindow::onToggleDebugLog(bool checked)
{
    if (!debugDock_)
        return;

    debugDock_->setVisible(checked);
}

void MainWindow::onShowEdid()
{
#ifdef _WIN32
    if (!windowHandle())
    {
        QMessageBox::warning(this, tr("EDID"), tr("No window handle available"));
        return;
    }

    HWND hwnd = reinterpret_cast<HWND>(windowHandle()->winId());
    EdidResult res = readEdidForWindow(hwnd);

    if (!res.ok)
    {
        QMessageBox::warning(this, tr("EDID"),
                             tr("Failed to read/parse EDID:\n%1").arg(res.error));
        return;
    }

    // 把 raw EDID 轉成 hex dump
    QString hex;
    const QByteArray &raw = res.raw;
    for (int i = 0; i < raw.size(); ++i)
    {
        if (i % 16 == 0)
            hex += QString("\n%1: ").arg(i, 4, 16, QLatin1Char('0')).toUpper();

        hex += QString("%1 ").arg(static_cast<quint8>(raw[i]), 2, 16, QLatin1Char('0')).toUpper();
    }

    // 解析 summary（highLevelText = HTML, basicText = 純文字）
    EdidSummary sum = summarizeEdid(raw, res.decoded);

    // 組成整體 HTML
    QString html;

    // 1) 高階摘要（已經是 HTML + <br>）
    if (!sum.highLevelText.isEmpty())
    {
        html += sum.highLevelText;
        html += "<br><br>";
    }

    // 2) basicText：現在 summarizeEdidBasic 已經回傳 HTML
    if (!sum.basicText.isEmpty())
    {
        html += sum.basicText;
        html += "<br><br>";
    }

    // 3) Source / Size 資訊
    html += tr("Source: %1<br>Size: %2 bytes<br>")
                .arg(res.sourceName.toHtmlEscaped())
                .arg(raw.size());
    html += "<br><br>";

    // 4) Raw EDID（hex dump）
    html += "<b>Raw EDID</b>";
    html += hex.toHtmlEscaped().replace("\n", "<br>");
    html += "<br><br>";

    // 5) edid-decode 原始輸出
    html += "<b>Decoded by edid-decode</b><br>";
    if (res.decoded.isEmpty())
    {
        html += tr("(No output from edid-decode)");
    }
    else
    {
        html += res.decoded.toHtmlEscaped().replace("\n", "<br>");
    }

    // 顯示對話框（改用 QTextEdit + setHtml）
    QDialog dlg(this);
    dlg.setWindowTitle(tr("EDID Viewer"));
    QVBoxLayout *layout = new QVBoxLayout(&dlg);

    auto *edit = new QTextEdit(&dlg);
    edit->setReadOnly(true);
    edit->setHtml(html);

    layout->addWidget(edit);
    dlg.resize(800, 600);
    dlg.exec();
#else
    QMessageBox::information(this, tr("EDID"),
                             tr("EDID viewer is only implemented on Windows."));
#endif
}

void MainWindow::setupPreviewWindow()
{
    if (previewWindow_)
        return;

    previewWindow_ = new previewwindow();
    previewWindow_->setWindowTitle(QStringLiteral("Preview"));
    previewWindow_->resize(1280, 720);
}

void MainWindow::setupRuntimeStatusTimer()
{
    runtimeStatusTimer_ = new QTimer(this);
    runtimeStatusTimer_->setInterval(500);
    connect(runtimeStatusTimer_, &QTimer::timeout, this, [this]()
            {
                updateRuntimeStatusUi();
                refreshCaptureInfoFromSdkAndRuntime(true);
                refreshDisplayInfoFromCurrentState();

                if (infoDlg_ && infoDlg_->isVisible())
                {
                    infoDlg_->setInfoText(lastInfoText_);
                    infoDlg_->setPropertyPages(captureInfo_.propertyPages);
                    infoDlg_->setCurrentAudioDevice(recordAudioDeviceIdUtf8_);
                }

                if (DpinfoDlg_ && DpinfoDlg_->isVisible())
                    DpinfoDlg_->setInfoText(formatDisplayOutputInfo(displayInfo_));
            });
    runtimeStatusTimer_->start();
}

void MainWindow::setupDebugDock()
{
    debugDock_ = new QDockWidget(tr("Debug Log"), this);
    debugDock_->setObjectName("DebugLogDock");
    debugDock_->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::RightDockWidgetArea);

    debugText_ = new QPlainTextEdit(debugDock_);
    debugText_->setReadOnly(true);
    debugText_->document()->setMaximumBlockCount(2000);
    debugDock_->setWidget(debugText_);

    addDockWidget(Qt::BottomDockWidgetArea, debugDock_);
    debugDock_->hide();

    if (ui->actionDebugLog)
    {
        ui->actionDebugLog->setCheckable(true);
        ui->actionDebugLog->setChecked(false);
    }
}

void MainWindow::setupProcAmpAction()
{
    m_currentProcAmp = {128, 128, 128, 128, 128};
    if (!ui->actionProcAmp)
        return;

    connect(ui->actionProcAmp, &QAction::triggered, this, [this]()
            {
                if (!procampDlg_)
                {
                    procampDlg_ = new ProcAmp(this);
                    procampDlg_->setWindowTitle(tr("ProcAmp"));
                    procampDlg_->setValues(m_currentProcAmp);
                    connect(procampDlg_, &ProcAmp::valuesChanged,
                            this,
                            [this](const gcap_procamp_t &p)
                            {
                                m_currentProcAmp = p;
                                if (h_)
                                    gcap_set_procamp(h_, &p);
                            });
                }

                const int backend = ui->comboBackend ? ui->comboBackend->currentData().toInt() : 1;
                usePacketCallback_ = false;
                const bool supported = (backend == 0 || backend == 1 || backend == 3);
                procampDlg_->setControlsEnabled(supported);
                procampDlg_->setValues(m_currentProcAmp);
                procampDlg_->show();
                procampDlg_->raise();
                procampDlg_->activateWindow(); });
}

void MainWindow::setupBackendControls()
{
    if (!ui->comboBackend)
        return;

    ui->comboBackend->addItem("WinMF GPU", 1);
    ui->comboBackend->addItem("WinMF CPU", 0);
    ui->comboBackend->addItem("DirectShow", 2);

#ifdef _WIN32
    ui->comboBackend->addItem("CaptureSDK", 100);
    capSdk_ = new CaptureSdkSource(this);
    connect(capSdk_, &CaptureSdkSource::frameReady, this, &MainWindow::onFrameArrived, Qt::QueuedConnection);
    connect(capSdk_, &CaptureSdkSource::errorOccurred, this, [this](const QString &m)
            { MainWindow::postLog(QStringLiteral("[CaptureSDK] %1").arg(m), true); });

    if (ui->comboDevice)
    {
        connect(ui->comboBackend, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int)
                {
                    const int backend = ui->comboBackend->currentData().toInt();
                    const bool isCapSdk = (backend == 100);
                    ui->comboDevice->setEnabled(!isCapSdk); });
    }
#endif
}

void MainWindow::initializeDeviceList()
{
    if (!ui->comboDevice)
        return;

    gcap_device_info_t list[16];
    int n = 0;
    if (gcap_enumerate(list, 16, &n) == GCAP_OK)
    {
        for (int i = 0; i < n; ++i)
            ui->comboDevice->addItem(QString::fromUtf8(list[i].name), i);
    }

    if (ui->comboDevice->count() > 0)
    {
        ui->comboDevice->setCurrentIndex(0);
        deviceIndex_ = ui->comboDevice->itemData(0).toInt();
    }
}

void MainWindow::initializeGpuList()
{
#ifdef _WIN32
    if (!ui->comboGpu)
        return;

    ComPtr<IDXGIFactory1> fac;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(fac.GetAddressOf()))) && fac)
    {
        UINT idx = 0;
        while (true)
        {
            ComPtr<IDXGIAdapter1> ad;
            HRESULT hr = fac->EnumAdapters1(idx, &ad);
            if (hr == DXGI_ERROR_NOT_FOUND)
                break;
            if (FAILED(hr) || !ad)
            {
                ++idx;
                continue;
            }

            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(ad->GetDesc1(&desc)))
            {
                if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
                {
                    ++idx;
                    continue;
                }
                QString name = QString::fromWCharArray(desc.Description);
                ui->comboGpu->addItem(name, static_cast<int>(idx));
            }
            ++idx;
        }
    }

    if (ui->comboGpu->count() == 0)
        ui->comboGpu->addItem(QStringLiteral("Default GPU (DXGI)"), -1);

    ui->comboGpu->setCurrentIndex(0);
    gpuIndex_ = ui->comboGpu->currentData().toInt();
    gpuName_ = ui->comboGpu->currentText();
    gcap_set_d3d_adapter(gpuIndex_);
#else
    Q_UNUSED(this);
#endif
}

void MainWindow::setupConnections()
{
    if (ui->comboDevice)
    {
        connect(ui->comboDevice, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int idx)
                {
                    deviceIndex_ = ui->comboDevice->itemData(idx).toInt();
                    invalidateDeviceCapabilityCache();
                    refreshCaptureInfoFromSdkAndRuntime(false);
                    if (infoDlg_ && infoDlg_->isVisible())
                    {
                        infoDlg_->setInfoText(lastInfoText_);
                        infoDlg_->setPropertyPages(captureInfo_.propertyPages);
                    }
                });
    }

#ifdef _WIN32
    if (ui->comboGpu)
    {
        connect(ui->comboGpu,
                QOverload<int>::of(&QComboBox::currentIndexChanged),
                this,
                [this](int idx)
                {
                    gpuIndex_ = ui->comboGpu->itemData(idx).toInt();
                    gpuName_ = ui->comboGpu->itemText(idx);
                    gcap_set_d3d_adapter(gpuIndex_);
                });
    }
#endif

    connect(ui->btnStart, &QPushButton::clicked, this, &MainWindow::onStart);
    connect(ui->btnStop, &QPushButton::clicked, this, &MainWindow::onStop);
    connect(this, &MainWindow::sigFrame, this, &MainWindow::onFrameArrived, Qt::QueuedConnection);
    if (ui->actionDebugLog)
        connect(ui->actionDebugLog, &QAction::toggled, this, &MainWindow::onToggleDebugLog);
    if (debugDock_ && ui->actionDebugLog)
        connect(debugDock_, &QDockWidget::visibilityChanged, ui->actionDebugLog, &QAction::setChecked);
    if (ui->btnShowEdid)
        connect(ui->btnShowEdid, &QPushButton::clicked, this, &MainWindow::onShowEdid);
    if (ui->btnRecord)
        connect(ui->btnRecord, &QPushButton::clicked, this, &MainWindow::onRecord);
    if (ui->actionOpenRecordFolder)
        connect(ui->actionOpenRecordFolder, &QAction::triggered, this, &MainWindow::onOpenRecordFolder);
    if (ui->actionOpenLogFolder)
        connect(ui->actionOpenLogFolder, &QAction::triggered, this, &MainWindow::onOpenLogFolder);
    if (ui->actionOpenSnapshot)
        connect(ui->actionOpenSnapshot, &QAction::triggered, this, &MainWindow::onOpenSnapshot);
    if (ui->actionInputInfo)
        connect(ui->actionInputInfo, &QAction::triggered, this, &MainWindow::onShowInputInfo);
    if (ui->actionDisplayInfo)
        connect(ui->actionDisplayInfo, &QAction::triggered, this, &MainWindow::onShowDisplayInfo);
    if (ui->btnSnapshot)
        connect(ui->btnSnapshot, &QPushButton::clicked, this, &MainWindow::onSnapshot);
    if (ui->actionOpenVendorPropertyPageTest)
        connect(ui->actionOpenVendorPropertyPageTest, &QAction::triggered, this, &MainWindow::onOpenVendorPropertyPageTest);
    if (ui->btnOpenTiff)
        connect(ui->btnOpenTiff, &QPushButton::clicked, this, &MainWindow::onOpenTiffAnalyze);
    if (ui->actionOpenTiffAnalyzer)
        connect(ui->actionOpenTiffAnalyzer, &QAction::triggered, this, &MainWindow::onOpenTiffAnalyze);
}

void MainWindow::logStartupInfo()
{
    const QString logPath = qApp ? qApp->property("logPath").toString() : QString();
    if (!logPath.isEmpty())
        MainWindow::postLog(QStringLiteral("Log file: %1").arg(logPath));
}

void MainWindow::postLog(const QString &line, bool isError)
{
    // 1) 寫到檔案（透過 Qt message handler）
    if (isError)
        qWarning().noquote() << line;
    else
        qInfo().noquote() << line;

    // 2) 同時送到 UI（若存在）
    if (g_mainWindow)
    {
        QMetaObject::invokeMethod(
            g_mainWindow,
            "appendDebugLog",
            Qt::QueuedConnection,
            Q_ARG(QString, line));
    }
}

void MainWindow::appendDebugLog(const QString &line)
{
    if (!debugText_)
        return;

    const QString ts = QDateTime::currentDateTime().toString("HH:mm:ss.zzz");
    debugText_->appendPlainText(QStringLiteral("[%1] %2").arg(ts, line));
}


void MainWindow::onOpenTiffAnalyze()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        tr("Open TIFF"),
        QString(),
        tr("TIFF Files (*.tif *.tiff)"));
    if (path.isEmpty())
        return;

    lastTiffReport_ = TiffAnalyzer::analyzeFile(path);

    if (!tiffAnalysisDlg_)
        tiffAnalysisDlg_ = new TiffAnalysisDialog(this);

    tiffAnalysisDlg_->setReport(lastTiffReport_);
    tiffAnalysisDlg_->show();
    tiffAnalysisDlg_->raise();
    tiffAnalysisDlg_->activateWindow();

    if (ui && ui->labelinfo1)
    {
        if (lastTiffReport_.ok)
        {
            ui->labelinfo1->setEnabled(true);
            ui->labelinfo1->setText(
                tr("TIFF: %1 | stored=%2-bit | effective=%3-bit | 10-bit ramp=%4")
                    .arg(QFileInfo(path).fileName())
                    .arg(lastTiffReport_.storedBitDepth)
                    .arg(lastTiffReport_.effectiveBitDepth)
                    .arg(lastTiffReport_.likelyTenBitRamp ? tr("Yes") : tr("No")));
        }
        else
        {
            ui->labelinfo1->setEnabled(true);
            ui->labelinfo1->setText(tr("TIFF analyze failed: %1").arg(lastTiffReport_.error));
        }
    }

    if (lastTiffReport_.ok)
    {
        MainWindow::postLog(
            QStringLiteral("[TIFF] %1 stored=%2 effective=%3 ramp=%4 fmt=%5 range=%6..%7 unique=%8")
                .arg(path)
                .arg(lastTiffReport_.storedBitDepth)
                .arg(lastTiffReport_.effectiveBitDepth)
                .arg(lastTiffReport_.likelyTenBitRamp ? QStringLiteral("Yes") : QStringLiteral("No"))
                .arg(lastTiffReport_.pixelFormatName)
                .arg(lastTiffReport_.minValue)
                .arg(lastTiffReport_.maxValue)
                .arg(lastTiffReport_.uniqueValueCount));
    }
    else
    {
        MainWindow::postLog(QStringLiteral("[TIFF] analyze failed: %1 (%2)").arg(path, lastTiffReport_.error), true);
    }
}

void MainWindow::on_btnPreview_clicked()
{
    setupPreviewWindow();
    previewWindow_->show();
    previewWindow_->raise();
    previewWindow_->activateWindow();
}
