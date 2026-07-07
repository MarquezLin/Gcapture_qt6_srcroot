#include "mainwindow.h"

#include "./ui_mainwindow.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QUrl>
namespace
{
QString recordInputFormatName(gcap_pixfmt_t fmt)
{
    return QString::fromUtf8(gcap_pixfmt_name(fmt));
}

gcap_pixfmt_t effectiveRecordingFormat(gcap_handle h, gcap_pixfmt_t fallback)
{
    if (!h)
        return fallback;
    gcap_runtime_info_t rt{};
    if (gcap_get_runtime_info(h, &rt) == GCAP_OK && gcap_pixfmt_bit_depth(rt.negotiated.pixfmt) > 0)
        return rt.negotiated.pixfmt;
    return fallback;
}

QString buildRecordEncoderLabel(gcap_handle h, int backend, gcap_pixfmt_t fallbackFmt)
{
    gcap_recording_info_t info{};
    if (h && gcap_get_recording_info(h, &info) == GCAP_OK && info.encoder_name[0])
        return QString::fromUtf8(info.encoder_name);

    const gcap_pixfmt_t fmt = fallbackFmt;
    const QString inFmt = recordInputFormatName(fmt);
    if (backend == GCAP_BACKEND_DSHOW)
    {
        if (fmt == GCAP_FMT_P010)
            return QStringLiteral("FFmpeg HEVC / H.265 via Media Foundation (input P010 10-bit, video-only)");
        if (fmt == GCAP_FMT_Y210)
            return QStringLiteral("FFmpeg HEVC / H.265 via Media Foundation (input Y210 10-bit 4:2:2, video-only)");
        return QStringLiteral("FFmpeg H.264 / AVC via Media Foundation (input %1, video-only)").arg(inFmt);
    }
    if (gcap_recording_uses_hevc_main10(fmt))
        return QStringLiteral("Media Foundation HEVC / H.265 Encoder (Sink Writer, input %1 / 10-bit)").arg(inFmt);
    return QStringLiteral("Media Foundation H.264 / AVC Encoder (Sink Writer, input %1 / 8-bit)").arg(inFmt);
}

QString buildRecordModeLabel(gcap_handle h, int backend)
{
    gcap_recording_info_t info{};
    if (h && gcap_get_recording_info(h, &info) == GCAP_OK && info.mode_name[0])
        return QString::fromUtf8(info.mode_name);
    return QString::fromUtf8(gcap_recording_mode_name(backend));
}

#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
gcap_pixfmt_t gvfgRecordingFormatFromRuntime(const gvfg_runtime_info_t &rt, gcap_pixfmt_t fallback)
{
    const QString fmt = QString::fromUtf8(rt.last_frame.pixel_format).trimmed().toUpper();
    if (fmt == QStringLiteral("YUY2"))
        return GCAP_FMT_YUY2;
    if (fmt == QStringLiteral("NV12"))
        return GCAP_FMT_NV12;
    if (fmt == QStringLiteral("P010"))
        return GCAP_FMT_P010;
    if (fmt == QStringLiteral("Y210"))
        return GCAP_FMT_Y210;
    if (fmt == QStringLiteral("BGRA8") || fmt == QStringLiteral("BGRA") || fmt == QStringLiteral("BGRX32"))
        return GCAP_FMT_ARGB;
    return fallback;
}
#endif
}


