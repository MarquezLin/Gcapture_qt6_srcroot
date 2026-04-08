#include "mainwindow.h"

#include "./ui_mainwindow.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QUrl>

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

    MainWindow::postLog(QStringLiteral("[Record] apply audio device=%1")
                            .arg(recordAudioDeviceIdUtf8_.isEmpty() ? QStringLiteral("default") : recordAudioDeviceIdUtf8_));
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
