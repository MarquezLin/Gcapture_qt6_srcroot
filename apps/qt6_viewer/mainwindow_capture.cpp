#include "mainwindow.h"

#include "./ui_mainwindow.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QUrl>

void MainWindow::resetRuntimeTracking()
{
    avgFps_ = 0.0;
    lastFramePtsNs_ = 0;
    lastFrameWidth_ = 0;
    lastFrameHeight_ = 0;
    lastVideoCallbackPtsNs_ = 0;
    lastPacketCallbackPtsNs_ = 0;
    framePacketLogCount_ = 0;
    ++framePacketSessionId_;
}

void MainWindow::clearPreviewSurface()
{
    if (!previewWindow_)
        return;

    previewWindow_->clearFrame();
    previewWindow_->update();
    previewWindow_->repaint();
}

void MainWindow::closeCaptureSession()
{
    if (!h_)
        return;

    gcap_close(h_);
    h_ = nullptr;
}

bool MainWindow::showCaptureErrorAndClose(const QString &action, gcap_status_t st, const char *apiName)
{
    QString message = QStringLiteral("%1 fail: %2").arg(action, QString::fromUtf8(gcap_strerror(st)));
    if (apiName && *apiName)
    {
        MainWindow::postLog(QStringLiteral("[Capture] %1 failed at %2: %3")
                                .arg(action, QString::fromUtf8(apiName), QString::fromUtf8(gcap_strerror(st))),
                            true);
    }
    QMessageBox::warning(this, QStringLiteral("gcapture"), message);
    closeCaptureSession();
    return false;
}

void MainWindow::stopRecordingSession(bool showSummary)
{
    if (!recording_ || !h_)
        return;

    gcap_stop_recording(h_);
    recording_ = false;
    ui->btnRecord->setText(QStringLiteral("Record"));

    if (!showSummary)
        return;

    if (!recordPath_.isEmpty() && recordStartTime_.isValid())
    {
        QFileInfo fi(recordPath_);
        const qint64 sizeBytes = fi.size();
        const qint64 ms = recordStartTime_.msecsTo(QDateTime::currentDateTime());
        const double seconds = ms / 1000.0;
        double bitrateKbps = 0.0;
        if (seconds > 0.0)
            bitrateKbps = (sizeBytes * 8.0 / 1000.0) / seconds;

        const double fps = (avgFps_ > 0.0)
                               ? avgFps_
                               : (currentProfile_.fps_den
                                      ? static_cast<double>(currentProfile_.fps_num) / currentProfile_.fps_den
                                      : static_cast<double>(currentProfile_.fps_num));

        const int srcW = lastFrameWidth_ > 0 ? lastFrameWidth_ : currentProfile_.width;
        const int srcH = lastFrameHeight_ > 0 ? lastFrameHeight_ : currentProfile_.height;

        QString codecLabel;
        if (currentProfile_.format == GCAP_FMT_P010)
            codecLabel = QStringLiteral("HEVC / H.265 (input P010 10-bit)");
        else
            codecLabel = QStringLiteral("H.264 / AVC(input NV12 8-bit)");

        if (!recordEncoderName_.isEmpty())
            codecLabel = recordEncoderName_;

        const QString info = QStringLiteral(
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

        QMessageBox::information(this, QStringLiteral("Record"), info);

        if (ui->statusbar)
        {
            const QString sb = QStringLiteral("Record mode:Media Foundation (Sink Writer) | file:%1 | %2 kbps")
                                   .arg(fi.fileName())
                                   .arg(QString::number(bitrateKbps, 'f', 1));
            ui->statusbar->showMessage(sb);
        }
    }
}

QString MainWindow::buildRecordingPath(const QDateTime &now) const
{
    const QString baseDir = QCoreApplication::applicationDirPath() + QStringLiteral("/recordings");
    QDir().mkpath(baseDir);
    const QString ts = now.toString(QStringLiteral("yyyyMMdd_HHmmss"));
    return baseDir + QStringLiteral("/capture_") + ts + QStringLiteral(".mp4");
}

void MainWindow::applySelectedRecordingAudioDevice()
{
    const QString deviceId = recordAudioDeviceIdUtf8_.isEmpty() ? QStringLiteral("default") : recordAudioDeviceIdUtf8_;
    MainWindow::postLog(QStringLiteral("[Record] apply audio device=%1").arg(deviceId));

    if (recordAudioDeviceIdUtf8_.isEmpty())
        gcap_set_recording_audio_device(h_, nullptr);
    else
        gcap_set_recording_audio_device(h_, recordAudioDeviceIdUtf8_.toUtf8().constData());
}

void MainWindow::onStart()
{
    if (h_
#ifdef _WIN32
        || usingCaptureSdk_
#endif
    )
    {
        MainWindow::postLog(QStringLiteral("[Start] ignored: capture session is already running."));
        if (ui->statusbar)
            ui->statusbar->showMessage(QStringLiteral("Capture is already running."), 3000);
        if (previewWindow_)
        {
            previewWindow_->show();
            previewWindow_->raise();
            previewWindow_->activateWindow();
        }
        return;
    }

    resetRuntimeTracking();
    if (ui->statusbar)
        ui->statusbar->showMessage(QStringLiteral("Starting..."));

    setupPreviewWindow();
    previewWindow_->show();
    previewWindow_->raise();
    previewWindow_->activateWindow();

    void *hwnd = previewWindow_->previewHwnd();

    const int backend = ui->comboBackend ? ui->comboBackend->currentData().toInt() : 1;
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
            QMessageBox::warning(this, QStringLiteral("CaptureSDK"),
                                 QStringLiteral("Start failed: %1")
                                     .arg(capSdk_ ? capSdk_->lastError() : QStringLiteral("capSdk_ is null")));
        }
        return;
    }
