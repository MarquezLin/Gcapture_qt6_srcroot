#include "mainwindow.h"
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

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    // if (ui->statusbar)
    //     ui->statusbar->showMessage(QStringLiteral("Record mode：Media Foundation (Sink Writer)"));

    ui->labelView->setMinimumSize(640, 360);
    ui->labelView->setAlignment(Qt::AlignCenter);

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

    ui->comboBackend->addItem("WinMF GPU", 1);
    ui->comboBackend->addItem("WinMF CPU", 0);
    // ui->comboBackend->addItem("DirectShow", 2);

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
    delete ui;
}

void MainWindow::onStart()
{
    // If either backend is running, ignore.
    if (h_
#ifdef _WIN32
        || usingCaptureSdk_
#endif
    )
        return;

    int backend = ui->comboBackend ? ui->comboBackend->currentData().toInt() : 1;

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

    gcap_status_t st = gcap_open(deviceIndex_, &h_);
    if (st != GCAP_OK)
    {
        QMessageBox::warning(this, "gcapture",
                             QString("open fail: %1").arg(gcap_strerror(st)));
        h_ = nullptr;
        return;
    }

    // OBS-style: Device Default，不強制指定解析度
    currentProfile_ = {};
    gcap_set_buffers(h_, 6, 0);
    gcap_set_callbacks(h_, &MainWindow::s_vcb, &MainWindow::s_ecb, this);
    gcap_start(h_);
}

void MainWindow::onStop()
{
#ifdef _WIN32
    if (usingCaptureSdk_)
    {
        if (capSdk_)
            capSdk_->stop();
        usingCaptureSdk_ = false;
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

    // 更新「實際來源」資訊
    self->lastFramePtsNs_ = f->pts_ns;
    self->lastFrameWidth_ = f->width;
    self->lastFrameHeight_ = f->height;

    // FPS：集中在 callback 算（避免 UI thread 又算一次造成重複/不一致）
    static uint64_t lastPtsForFps = 0;
    if (lastPtsForFps != 0 && self->lastFramePtsNs_ > lastPtsForFps)
    {
        uint64_t delta = self->lastFramePtsNs_ - lastPtsForFps;
        double fps = 1e9 / double(delta);
        if (fps > 0.0)
            self->avgFps_ = (self->avgFps_ <= 0.0) ? fps : (self->avgFps_ * 0.9 + fps * 0.1);
    }
    lastPtsForFps = self->lastFramePtsNs_;

    // 這裡 f 是 BGRA 一張圖，直接包成 QImage
    QImage img((const uchar *)f->data[0], f->width, f->height, f->stride[0], QImage::Format_ARGB32);
    self->sigFrame(img.copy()); // copy 確保 thread-safe
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

    // 1. Render
    ui->labelView->setPixmap(
        QPixmap::fromImage(img).scaled(
            ui->labelView->size(),
            Qt::KeepAspectRatio,
            Qt::SmoothTransformation));

    // 2. Capture device signal (every frame)
    if (h_)
    {
        gcap_signal_status_t sig{};
        if (gcap_get_signal_status(h_, &sig) == GCAP_OK)
            captureInfo_.signal = sig;
    }

    // 3. Capture device properties (1 Hz)
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

    if (backend == 1)
    {
        displayInfo_.pipe.path = DisplayOutputInfo::Pipeline::Path::WinMFGpu;
        displayInfo_.pipe.adapterName = gpuName_;
        displayInfo_.pipe.adapterIndex = gpuIndex_;
    }
    else if (backend == 0)
    {
        displayInfo_.pipe.path = DisplayOutputInfo::Pipeline::Path::WinMFCpu;
    }
    else
    {
        displayInfo_.pipe.path = DisplayOutputInfo::Pipeline::Path::DirectShow;
    }

    // 5. Update dialogs
    if (infoDlg_)
        infoDlg_->setInfoText(
            formatCaptureDeviceInfo(captureInfo_, avgFps_));

    if (DpinfoDlg_)
        DpinfoDlg_->setInfoText(
            formatDisplayOutputInfo(displayInfo_));
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
    // 第一次打開時建一個 Dialog，之後就重用
    if (!infoDlg_)
    {
        infoDlg_ = new inputinfodialog(this);
        infoDlg_->setWindowTitle(tr("Input Signal Info"));
        // 如果不想關掉就刪除物件，可以加：
        // infoDlg_->setAttribute(Qt::WA_DeleteOnClose, false);
        connect(infoDlg_, &inputinfodialog::audioDeviceSelected,
                this, [this](const QString &id)
                { recordAudioDeviceIdUtf8_ = id; 
                qDebug() << "[AudioSelect] MainWindow store id=" << id; });
    }

    infoDlg_->setCurrentAudioDevice(recordAudioDeviceIdUtf8_);
    // 把目前最新的資訊丟進去
    infoDlg_->setInfoText(lastInfoText_);

    infoDlg_->setCurrentAudioDevice(recordAudioDeviceIdUtf8_);

    // 顯示成「非 modal」的小視窗（可以一邊看畫面一邊開著）
    infoDlg_->show();
    infoDlg_->raise();
    infoDlg_->activateWindow();
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