void MainWindow::resetRuntimeTracking()
{
    avgFps_ = 0.0;
    lastFramePtsNs_ = 0;
    lastFrameWidth_ = 0;
    lastFrameHeight_ = 0;
    lastVideoCallbackPtsNs_ = 0;
    lastPacketCallbackPtsNs_ = 0;
    framePacketLogCount_ = 0;
    lastGvfgRawStateKey_.clear();
    ++framePacketSessionId_;
    lastWatchdogFrameCounter_ = 0;
    frameStallTicks_ = 0;
    frameStallWarningActive_ = false;
    initialPreviewSizeApplied_ = false;
    lastFrameImage_ = QImage();
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
    stopPreviewAudio();

#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
    if (usingGvfg_ && gvfg_)
    {
        gvfg_->stop();
        usingGvfg_ = false;
    }
#endif

    if (!h_)
        return;

    gcap_set_frame_packet_callback(h_, nullptr, nullptr);
    gcap_set_callbacks(h_, nullptr, nullptr, nullptr);
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
    if (!recording_)
        return;

#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
    if (usingGvfg_ && gvfg_)
    {
        const uint64_t framesWritten = gvfg_->recordingFrames();
        gvfg_->stopRecording();
        recording_ = false;
        ui->btnRecord->setText(QStringLiteral("Record"));

        if (showSummary && !recordPath_.isEmpty() && recordStartTime_.isValid())
        {
            QFileInfo fi(recordPath_);
            const qint64 sizeBytes = fi.size();
            if (framesWritten == 0 || !fi.exists() || sizeBytes <= 0)
            {
                const QString info = QStringLiteral(
                                         "No frames were written.\n"
                                         "file:%1\n"
                                         "frames written:%2\n"
                                         "The recording was stopped before the GVFG recorder wrote its first frame.")
                                         .arg(recordPath_)
                                         .arg(QString::number(static_cast<qulonglong>(framesWritten)));
                QMessageBox::warning(this, QStringLiteral("Record"), info);

                if (ui->statusbar)
                    ui->statusbar->showMessage(QStringLiteral("Record stopped: no frames were written"), 8000);
                return;
            }

            const qint64 ms = recordStartTime_.msecsTo(QDateTime::currentDateTime());
            const double seconds = ms / 1000.0;
            const double bitrateKbps = seconds > 0.0 ? (sizeBytes * 8.0 / 1000.0) / seconds : 0.0;
            const double captureFps = seconds > 0.0 ? double(framesWritten) / seconds : avgFps_;
            const int srcW = lastFrameWidth_ > 0 ? lastFrameWidth_ : currentProfile_.width;
            const int srcH = lastFrameHeight_ > 0 ? lastFrameHeight_ : currentProfile_.height;

            const QString info = QStringLiteral(
                                     "Record done\n"
                                     "file:%1\n"
                                     "size:%2 MB\n"
                                     "Actual resolution:%3 x %4\n"
                                     "Capture FPS:%5\n"
                                     "record mode:%6\n"
                                     "encoder:%7\n"
                                     "frames written:%8\n"
                                     "file avg bit rate:%9 kbps")
                                     .arg(recordPath_)
                                     .arg(QString::number(sizeBytes / (1024.0 * 1024.0), 'f', 2))
                                     .arg(srcW)
                                     .arg(srcH)
                                     .arg(QString::number(captureFps, 'f', 2))
                                     .arg(QStringLiteral("GVFG + FFmpeg MP4"))
                                     .arg(recordEncoderName_.isEmpty() ? QStringLiteral("FFmpeg") : recordEncoderName_)
                                     .arg(QString::number(static_cast<qulonglong>(framesWritten)))
                                     .arg(QString::number(bitrateKbps, 'f', 1));
            QMessageBox::information(this, QStringLiteral("Record"), info);

            if (ui->statusbar)
            {
                ui->statusbar->showMessage(QStringLiteral("Record done: GVFG + FFmpeg MP4 | file:%1 | avg %2 kbps")
                                               .arg(fi.fileName())
                                               .arg(QString::number(bitrateKbps, 'f', 1)));
            }
        }
        return;
    }
#endif

    if (!h_)
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

        const double captureFps = (avgFps_ > 0.0)
                               ? avgFps_
                               : (currentProfile_.fps_den
                                      ? static_cast<double>(currentProfile_.fps_num) / currentProfile_.fps_den
                                      : static_cast<double>(currentProfile_.fps_num));

        const int srcW = lastFrameWidth_ > 0 ? lastFrameWidth_ : currentProfile_.width;
        const int srcH = lastFrameHeight_ > 0 ? lastFrameHeight_ : currentProfile_.height;
        const int backend = ui->comboBackend ? ui->comboBackend->currentData().toInt() : GCAP_BACKEND_DSHOW;
        gcap_recording_info_t sdkRecInfo{};
        const bool hasSdkRecInfo = (h_ && gcap_get_recording_info(h_, &sdkRecInfo) == GCAP_OK);
        gcap_recording_stats_t sdkRecStats{};
        const bool hasSdkRecStats = (h_ && gcap_get_recording_stats(h_, &sdkRecStats) == GCAP_OK);
        const QString modeLabel = (hasSdkRecInfo && sdkRecInfo.mode_name[0])
                                    ? QString::fromUtf8(sdkRecInfo.mode_name)
                                    : buildRecordModeLabel(h_, backend);
        const QString codecLabel = recordEncoderName_.isEmpty()
                                     ? ((hasSdkRecInfo && sdkRecInfo.encoder_name[0])
                                           ? QString::fromUtf8(sdkRecInfo.encoder_name)
                                           : buildRecordEncoderLabel(h_, backend, effectiveRecordingFormat(h_, currentProfile_.format)))
                                     : recordEncoderName_;

        double recordOutputFps = currentProfile_.fps_den
                                     ? static_cast<double>(currentProfile_.fps_num) / currentProfile_.fps_den
                                     : static_cast<double>(currentProfile_.fps_num);
        if (hasSdkRecInfo && sdkRecInfo.output_fps_num > 0 && sdkRecInfo.output_fps_den > 0)
            recordOutputFps = static_cast<double>(sdkRecInfo.output_fps_num) / sdkRecInfo.output_fps_den;

        QString info = QStringLiteral(
                           "Record done\n"
                           "file:%1\n"
                           "size:%2 MB\n"
                           "Actual resolution:%3 x %4\n"
                           "Capture FPS:%5\n"
                           "Record output FPS:%6\n"
                           "record mode:%7\n"
                           "encoder:%8\n"
                           "file avg bit rate:%9 kbps")
                           .arg(recordPath_)
                           .arg(QString::number(sizeBytes / (1024.0 * 1024.0), 'f', 2))
                           .arg(srcW)
                           .arg(srcH)
                           .arg(QString::number(captureFps, 'f', 2))
                           .arg(QString::number(recordOutputFps, 'f', 2))
                           .arg(modeLabel)
                           .arg(codecLabel)
                           .arg(QString::number(bitrateKbps, 'f', 1));

        if (hasSdkRecStats)
        {
            info += QStringLiteral("\nframes written:%1\nframes dropped:%2\ninput frames:%3")
                        .arg(QString::number(static_cast<qulonglong>(sdkRecStats.frames_written)))
                        .arg(QString::number(static_cast<qulonglong>(sdkRecStats.frames_dropped)))
                        .arg(QString::number(static_cast<qulonglong>(sdkRecStats.input_frames)));
        }

        QMessageBox::information(this, QStringLiteral("Record"), info);

        if (ui->statusbar)
        {
            const QString sb = QStringLiteral("Record done: %1 | file:%2 | avg %3 kbps")
                                   .arg(modeLabel)
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

void MainWindow::startPreviewAudio()
{
    stopPreviewAudio();

    const QString videoDeviceName = currentDeviceText();
    if (videoDeviceName.isEmpty())
        return;

    gcap_audio_device_t audio{};
    const QByteArray videoNameUtf8 = videoDeviceName.toUtf8();
    if (!gcap_audio_find_device_for_capture(videoNameUtf8.constData(), &audio))
    {
        MainWindow::postLog(QStringLiteral("[AudioPreview] no matching WASAPI capture endpoint for video device=%1")
                                .arg(videoDeviceName));
        return;
    }

    gcap_audio_capture_config_t cfg{};
    cfg.device_id = audio.id;
    cfg.sample_rate = audio.sample_rate;
    cfg.channels = audio.channels;

    const int st = gcap_start_audio_capture(&cfg);
    if (st != GCAP_OK)
    {
        MainWindow::postLog(QStringLiteral("[AudioPreview] start failed for endpoint=%1 status=%2")
                                .arg(QString::fromUtf8(audio.name))
                                .arg(st),
                            true);
        return;
    }

    previewAudioActive_ = true;
    MainWindow::postLog(QStringLiteral("[AudioPreview] started endpoint=%1 (%2 Hz, %3 ch)")
                            .arg(QString::fromUtf8(audio.name))
                            .arg(audio.sample_rate)
                            .arg(audio.channels));
}

void MainWindow::stopPreviewAudio()
{
    if (!previewAudioActive_)
        return;

    gcap_stop_audio_capture();
    previewAudioActive_ = false;
    MainWindow::postLog(QStringLiteral("[AudioPreview] stopped"));
}

void MainWindow::onStart()
{
    if (h_
#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
        || usingGvfg_
#endif
    )
    {
        MainWindow::postLog(QStringLiteral("[Start] ignored: capture session is already running."));
        if (ui->statusbar)
            ui->statusbar->showMessage(QStringLiteral("Capture is already running."), 3000);
        return;
    }

    resetRuntimeTracking();
    invalidateDeviceCapabilityCache();
    if (ui->statusbar)
        ui->statusbar->showMessage(QStringLiteral("Starting..."));

    setupPreviewWindow();
    previewWindow_->clearFrame();

    void *hwnd = previewWindow_ ? previewWindow_->previewHwnd() : nullptr;

    const int backend = ui->comboBackend ? ui->comboBackend->currentData().toInt() : 1;
    usePacketCallback_ = false;
    packetLogOnly_ = (backend == 2);

    appendDebugLog(QStringLiteral("[MainWindow] backend=%1 usePacketCallback=%2 packetLogOnly=%3")
                       .arg(backend)
                       .arg(usePacketCallback_ ? "true" : "false")
                       .arg(packetLogOnly_ ? "true" : "false"));

#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
    if (backend == kQtViewerGvfgBackend)
    {
        if (!gvfg_)
        {
            QMessageBox::warning(this, QStringLiteral("GVFG"), QStringLiteral("GVFG source is not available."));
            return;
        }

        currentProfile_ = {};
        currentProfile_.mode = GCAP_PROFILE_DEVICE_DEFAULT;
        currentProfile_.format = GCAP_FMT_YUY2;
        currentProfile_.fps_num = 30000;
        currentProfile_.fps_den = 1001;

        const bool started = gvfg_->start(hwnd, deviceIndex_, selectedPreviewBitDepthMode());
        if (!started)
        {
            usingGvfg_ = false;
            clearPreviewSurface();
            if (ui->statusbar)
                ui->statusbar->showMessage(QStringLiteral("GVFG start failed"), 5000);
            return;
        }

        usingGvfg_ = true;
        const gvfg_signal_status_t sig = gvfg_->signalStatus();
        if (sig.width > 0)
            currentProfile_.width = sig.width;
        if (sig.height > 0)
            currentProfile_.height = sig.height;

        if (sig.bit_depth >= 10)
            currentProfile_.format = GCAP_FMT_Y210;

        const QString signalFps = sig.frame_rate_name[0]
                                      ? QString::fromLatin1(sig.frame_rate_name)
                                      : QStringLiteral("--");
        MainWindow::postLog(QStringLiteral("[GVFG] started deviceIndex=%1 %2x%3 fps=%4 format=%5 bitdepth=%6")
                                .arg(deviceIndex_)
                                .arg(currentProfile_.width)
                                .arg(currentProfile_.height)
                                .arg(signalFps)
                                .arg(QString::fromLatin1(sig.video_format))
                                .arg(sig.bit_depth));
        updateRuntimeStatusUi();
        refreshCaptureInfoFromSdkAndRuntime(false);
        refreshDisplayInfoFromCurrentState();
        return;
    }
#endif

    gcap_set_backend(backend);
    gcap_set_d3d_adapter(-1);

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
    pv.enable_preview = (hwnd != nullptr) ? 1 : 0;
    pv.use_fp16_pipeline = 1;
    pv.swapchain_10bit = selectedPreviewBitDepthMode();
    appendDebugLog(QStringLiteral("[Preview] requested bit depth: %1 mode=%2")
                       .arg(selectedPreviewBitDepthText())
                       .arg(pv.swapchain_10bit));

    st = gcap_set_preview(h_, &pv);
    if (st != GCAP_OK)
    {
        showCaptureErrorAndClose(QStringLiteral("set preview"), st, "gcap_set_preview");
        return;
    }

    const int selectedPixfmt = ui->comboPixelFormat ? ui->comboPixelFormat->currentData().toInt() : -1;
    currentProfile_ = {};
    currentProfile_.mode = (selectedPixfmt >= 0) ? GCAP_PROFILE_CUSTOM : GCAP_PROFILE_DEVICE_DEFAULT;
    currentProfile_.width = 0;
    currentProfile_.height = 0;
    currentProfile_.fps_num = 0;
    currentProfile_.fps_den = 0;
    // Keep format enum valid across all backends; Auto is expressed by mode, not by an invalid format value.
    currentProfile_.format = static_cast<gcap_pixfmt_t>((selectedPixfmt >= 0) ? selectedPixfmt : GCAP_FMT_NV12);

    st = gcap_set_profile(h_, &currentProfile_);
    if (st != GCAP_OK)
    {
        showCaptureErrorAndClose(QStringLiteral("set profile"), st, "gcap_set_profile");
        return;
    }

    const QString prefText = ui->comboPixelFormat ? ui->comboPixelFormat->currentText() : QStringLiteral("Format: Auto");
    appendDebugLog(QStringLiteral("[Start] user negotiation preference=%1 profile.format=%2")
                       .arg(prefText)
                       .arg(static_cast<int>(currentProfile_.format)));

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

    startPreviewAudio();
    updateRuntimeStatusUi();
    refreshCaptureInfoFromSdkAndRuntime(false);
    refreshDisplayInfoFromCurrentState();
}

void MainWindow::onStop()
{
    if (!h_
#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
        && !usingGvfg_
#endif
    )
        return;

    stopRecordingSession(false);
    stopPreviewAudio();
    if (h_)
        gcap_stop(h_);
    closeCaptureSession();
    clearPreviewSurface();
    lastVideoCallbackPtsNs_ = 0;
    lastPacketCallbackPtsNs_ = 0;
    framePacketLogCount_ = 0;
    invalidateDeviceCapabilityCache();
    refreshCaptureInfoFromSdkAndRuntime(false);
    refreshDisplayInfoFromCurrentState();
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
#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
    if (usingGvfg_)
    {
        if (!gvfg_ || !gvfg_->isRunning())
        {
            QMessageBox::warning(this, QStringLiteral("Record"),
                                 QStringLiteral("Please press Start to begin GVFG capture before recording."));
            return;
        }

        if (recording_)
        {
            stopRecordingSession(true);
            return;
        }

        const QDateTime now = QDateTime::currentDateTime();
        const QString fullPath = buildRecordingPath(now);
        const int fpsNum = currentProfile_.fps_num > 0 ? currentProfile_.fps_num : 30;
        const int fpsDen = currentProfile_.fps_den > 0 ? currentProfile_.fps_den : 1;
        const gvfg_runtime_info_t rt = gvfg_->runtimeInfo();
        const gcap_pixfmt_t recFmt = gvfgRecordingFormatFromRuntime(rt, currentProfile_.format);
        const bool hevc = recFmt == GCAP_FMT_P010 || recFmt == GCAP_FMT_Y210;
        const int bitrateKbps = hevc ? 12000 : 8000;

        QString error;
        if (!gvfg_->startRecording(fullPath, fpsNum, fpsDen, bitrateKbps, &error))
        {
            QMessageBox::warning(this, QStringLiteral("Record"),
                                 QStringLiteral("Start GVFG recording failed: %1").arg(error));
            return;
        }

        recording_ = true;
        ui->btnRecord->setText(QStringLiteral("Stop Rec"));
        recordStartTime_ = now;
        recordPath_ = fullPath;
        recordEncoderName_ = hevc
                                 ? QStringLiteral("FFmpeg HEVC / H.265 via Media Foundation")
                                 : QStringLiteral("FFmpeg H.264 / AVC via Media Foundation");

        if (ui->statusbar)
        {
            const int srcW = lastFrameWidth_ > 0 ? lastFrameWidth_ : currentProfile_.width;
            const int srcH = lastFrameHeight_ > 0 ? lastFrameHeight_ : currentProfile_.height;
            ui->statusbar->showMessage(QStringLiteral("Record mode: GVFG + FFmpeg MP4 | Encoder: %1 | %2 x %3")
                                           .arg(recordEncoderName_)
                                           .arg(srcW)
                                           .arg(srcH));
        }
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

    const int backend = ui->comboBackend ? ui->comboBackend->currentData().toInt() : GCAP_BACKEND_DSHOW;
    const gcap_pixfmt_t recFmt = effectiveRecordingFormat(h_, currentProfile_.format);
    const QString modeLabel = buildRecordModeLabel(h_, backend);
    recordEncoderName_ = buildRecordEncoderLabel(h_, backend, recFmt);

    if (ui->statusbar)
    {
        const int srcW = lastFrameWidth_ > 0 ? lastFrameWidth_ : currentProfile_.width;
        const int srcH = lastFrameHeight_ > 0 ? lastFrameHeight_ : currentProfile_.height;
        const QString sb = QStringLiteral("Record mode: %1 | Encoder: %2 | %3 x %4")
                               .arg(modeLabel)
                               .arg(recordEncoderName_)
                               .arg(srcW)
                               .arg(srcH);
        ui->statusbar->showMessage(sb);
    }
}
