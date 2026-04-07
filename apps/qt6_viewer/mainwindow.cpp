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
#include <QTextEdit>
#include <QLabel>
#include <QDesktopServices>
#include <QUrl>
#include <QDir>
#include "edid_reader.h"
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

    previewWindow_ = new previewwindow();
    previewWindow_->setWindowTitle(QStringLiteral("Preview"));
    previewWindow_->resize(1280, 720);
    previewWindow_->show();

    runtimeStatusTimer_ = new QTimer(this);
    runtimeStatusTimer_->setInterval(500);
    connect(runtimeStatusTimer_, &QTimer::timeout, this, [this]()
            { updateRuntimeStatusUi(); });
    runtimeStatusTimer_->start();

    // if (ui->statusbar)
    //     ui->statCCusbar->showMessage(QStringLiteral("Record mode：Media Foundation (Sink Writer)"));

    // ui->labelView->setMinimumSize(640, 360);
    // ui->labelView->setAlignment(Qt::AlignCenter);

    // --- Debug Log Dock ---
    debugDock_ = new QDockWidget(tr("Debug Log"), this);
    debugDock_->setObjectName("DebugLogDock"); // 重要：給 Qt 記狀態用
    debugDock_->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::RightDockWidgetArea);

    debugText_ = new QTextEdit(debugDock_);
    debugText_->setReadOnly(true);
    debugDock_->setWidget(debugText_);

    addDockWidget(Qt::BottomDockWidgetArea, debugDock_);
    debugDock_->hide(); // 預設不顯示

    // --- Menu action ---
    ui->actionDebugLog->setCheckable(true);
    ui->actionDebugLog->setChecked(false);

    // ProcAmp dialog
    m_currentProcAmp = {128, 128, 128, 128, 128};
    if (ui->actionProcAmp)
        connect(ui->actionProcAmp, &QAction::triggered, this, [this]()
                {
            // Create dialog lazily
            if (!procampDlg_)
            {
                procampDlg_ = new ProcAmp(this);
                procampDlg_->setWindowTitle(tr("ProcAmp"));
                procampDlg_->setValues(m_currentProcAmp);

                // Live preview.
//                 connect(procampDlg_, &ProcAmp::valuesChanged, this, [this](const gcap_procamp_t& p) {
//     if (h_) gcap_set_procamp(h_, &m_currentProcAmp);
// });

connect(procampDlg_, &ProcAmp::valuesChanged,
        this,
        [this](const gcap_procamp_t& p)
{
    m_currentProcAmp = p;

    if (h_)
        gcap_set_procamp(h_, &p);
});
            }

            // Enable/disable based on backend support
            const int backend = ui->comboBackend ? ui->comboBackend->currentData().toInt() : 1;
    usePacketCallback_ = (backend == 2);
            const bool supported = (backend == 0 || backend == 1 || backend == 3); // Auto 可能最後落在 WinMF
            procampDlg_->setControlsEnabled(supported);
            procampDlg_->setValues(m_currentProcAmp);
            procampDlg_->show();
            procampDlg_->raise();
            procampDlg_->activateWindow(); });

    // ui->comboBackend->addItem("Auto (WinMF→DShow)", 3);
    ui->comboBackend->addItem("WinMF GPU", 1);
    ui->comboBackend->addItem("WinMF CPU", 0);
    ui->comboBackend->addItem("DirectShow", 2);

#ifdef _WIN32
    // Optional backend: external CaptureSDK.dll (loaded at runtime)
    ui->comboBackend->addItem("CaptureSDK", 100);
    capSdk_ = new CaptureSdkSource(this);
    connect(capSdk_, &CaptureSdkSource::frameReady, this, &MainWindow::onFrameArrived, Qt::QueuedConnection);
    connect(capSdk_, &CaptureSdkSource::errorOccurred, this, [this](const QString &m)
            { MainWindow::postLog(QStringLiteral("[CaptureSDK] %1").arg(m), true); });

    // When CaptureSDK selected, device list is irrelevant (keep UI simple).
    if (ui->comboBackend && ui->comboDevice)
    {
        connect(ui->comboBackend, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int)
                {
            const int backend = ui->comboBackend->currentData().toInt();
            const bool isCapSdk = (backend == 100);
            ui->comboDevice->setEnabled(!isCapSdk); });
    }
#endif

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

    connect(ui->comboDevice, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx)
            { deviceIndex_ = ui->comboDevice->itemData(idx).toInt(); });