#endif

    gcap_set_backend(backend);
    if (ui->comboGpu)
        gcap_set_d3d_adapter(ui->comboGpu->currentData().toInt());

    currentProfile_ = {};

    gcap_status_t st = gcap_create(&h_);
    if (st != GCAP_OK || !h_)
    {
        QMessageBox::warning(this, QStringLiteral("gcapture"),
                             QStringLiteral("create fail: %1").arg(QString::fromUtf8(gcap_strerror(st))));
        h_ = nullptr;
        return;
    }

    st = gcap_set_buffers(h_, 6, 0);
    if (st != GCAP_OK)
    {
        showCaptureErrorAndClose(QStringLiteral("set buffers"), st, "gcap_set_buffers");
        return;
    }

    st = gcap_set_callbacks(h_, &MainWindow::s_vcb, &MainWindow::s_ecb, this);
    if (st == GCAP_OK && (usePacketCallback_ || packetLogOnly_))
    {
        st = gcap_set_frame_packet_callback(h_, &MainWindow::s_pcb, this);
        if (st == GCAP_OK)
        {
            appendDebugLog(QStringLiteral("[MainWindow] packet callback registered backend=%1 h_=%2")
                               .arg(backend)
                               .arg(reinterpret_cast<quintptr>(h_), 0, 16));
        }
    }
    if (st != GCAP_OK)
    {
        showCaptureErrorAndClose(QStringLiteral("set callbacks"), st,
                                 (usePacketCallback_ || packetLogOnly_) ? "gcap_set_frame_packet_callback" : "gcap_set_callbacks");
        return;
    }

    if (backend == 0 || backend == 1 || backend == 3)
        gcap_set_procamp(h_, &m_currentProcAmp);

    gcap_preview_desc_t pv{};
    pv.hwnd = hwnd;
    pv.enable_preview = 1;
    if (backend == 2)
    {
        pv.use_fp16_pipeline = 1;
        pv.swapchain_10bit = 1;
    }
    else
    {
        pv.use_fp16_pipeline = 1;
        pv.swapchain_10bit = 1;
    }

    st = gcap_set_preview(h_, &pv);
    if (st != GCAP_OK)
    {
        showCaptureErrorAndClose(QStringLiteral("set preview"), st, "gcap_set_preview");
        return;
    }

    st = gcap_open2(h_, deviceIndex_);
    if (st != GCAP_OK)
    {
        showCaptureErrorAndClose(QStringLiteral("open"), st, "gcap_open2");
        return;
    }

    st = gcap_start(h_);
    if (st != GCAP_OK)
    {
        showCaptureErrorAndClose(QStringLiteral("start"), st, "gcap_start");
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
        clearPreviewSurface();
        updateRuntimeStatusUi();
        return;
    }
#endif

    if (!h_)
        return;

    stopRecordingSession(false);
    gcap_stop(h_);
    closeCaptureSession();
    clearPreviewSurface();
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
    const QString logFile = qApp ? qApp->property("logPath").toString() : QString();

    if (!logFile.isEmpty())
    {
        const QFileInfo fi(logFile);
        QDesktopServices::openUrl(QUrl::fromLocalFile(fi.absolutePath()));
        return;
    }

    const QString fallback = QCoreApplication::applicationDirPath() + "/logs";
    QDesktopServices::openUrl(QUrl::fromLocalFile(QDir(fallback).absolutePath()));
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
        QMessageBox::information(this, QStringLiteral("Record"),
                                 QStringLiteral("CaptureSDK backend is enabled. This demo currently supports recording only via gcapture backend."));
        return;
    }
#endif
    if (!h_)
    {
        QMessageBox::warning(this, QStringLiteral("Record"),
                             QStringLiteral("Please press Start to begin capturing the screen before recording."));
        return;
    }

    if (recording_)
    {
        stopRecordingSession(true);
        return;
    }

    const QDateTime now = QDateTime::currentDateTime();
    const QString fullPath = buildRecordingPath(now);

    applySelectedRecordingAudioDevice();

    const gcap_status_t st = gcap_start_recording(h_, fullPath.toUtf8().constData());
    if (st != GCAP_OK)
    {
        QMessageBox::warning(this, QStringLiteral("Record"),
                             QStringLiteral("Start recording failed: %1").arg(QString::fromUtf8(gcap_strerror(st))));
        return;
    }

    recording_ = true;
    ui->btnRecord->setText(QStringLiteral("Stop Rec"));
    recordStartTime_ = now;
    recordPath_ = fullPath;

    if (currentProfile_.format == GCAP_FMT_P010)
        recordEncoderName_ = QStringLiteral("Media Foundation HEVC / H.265 Encoder(Sink Writer, P010 10-bit)");
    else
        recordEncoderName_ = QStringLiteral("Media Foundation H.264 / AVC Encoder(Sink Writer, NV12 8-bit)");

    if (ui->statusbar)
    {
        const int srcW = lastFrameWidth_ > 0 ? lastFrameWidth_ : currentProfile_.width;
        const int srcH = lastFrameHeight_ > 0 ? lastFrameHeight_ : currentProfile_.height;
        const QString sb = QStringLiteral("Record mode : Media Foundation (Sink Writer) | Encord : %1 | %2 x %3")
                               .arg(recordEncoderName_)
                               .arg(srcW)
                               .arg(srcH);
        ui->statusbar->showMessage(sb);
    }
}