#ifdef _WIN32
    // ---- 初始化 GPU ComboBox：列舉 DXGI Adapter ----
    if (ui->comboGpu)
    {
        ComPtr<IDXGIFactory1> fac;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                         reinterpret_cast<void **>(fac.GetAddressOf()))) &&
            fac)
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
        {
            // 沒成功列舉任何東西時，放一個 default 選項
            ui->comboGpu->addItem(QStringLiteral("Default GPU (DXGI)"), -1);
        }

        ui->comboGpu->setCurrentIndex(0);
        gpuIndex_ = ui->comboGpu->currentData().toInt();
        gpuName_ = ui->comboGpu->currentText();

        // 通知 DLL，以後 create_d3d() 要用這張 Adapter
        gcap_set_d3d_adapter(gpuIndex_);

        // 切換 GPU 時更新 adapter index
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
    connect(ui->actionDebugLog, &QAction::toggled, this, &MainWindow::onToggleDebugLog);
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

    g_mainWindow = this;

    // 印出 log 檔路徑（給外部測試用）
    const QString logPath = qApp ? qApp->property("logPath").toString() : QString();
    if (!logPath.isEmpty())
        MainWindow::postLog(QStringLiteral("Log file: %1").arg(logPath));
}

MainWindow::~MainWindow()
{
    if (g_mainWindow == this)
        g_mainWindow = nullptr;
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

void MainWindow::onStart()
{
    avgFps_ = 0.0;
    lastFramePtsNs_ = 0;
    lastFrameWidth_ = 0;
    lastFrameHeight_ = 0;
    lastVideoCallbackPtsNs_ = 0;
    lastPacketCallbackPtsNs_ = 0;
    framePacketLogCount_ = 0;
    ++framePacketSessionId_;
    if (ui->statusbar)
        ui->statusbar->showMessage(QStringLiteral("Starting..."));
    qDebug() << "MainWindow::onStart";

    if (!previewWindow_)
    {
        previewWindow_ = new previewwindow();
        previewWindow_->setWindowTitle(QStringLiteral("Preview"));
        previewWindow_->resize(1280, 720);
    }

    previewWindow_->show();
    previewWindow_->raise();
    previewWindow_->activateWindow();

    void *hwnd = previewWindow_->previewHwnd();
    qDebug() << "preview hwnd =" << hwnd;
    // If either backend is running, ignore.
    if (h_
#ifdef _WIN32
        || usingCaptureSdk_
#endif
    )
        return;

    int backend = ui->comboBackend ? ui->comboBackend->currentData().toInt() : 1;
    usePacketCallback_ = false;
    packetLogOnly_ = (backend == 2);

    appendDebugLog(QStringLiteral("[MainWindow] backend=%1 usePacketCallback=%2 packetLogOnly=%3")
                       .arg(backend)
                       .arg(usePacketCallback_ ? "true" : "false")
                       .arg(packetLogOnly_ ? "true" : "false"));

#ifdef _WIN32
    if (backend == 100)
    {
        usingCaptureSdk_ = capSdk_ && capSdk_->start(1920, 1080);
        if (!usingCaptureSdk_)
        {
            QMessageBox::warning(this, "CaptureSDK",
                                 QStringLiteral("Start failed: %1").arg(capSdk_ ? capSdk_->lastError() : QStringLiteral("capSdk_ is null")));
        }
        return;
    }
#endif

    gcap_set_backend(backend);

    // 再把目前選到的 GPU index 丟進 DLL
    if (ui->comboGpu)
        gcap_set_d3d_adapter(ui->comboGpu->currentData().toInt());

    gcap_status_t st = GCAP_OK;

    // OBS-style: Device Default，不強制指定解析度
    currentProfile_ = {};

    st = gcap_create(&h_);
    if (st != GCAP_OK || !h_)
    {
        QMessageBox::warning(this, "gcapture",
                             QString("create fail: %1").arg(gcap_strerror(st)));
        h_ = nullptr;
        return;
    }

    st = gcap_set_buffers(h_, 6, 0);
    if (st != GCAP_OK)
    {
        QMessageBox::warning(this, "gcapture",
                             QString("set buffers fail: %1").arg(gcap_strerror(st)));
        gcap_close(h_);
        h_ = nullptr;
        return;
    }

    st = gcap_set_callbacks(h_, &MainWindow::s_vcb, &MainWindow::s_ecb, this);
    if (st == GCAP_OK && (usePacketCallback_ || packetLogOnly_))
    {
        st = gcap_set_frame_packet_callback(h_, &MainWindow::s_pcb, this);
        if (st == GCAP_OK)
            appendDebugLog(QStringLiteral("[MainWindow] packet callback registered backend=%1 h_=%2")
                               .arg(backend)
                               .arg(reinterpret_cast<quintptr>(h_), 0, 16));
    }
    if (st != GCAP_OK)
    {
        QMessageBox::warning(this, "gcapture",
                             QString("set callbacks fail: %1").arg(gcap_strerror(st)));
        gcap_close(h_);
        h_ = nullptr;
        return;
    }

    // Apply ProcAmp (WinMF GPU/CPU). Other backends may ignore it.
    if (backend == 0 || backend == 1 || backend == 3)
        gcap_set_procamp(h_, &m_currentProcAmp);

    gcap_preview_desc_t pv{};
    pv.hwnd = hwnd;
    pv.enable_preview = 1;

    if (backend == 2)
    {
        // DirectShow: use shared native preview for Phase C-2
        pv.use_fp16_pipeline = 0;
        pv.swapchain_10bit = 0;
    }
    else
    {
        pv.use_fp16_pipeline = 1;
        pv.swapchain_10bit = 1;
    }

    st = gcap_set_preview(h_, &pv);
    qDebug() << "gcap_set_preview st =" << st << ", h_ =" << h_;
    if (st != GCAP_OK)
    {
        QMessageBox::warning(this, "gcapture",
                             QString("set preview fail: %1").arg(gcap_strerror(st)));
        gcap_close(h_);
        h_ = nullptr;
        return;
    }

    st = gcap_open2(h_, deviceIndex_);
    if (st != GCAP_OK)
    {
        QMessageBox::warning(this, "gcapture",
                             QString("open fail: %1").arg(gcap_strerror(st)));
        gcap_close(h_);
        h_ = nullptr;
        return;
    }

    st = gcap_start(h_);
    if (st != GCAP_OK)
    {
        QMessageBox::warning(this, "gcapture",
                             QString("start fail: %1").arg(gcap_strerror(st)));
        gcap_close(h_);
        h_ = nullptr;
        return;
    }

    updateRuntimeStatusUi();
}

void MainWindow::onStop()
{
#ifdef _WIN32
    if (usingCaptureSdk_)
    {
        if (capSdk_)
            capSdk_->stop();
        usingCaptureSdk_ = false;
        if (previewWindow_)
        {
            previewWindow_->clearFrame();
            previewWindow_->update();
            previewWindow_->repaint();
        }
        updateRuntimeStatusUi();
        return;
    }
#endif

    if (!h_)
        return;

    if (recording_)
    {
        gcap_stop_recording(h_);
        recording_ = false;
        ui->btnRecord->setText("Record");
    }

    gcap_stop(h_);
    gcap_close(h_);
    h_ = nullptr;
    if (previewWindow_)
    {
        previewWindow_->clearFrame();
        previewWindow_->update();
        previewWindow_->repaint();
    }
    lastVideoCallbackPtsNs_ = 0;
    lastPacketCallbackPtsNs_ = 0;
    framePacketLogCount_ = 0;
    updateRuntimeStatusUi();
}

void MainWindow::onOpenRecordFolder()
{
    QString baseDir = QCoreApplication::applicationDirPath() + "/recordings";

    QDir dir(baseDir);
    if (!dir.exists())
        QDir().mkpath(baseDir);

    QDesktopServices::openUrl(QUrl::fromLocalFile(dir.absolutePath()));
}

void MainWindow::onOpenLogFolder()
{
    // main.cpp 在啟動時已把 logPath 存在 qApp property
    const QString logFile = qApp ? qApp->property("logPath").toString()
                                 : QString();

    if (!logFile.isEmpty())
    {
        const QFileInfo fi(logFile);
        QDesktopServices::openUrl(
            QUrl::fromLocalFile(fi.absolutePath()));
        return;
    }

    // fallback：<exe>/logs
    const QString fallback =
        QCoreApplication::applicationDirPath() + "/logs";

    QDesktopServices::openUrl(
        QUrl::fromLocalFile(QDir(fallback).absolutePath()));
}

void MainWindow::onOpenSnapshot()
{
    QString baseDir = QCoreApplication::applicationDirPath() + "/snapshots";

    QDir dir(baseDir);
    if (!dir.exists())
        QDir().mkpath(baseDir);

    QDesktopServices::openUrl(QUrl::fromLocalFile(dir.absolutePath()));
}

void MainWindow::onRecord()
{
#ifdef _WIN32
    if (usingCaptureSdk_)
    {
        QMessageBox::information(this, "Record",
                                 "CaptureSDK backend is enabled. This demo currently supports recording only via gcapture backend.");
        return;
    }
#endif
    if (!h_)
    {
        QMessageBox::warning(this, "Record", "Please press Start to begin capturing the screen before recording.");
        return;
    }

    // === 停止錄影 ===
    if (recording_)
    {
        gcap_stop_recording(h_);
        recording_ = false;
        ui->btnRecord->setText("Record");
        // ---- 停錄後，估算實際 bitrate 並顯示 ----
        if (!recordPath_.isEmpty() && recordStartTime_.isValid())
        {
            QFileInfo fi(recordPath_);
            qint64 sizeBytes = fi.size();
            qint64 ms = recordStartTime_.msecsTo(QDateTime::currentDateTime());
            double seconds = ms / 1000.0;
            double bitrateKbps = 0.0;
            if (seconds > 0.0)
                bitrateKbps = (sizeBytes * 8.0 / 1000.0) / seconds; // kbps

            // FPS：優先用實際測到的 avgFps_
            double fps = (avgFps_ > 0.0)
                             ? avgFps_
                             : (currentProfile_.fps_den
                                    ? (double)currentProfile_.fps_num / currentProfile_.fps_den
                                    : (double)currentProfile_.fps_num);

            // 解析度：優先用最後一幀的寬高
            int srcW = lastFrameWidth_ > 0 ? lastFrameWidth_ : currentProfile_.width;
            int srcH = lastFrameHeight_ > 0 ? lastFrameHeight_ : currentProfile_.height;

            QString codecLabel;
            if (currentProfile_.format == GCAP_FMT_P010)
                codecLabel = QStringLiteral("HEVC / H.265 (input P010 10-bit)");
            else
                codecLabel = QStringLiteral("H.264 / AVC(input NV12 8-bit)");

            if (!recordEncoderName_.isEmpty())
                codecLabel = recordEncoderName_;

            QString info = QStringLiteral(
                               "Record done\n"
                               "file:%1\n"
                               "size:%2 MB\n"
                               "Actual resolution:%3 x %4\n"
                               "Actual FPS:%5\n"
                               "encoder:%6\n"
                               "bit rate : %7 kbps")
                               .arg(recordPath_)
                               .arg(QString::number(sizeBytes / (1024.0 * 1024.0), 'f', 2))
                               .arg(srcW)
                               .arg(srcH)
                               .arg(QString::number(fps, 'f', 2))
                               .arg(codecLabel)
                               .arg(QString::number(bitrateKbps, 'f', 1));

            QMessageBox::information(this, "Record", info);

            if (ui->statusbar)
            {
                QString sb = QStringLiteral("Record mode:Media Foundation (Sink Writer) | file:%1 | %2 kbps")
                                 .arg(fi.fileName())
                                 .arg(QString::number(bitrateKbps, 'f', 1));
                ui->statusbar->showMessage(sb);
            }
        }

        return;
    }

    // === 開始錄影 ===

    // 固定輸出資料夾：<exe>/recordings
    QString baseDir = QCoreApplication::applicationDirPath() + "/recordings";
    QDir().mkpath(baseDir);

    // 自動檔名：capture_YYYYMMDD_HHMMSS.mp4
    QDateTime now = QDateTime::currentDateTime();
    QString ts = now.toString("yyyyMMdd_HHmmss");
    QString fileName = QString("capture_%1.mp4").arg(ts);
    QString fullPath = baseDir + "/" + fileName;

    qDebug() << "[Record] apply audio id=" << recordAudioDeviceIdUtf8_;
    if (recordAudioDeviceIdUtf8_.isEmpty())
        gcap_set_recording_audio_device(h_, nullptr); // default
    else
        gcap_set_recording_audio_device(h_, recordAudioDeviceIdUtf8_.toUtf8().constData());

    gcap_status_t st = gcap_start_recording(h_, fullPath.toUtf8().constData());
    if (st != GCAP_OK)
    {
        QMessageBox::warning(this, "Record",
                             QString("Start recording failed: %1").arg(gcap_strerror(st)));
        return;
    }

    // ---- 錄影啟動成功，記錄資訊給之後顯示 ----
    recording_ = true;
    ui->btnRecord->setText("Stop Rec");

    recordStartTime_ = now;
    recordPath_ = fullPath;

    // 推測目前使用的 encoder 名稱（由 pixfmt 判斷）
    if (currentProfile_.format == GCAP_FMT_P010)
        recordEncoderName_ = QStringLiteral("Media Foundation HEVC / H.265 Encoder(Sink Writer, P010 10-bit)");
    else
        recordEncoderName_ = QStringLiteral("Media Foundation H.264 / AVC Encoder(Sink Writer, NV12 8-bit)");

    if (ui->statusbar)
    {
        double fps = currentProfile_.fps_den
                         ? (double)currentProfile_.fps_num / currentProfile_.fps_den
                         : (double)currentProfile_.fps_num;

        // 解析度：優先用最後一幀的寬高
        int srcW = lastFrameWidth_ > 0 ? lastFrameWidth_ : currentProfile_.width;
        int srcH = lastFrameHeight_ > 0 ? lastFrameHeight_ : currentProfile_.height;

        QString sb = QStringLiteral("Record mode : Media Foundation (Sink Writer) | Encord : %1 | %2 x %3")
                         .arg(recordEncoderName_)
                         .arg(srcW)
                         .arg(srcH);

        ui->statusbar->showMessage(sb);
    }
}

void MainWindow::s_vcb(const gcap_frame_t *f, void *u)
{
    auto *self = static_cast<MainWindow *>(u);
    if (!self || !f || !f->data[0] || f->width <= 0 || f->height <= 0)
        return;
    if (self->usePacketCallback_ || self->packetLogOnly_)
        return;
    // 更新「實際來源」資訊
    self->lastFramePtsNs_ = f->pts_ns;
    self->lastFrameWidth_ = f->width;
    self->lastFrameHeight_ = f->height;

    // FPS：集中在 callback 算（避免 UI thread 又算一次造成重複/不一致）
    if (self->lastVideoCallbackPtsNs_ != 0 && self->lastFramePtsNs_ > self->lastVideoCallbackPtsNs_)
    {
        uint64_t delta = self->lastFramePtsNs_ - self->lastVideoCallbackPtsNs_;
        double fps = 1e9 / double(delta);
        if (fps > 0.0)
            self->avgFps_ = (self->avgFps_ <= 0.0) ? fps : (self->avgFps_ * 0.9 + fps * 0.1);
    }
    self->lastVideoCallbackPtsNs_ = self->lastFramePtsNs_;

    // 這裡 f 是 BGRA 一張圖，直接包成 QImage
    QImage img((const uchar *)f->data[0], f->width, f->height, f->stride[0], QImage::Format_ARGB32);
    self->sigFrame(img.copy()); // copy 確保 thread-safe
}

void MainWindow::s_pcb(const gcap_frame_packet_t *pkt, void *u)
{
    auto *self = static_cast<MainWindow *>(u);
    if (!self || !pkt)
        return;

    self->lastFramePtsNs_ = pkt->pts_ns;
    self->lastFrameWidth_ = pkt->width;
    self->lastFrameHeight_ = pkt->height;
    if (self->lastPacketCallbackPtsNs_ != 0 && self->lastFramePtsNs_ > self->lastPacketCallbackPtsNs_)
    {
        uint64_t delta = self->lastFramePtsNs_ - self->lastPacketCallbackPtsNs_;
        double fps = 1e9 / double(delta);
        if (fps > 0.0)
            self->avgFps_ = (self->avgFps_ <= 0.0) ? fps : (self->avgFps_ * 0.9 + fps * 0.1);
    }
    self->lastPacketCallbackPtsNs_ = self->lastFramePtsNs_;

    ++self->framePacketLogCount_;
    const bool shouldLogPacket = (self->framePacketLogCount_ <= 5) || ((self->framePacketLogCount_ % 60) == 0);
    if (shouldLogPacket)
    {
        auto sourceName = [](int s) -> const char *
        {
            switch (s)
            {
            case GCAP_SOURCE_DSHOW_RAWSINK:
                return "DShowRawSink";
            case GCAP_SOURCE_DSHOW_RENDERER:
                return "DShowRenderer";
            case GCAP_SOURCE_WINMF_GPU:
                return "WinMFGPU";
            case GCAP_SOURCE_WINMF_CPU:
                return "WinMFCPU";
            default:
                return "Unknown";
            }
        };
        QString line = QStringLiteral("[FramePacket] session=%1 #%2 backend=%3 source=%4 fmt=%5 %6x%7 planes=%8 gpu=%9 pts=%10")
                           .arg(self->framePacketSessionId_)
                           .arg(self->framePacketLogCount_)
                           .arg(pkt->backend)
                           .arg(QString::fromLatin1(sourceName(pkt->source_kind)))
                           .arg(QString::fromLatin1(packetFmtName(pkt->format)))
                           .arg(pkt->width)
                           .arg(pkt->height)
                           .arg(pkt->plane_count)
                           .arg(pkt->gpu_backed)
                           .arg(QString::number(pkt->pts_ns));
        MainWindow::postLog(line);
    }

    if (!self->usePacketCallback_)
        return;

    QImage img = framePacketToQImage(*pkt);
    if (img.isNull())
        return;

    self->sigFrame(img.copy());
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

void MainWindow::onFrameArrived(const QImage &img)
{
    lastFrameImage_ = img;

    // 主畫面不再顯示 labelView。
    // DShow raw-only 路徑沒有 VMR9 native preview，因此這裡也要把 callback frame 畫到 previewwindow。
    if (previewWindow_)
        previewWindow_->setFrame(img);

    updateRuntimeStatusUi();

    if (h_)
    {
        gcap_signal_status_t sig{};
        if (gcap_get_signal_status(h_, &sig) == GCAP_OK)
            captureInfo_.signal = sig;

        gcap_runtime_info_t rt{};
        if (gcap_get_runtime_info(h_, &rt) == GCAP_OK)
        {
            captureInfo_.signalProbe = rt.signal_probe;
            captureInfo_.negotiated = rt.negotiated;
            captureInfo_.backendName = QString::fromUtf8(rt.backend_name);
            captureInfo_.frameSource = QString::fromUtf8(rt.frame_source);
            captureInfo_.pathName = QString::fromUtf8(rt.path_name);
            captureInfo_.captureFormat = rt.negotiated_desc[0] ? QString::fromUtf8(rt.negotiated_desc)
                                                               : (rt.source_format[0] ? QString::fromUtf8(rt.source_format) : QString());
            captureInfo_.renderFormat = QString::fromUtf8(rt.render_format);
        }
    }

    if (ui && ui->comboDevice)
        captureInfo_.deviceName = ui->comboDevice->currentText();
    if (infoDlg_ && infoDlg_->findChild<QLabel *>("labelAudioInfo"))
        captureInfo_.audioInfo = infoDlg_->findChild<QLabel *>("labelAudioInfo")->text();

    if (h_)
    {
        qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (nowMs - lastPropsQueryMs_ > 1000)
        {
            lastPropsQueryMs_ = nowMs;

            gcap_device_props_t props{};
            if (gcap_get_device_props(h_, &props) == GCAP_OK)
            {
                captureInfo_.driverVersion = QString::fromUtf8(props.driver_version);
                captureInfo_.firmwareVersion = QString::fromUtf8(props.firmware_version);
                captureInfo_.serialNumber = QString::fromUtf8(props.serial_number);
            }
        }
    }

    captureInfo_.supportedFormats.clear();
    captureInfo_.propertyPages.clear();
    if (captureInfo_.deviceName.contains(QStringLiteral("AVerMedia"), Qt::CaseInsensitive))
    {
        captureInfo_.supportedFormats << QStringLiteral("Y210 1920x1080 60.00 fps")
                                      << QStringLiteral("Y210 3840x2160 30.00 fps")
                                      << QStringLiteral("YUY2 1920x1080 60.00 fps")
                                      << QStringLiteral("YUY2 640x480 60.00 fps")
                                      << QStringLiteral("YUY2 720x480 60.00 fps")
                                      << QStringLiteral("YUY2 720x576 50.00 fps")
                                      << QStringLiteral("YUY2 800x600 60.00 fps")
                                      << QStringLiteral("YUY2 1024x768 60.00 fps")
                                      << QStringLiteral("YUY2 1152x864 75.00 fps");
        captureInfo_.propertyPages << QStringLiteral("AVerXBarPropertyPage — Filter")
                                   << QStringLiteral("AVerCertificateProp — Filter")
                                   << QStringLiteral("VideoDecoder Property Page — Filter")
                                   << QStringLiteral("VideoProcAmp Property Page — Filter")
                                   << QStringLiteral("AVerStreamFormatProp — Capture Pin");
    }

    // 4. Display / output info
    displayInfo_.video.size = img.size();
    displayInfo_.video.fps = avgFps_;

    QScreen *scr = windowHandle() ? windowHandle()->screen()
                                  : QGuiApplication::primaryScreen();
    if (scr)
    {
        displayInfo_.desktop.size = scr->size();
        displayInfo_.desktop.hz = scr->refreshRate();
        displayInfo_.desktop.bpp = scr->depth();
    }
    else
    {
        displayInfo_.desktop = {}; // reset
    }

    // ---- Output color info (DXGI/NVAPI) ----
    // 你專案已有：queryDisplayInfoForMonitor(hmon) -> GpuDisplayInfo
    displayInfo_.color = {}; // reset

    if (windowHandle())
    {
        HWND hwnd = reinterpret_cast<HWND>(windowHandle()->winId());
        HMONITOR hmon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);

        GpuDisplayInfo gpu = queryDisplayInfoForMonitor(hmon);
        if (gpu.valid)
        {
            displayInfo_.color.valid = true;
            displayInfo_.color.bitsPerColor = gpu.bitsPerColor;
            displayInfo_.color.colorFormat = gpu.colorSpaceStr;
            displayInfo_.color.dynamicRange = gpu.dynamicRangeStr;
            displayInfo_.color.colorSpaceHdr = gpu.gamutStr;
        }
    }

    // ---- Pipeline (runtime state) ----
    displayInfo_.pipe = {};
    int backend = ui->comboBackend ? ui->comboBackend->currentData().toInt() : 1;
    int actualBackend = backend;
    if (h_)
    {
        int q = gcap_get_active_backend(h_);
        if (q >= 0)
            actualBackend = q;
    }

    if (actualBackend == GCAP_BACKEND_WINMF_GPU)
    {
        displayInfo_.pipe.path = DisplayOutputInfo::Pipeline::Path::WinMFGpu;
        displayInfo_.pipe.adapterName = gpuName_;
        displayInfo_.pipe.adapterIndex = gpuIndex_;
    }
    else if (actualBackend == GCAP_BACKEND_WINMF_CPU)
    {
        displayInfo_.pipe.path = DisplayOutputInfo::Pipeline::Path::WinMFCpu;
    }
    else
    {
        displayInfo_.pipe.path = DisplayOutputInfo::Pipeline::Path::DirectShow;
    }

    // 5. Update dialogs
    lastInfoText_ = formatCaptureDeviceInfo(captureInfo_, avgFps_);
    if (infoDlg_)
        infoDlg_->setInfoText(lastInfoText_);

    if (DpinfoDlg_)
        DpinfoDlg_->setInfoText(
            formatDisplayOutputInfo(displayInfo_));
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

void MainWindow::onSnapshot()
{
    // 1) 檢查目前有沒有任何一張 frame
    if (lastFrameImage_.isNull())
    {
        QMessageBox::information(this, "Snapshot",
                                 "There is currently no screenshot available (please start capturing first).");
        return;
    }

    // 2) 固定輸出資料夾：<exe>/snapshots
    QString baseDir = QCoreApplication::applicationDirPath() + "/snapshots";
    QDir().mkpath(baseDir);

    // 3) 自動檔名：snapshot_YYYYMMDD_HHMMSS_zzz.png
    QDateTime now = QDateTime::currentDateTime();
    QString ts = now.toString("yyyyMMdd_HHmmss_zzz");
    QString fileName = QString("snapshot_%1.png").arg(ts);
    QString fullPath = baseDir + "/" + fileName;

    // 4) 存成 PNG（無損，適合擷取畫面）
    bool ok = lastFrameImage_.save(fullPath, "PNG");
    if (!ok)
    {
        QMessageBox::warning(this, "Snapshot",
                             QString("Snapshot storage failed:%1").arg(fullPath));
        return;
    }

    // 5) UI 提示（狀態列＋訊息）
    if (ui->statusbar)
    {
        ui->statusbar->showMessage(
            QStringLiteral("Snapshot saved: %1").arg(fileName),
            5000); // 顯示 5 秒
    }

    // 如果你不想跳視窗，可以把這段拿掉
    QMessageBox::information(this, "Snapshot",
                             QString("Snapshot already saved:\n%1").arg(fullPath));
}

void MainWindow::onShowInputInfo()
{
#ifdef _WIN32
    const QString deviceText = (ui && ui->comboDevice) ? ui->comboDevice->currentText() : QString();
    const int devIndex = (ui && ui->comboDevice) ? ui->comboDevice->currentIndex() : 0;
    const bool isSc0710 = deviceText.contains(QStringLiteral("SC0710"), Qt::CaseInsensitive);
    if (isSc0710)
    {
        MainWindow::postLog(QStringLiteral("[SignalInfo] SC0710 detected, launching vendor property page. devIndex=%1 name=%2")
                                .arg(devIndex)
                                .arg(deviceText));
        const bool ok = (gcap_open_vendor_property_page(devIndex) != 0);
        if (ok)
            return;

        MainWindow::postLog(QStringLiteral("[SignalInfo] vendor property page launch failed, fallback to generic dialog. devIndex=%1")
                                .arg(devIndex),
                            true);
        QMessageBox::warning(this,
                             tr("Signal Info"),
                             tr("Open SC0710 vendor signal page failed.\nFallback to generic signal info dialog."));
    }
#endif

    if (!infoDlg_)
    {
        infoDlg_ = new inputinfodialog(this);
        infoDlg_->setWindowTitle(tr("Signal Info"));
        connect(infoDlg_, &inputinfodialog::audioDeviceSelected,
                this, [this](const QString &id)
                {
                    recordAudioDeviceIdUtf8_ = id;
                    qDebug() << "[AudioSelect] MainWindow store id=" << id; });
        connect(infoDlg_, &inputinfodialog::openPropertyPageRequested,
                this, [this](const QString &pageNameUtf8, bool capturePin)
                {
#ifdef _WIN32
                    const int devIndex = (ui && ui->comboDevice) ? ui->comboDevice->currentIndex() : 0;
                    MainWindow::postLog(QStringLiteral("[SignalInfo] open property page request: devIndex=%1 page=%2 target=%3")
                                            .arg(devIndex)
                                            .arg(pageNameUtf8)
                                            .arg(capturePin ? QStringLiteral("Capture Pin") : QStringLiteral("Filter")));
                    const bool ok = (gcap_open_named_property_page(devIndex, pageNameUtf8.toUtf8().constData(), capturePin ? 1 : 0) != 0);
                    if (!ok)
                    {
                        QMessageBox::warning(this,
                                             tr("Property Page"),
                                             tr("Open property page failed: %1").arg(pageNameUtf8));
                    }
#else
                    Q_UNUSED(pageNameUtf8);
                    Q_UNUSED(capturePin);
#endif
                });
    }

    infoDlg_->setWindowTitle(tr("Signal Info"));
    infoDlg_->setCurrentAudioDevice(recordAudioDeviceIdUtf8_);

    if (ui && ui->comboDevice)
        captureInfo_.deviceName = ui->comboDevice->currentText();
    if (QLabel *audioLabel = infoDlg_->findChild<QLabel *>("labelAudioInfo"))
        captureInfo_.audioInfo = audioLabel->text();

    if (h_)
    {
        gcap_signal_status_t sig{};
        if (gcap_get_signal_status(h_, &sig) == GCAP_OK)
            captureInfo_.signal = sig;

        gcap_runtime_info_t rt{};
        if (gcap_get_runtime_info(h_, &rt) == GCAP_OK)
        {
            captureInfo_.signalProbe = rt.signal_probe;
            captureInfo_.negotiated = rt.negotiated;
            captureInfo_.backendName = QString::fromUtf8(rt.backend_name);
            captureInfo_.frameSource = QString::fromUtf8(rt.frame_source);
            captureInfo_.pathName = QString::fromUtf8(rt.path_name);
            captureInfo_.captureFormat = rt.negotiated_desc[0] ? QString::fromUtf8(rt.negotiated_desc)
                                                               : (rt.source_format[0] ? QString::fromUtf8(rt.source_format) : QString());
            captureInfo_.renderFormat = QString::fromUtf8(rt.render_format);
        }

        gcap_device_props_t props{};
        if (gcap_get_device_props(h_, &props) == GCAP_OK)
        {
            captureInfo_.driverVersion = QString::fromUtf8(props.driver_version);
            captureInfo_.firmwareVersion = QString::fromUtf8(props.firmware_version);
            captureInfo_.serialNumber = QString::fromUtf8(props.serial_number);
        }
    }

    captureInfo_.supportedFormats.clear();
    captureInfo_.propertyPages.clear();
    const QString deviceName = captureInfo_.deviceName;
    if (deviceName.contains(QStringLiteral("AVerMedia"), Qt::CaseInsensitive))
    {
        captureInfo_.supportedFormats << QStringLiteral("Y210 1920x1080 60.00 fps")
                                      << QStringLiteral("Y210 3840x2160 30.00 fps")
                                      << QStringLiteral("YUY2 1920x1080 60.00 fps")
                                      << QStringLiteral("YUY2 640x480 60.00 fps")
                                      << QStringLiteral("YUY2 720x480 60.00 fps")
                                      << QStringLiteral("YUY2 720x576 50.00 fps")
                                      << QStringLiteral("YUY2 800x600 60.00 fps")
                                      << QStringLiteral("YUY2 1024x768 60.00 fps")
                                      << QStringLiteral("YUY2 1152x864 75.00 fps");
        captureInfo_.propertyPages << QStringLiteral("AVerXBarPropertyPage — Filter")
                                   << QStringLiteral("AVerCertificateProp — Filter")
                                   << QStringLiteral("VideoDecoder Property Page — Filter")
                                   << QStringLiteral("VideoProcAmp Property Page — Filter")
                                   << QStringLiteral("AVerStreamFormatProp — Capture Pin");
    }

    lastInfoText_ = formatCaptureDeviceInfo(captureInfo_, avgFps_);
    infoDlg_->setInfoText(lastInfoText_);
    infoDlg_->setPropertyPages(captureInfo_.propertyPages);
    infoDlg_->setCurrentAudioDevice(recordAudioDeviceIdUtf8_);
    infoDlg_->show();
    infoDlg_->raise();
    infoDlg_->activateWindow();
}

void MainWindow::onOpenVendorPropertyPageTest()
{
#ifdef _WIN32
    const int devIndex = ui && ui->comboDevice ? ui->comboDevice->currentIndex() : 0;
    qDebug() << "[VendorPageTest] trigger devIndex=" << devIndex;
    MainWindow::postLog(QStringLiteral("[VendorPageTest] begin devIndex=%1").arg(devIndex));

    const bool ok = (gcap_open_vendor_property_page(devIndex) != 0);
    MainWindow::postLog(QStringLiteral("[VendorPageTest] launch result=%1 devIndex=%2")
                            .arg(ok ? QStringLiteral("STARTED") : QStringLiteral("FAIL"))
                            .arg(devIndex),
                        !ok);
    if (!ok)
    {
        QMessageBox::warning(this,
                             tr("Vendor Property Page"),
                             tr("Open Vendor Property Page failed.\nPlease check debug log."));
    }
#else
    QMessageBox::information(this,
                             tr("Vendor Property Page"),
                             tr("This test is only available on Windows."));
#endif
}

void MainWindow::onShowDisplayInfo()
{
    // 第一次打開時建一個 Dialog，之後就重用
    if (!DpinfoDlg_)
    {
        DpinfoDlg_ = new DisplayInfoDialog(this);
        DpinfoDlg_->setWindowTitle(tr("Display Info"));
    }

    // 把目前最新的資訊丟進去
    DpinfoDlg_->setInfoText(lastInfoText_);

    // 顯示成「非 modal」的小視窗（可以一邊看畫面一邊開著）
    DpinfoDlg_->show();
    DpinfoDlg_->raise();
    DpinfoDlg_->activateWindow();
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
    debugText_->append(QStringLiteral("[%1] %2").arg(ts, line));
}

void MainWindow::on_btnPreview_clicked()
{
    if (!previewWindow_)
    {
        previewWindow_ = new previewwindow();
        previewWindow_->setWindowTitle(QStringLiteral("Preview"));
        previewWindow_->resize(1280, 720);
    }

    previewWindow_->show();
    previewWindow_->raise();
    previewWindow_->activateWindow();
}
