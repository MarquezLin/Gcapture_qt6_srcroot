#include "mainwindow.h"
#include "dshow/directshow_source.h"

#include "./ui_mainwindow.h"

#include <QFileInfo>
#include <QDateTime>
#include <QMessageBox>

namespace
{
gcap_pixfmt_t effectiveRecordingFormat(const DirectShowSource &source, gcap_pixfmt_t fallback)
{
    gcap_runtime_info_t runtime{};
    if (source.runtimeInfo(&runtime) == GCAP_OK &&
        gcap_pixfmt_bit_depth(runtime.negotiated.pixfmt) > 0)
        return runtime.negotiated.pixfmt;
    return fallback;
}

QString recordEncoderLabel(const DirectShowSource &source, gcap_pixfmt_t fallbackFormat)
{
    gcap_recording_info_t info{};
    if (source.recordingInfo(&info) == GCAP_OK && info.encoder_name[0])
        return QString::fromUtf8(info.encoder_name);

    const QString inputFormat = QString::fromUtf8(gcap_pixfmt_name(fallbackFormat));
    if (fallbackFormat == GCAP_FMT_P010)
        return QStringLiteral("FFmpeg HEVC / H.265 (input P010 10-bit, video-only)");
    if (fallbackFormat == GCAP_FMT_Y210)
        return QStringLiteral("FFmpeg HEVC / H.265 (input Y210 10-bit 4:2:2, video-only)");
    return QStringLiteral("FFmpeg H.264 / AVC (input %1, video-only)").arg(inputFormat);
}

QString recordModeLabel(const DirectShowSource &source)
{
    gcap_recording_info_t info{};
    if (source.recordingInfo(&info) == GCAP_OK && info.mode_name[0])
        return QString::fromUtf8(info.mode_name);
    return QString::fromUtf8(gcap_recording_mode_name(GCAP_BACKEND_DSHOW));
}
} // namespace

bool MainWindow::startDirectShowCapture(void *previewHwnd)
{
    (void)procAmpSettings_;

    DirectShowStartConfig config;
    config.deviceIndex = deviceIndex_;
    config.videoCallback = &MainWindow::s_vcb;
    config.eventCallback = &MainWindow::s_ecb;
    config.packetCallback = &MainWindow::s_pcb;
    config.callbackUser = this;
    config.preview.hwnd = previewHwnd;
    config.preview.enable_preview = previewHwnd ? 1 : 0;
    config.preview.use_fp16_pipeline = 1;
    config.preview.swapchain_10bit = selectedPreviewBitDepthMode();
    appendDebugLog(QStringLiteral("[Preview] requested bit depth: %1 mode=%2")
                       .arg(selectedPreviewBitDepthText())
                       .arg(config.preview.swapchain_10bit));

    const int selectedFormat = ui->comboPixelFormat ? ui->comboPixelFormat->currentData().toInt() : -1;
    currentProfile_ = {};
    currentProfile_.mode = selectedFormat >= 0 ? GCAP_PROFILE_CUSTOM : GCAP_PROFILE_DEVICE_DEFAULT;
    currentProfile_.format = static_cast<gcap_pixfmt_t>(selectedFormat >= 0 ? selectedFormat : GCAP_FMT_NV12);
    config.profile = currentProfile_;

    const QString preference = ui->comboPixelFormat ? ui->comboPixelFormat->currentText() : QStringLiteral("Format: Auto");
    appendDebugLog(QStringLiteral("[Start] user negotiation preference=%1 profile.format=%2")
                       .arg(preference)
                       .arg(static_cast<int>(currentProfile_.format)));

    const DirectShowStartResult result = directShow_->start(config);
    if (result.status != GCAP_OK)
        return showCaptureErrorAndClose(QStringLiteral("start DirectShow"), result.status, result.failedApi);

    appendDebugLog(QStringLiteral("[MainWindow] DirectShow session started"));

    startAudioMonitoring();
    updateRuntimeStatusUi();
    refreshCaptureInfoFromSdkAndRuntime(false);
    refreshDisplayInfoFromCurrentState();
    return true;
}

void MainWindow::toggleDirectShowRecording()
{
    if (!directShow_ || !directShow_->isRunning())
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

    const QString fullPath = chooseRecordingPath(QDateTime::currentDateTime());
    if (fullPath.isEmpty())
        return;
    const QDateTime now = QDateTime::currentDateTime();
    const gcap_status_t status = directShow_->startRecording(fullPath.toUtf8().constData());
    if (status != GCAP_OK)
    {
        QMessageBox::warning(this, QStringLiteral("Record"),
                             QStringLiteral("Start recording failed: %1").arg(QString::fromUtf8(gcap_strerror(status))));
        return;
    }

    recording_ = true;
    ui->btnRecord->setText(QStringLiteral("Stop Rec"));
    recordStartTime_ = now;
    recordPath_ = fullPath;
    recordEncoderName_ = recordEncoderLabel(*directShow_, effectiveRecordingFormat(*directShow_, currentProfile_.format));

    if (ui->statusbar)
    {
        const int width = runtime_.width() > 0 ? runtime_.width() : currentProfile_.width;
        const int height = runtime_.height() > 0 ? runtime_.height() : currentProfile_.height;
        ui->statusbar->showMessage(QStringLiteral("Record mode: %1 | Encoder: %2 | %3 x %4")
                                       .arg(recordModeLabel(*directShow_))
                                       .arg(recordEncoderName_)
                                       .arg(width)
                                       .arg(height));
    }
}

void MainWindow::stopDirectShowRecording(bool showSummary)
{
    directShow_->stopRecording();
    recording_ = false;
    ui->btnRecord->setText(QStringLiteral("Record"));
    if (!showSummary || recordPath_.isEmpty() || !recordStartTime_.isValid())
        return;

    QFileInfo file(recordPath_);
    const qint64 sizeBytes = file.size();
    const double seconds = recordStartTime_.msecsTo(QDateTime::currentDateTime()) / 1000.0;
    const double bitrateKbps = seconds > 0.0 ? (sizeBytes * 8.0 / 1000.0) / seconds : 0.0;
    const double captureFps = runtime_.fps() > 0.0
                                  ? runtime_.fps()
                                  : (currentProfile_.fps_den
                                         ? static_cast<double>(currentProfile_.fps_num) / currentProfile_.fps_den
                                         : static_cast<double>(currentProfile_.fps_num));
    const int width = runtime_.width() > 0 ? runtime_.width() : currentProfile_.width;
    const int height = runtime_.height() > 0 ? runtime_.height() : currentProfile_.height;
    gcap_recording_info_t recordingInfo{};
    const bool hasInfo = directShow_->recordingInfo(&recordingInfo) == GCAP_OK;
    gcap_recording_stats_t stats{};
    const bool hasStats = directShow_->recordingStats(&stats) == GCAP_OK;
    const QString mode = hasInfo && recordingInfo.mode_name[0]
                             ? QString::fromUtf8(recordingInfo.mode_name)
                             : recordModeLabel(*directShow_);
    const QString encoder = !recordEncoderName_.isEmpty()
                                ? recordEncoderName_
                                : (hasInfo && recordingInfo.encoder_name[0]
                                       ? QString::fromUtf8(recordingInfo.encoder_name)
                                       : recordEncoderLabel(*directShow_, effectiveRecordingFormat(*directShow_, currentProfile_.format)));
    double outputFps = currentProfile_.fps_den
                           ? static_cast<double>(currentProfile_.fps_num) / currentProfile_.fps_den
                           : static_cast<double>(currentProfile_.fps_num);
    if (hasInfo && recordingInfo.output_fps_num > 0 && recordingInfo.output_fps_den > 0)
        outputFps = static_cast<double>(recordingInfo.output_fps_num) / recordingInfo.output_fps_den;

    QString summary = QStringLiteral(
                          "Record done\nfile:%1\nsize:%2 MB\nActual resolution:%3 x %4\n"
                          "Capture FPS:%5\nRecord output FPS:%6\nrecord mode:%7\nencoder:%8\n"
                          "file avg bit rate:%9 kbps")
                          .arg(recordPath_)
                          .arg(QString::number(sizeBytes / (1024.0 * 1024.0), 'f', 2))
                          .arg(width)
                          .arg(height)
                          .arg(QString::number(captureFps, 'f', 2))
                          .arg(QString::number(outputFps, 'f', 2))
                          .arg(mode)
                          .arg(encoder)
                          .arg(QString::number(bitrateKbps, 'f', 1));
    if (hasStats)
    {
        summary += QStringLiteral("\nframes written:%1\nframes dropped:%2\ninput frames:%3")
                       .arg(QString::number(static_cast<qulonglong>(stats.frames_written)))
                       .arg(QString::number(static_cast<qulonglong>(stats.frames_dropped)))
                       .arg(QString::number(static_cast<qulonglong>(stats.input_frames)));
    }
    QMessageBox::information(this, QStringLiteral("Record"), summary);
    if (ui->statusbar)
        ui->statusbar->showMessage(QStringLiteral("Record done: %1 | file:%2 | avg %3 kbps")
                                       .arg(mode)
                                       .arg(file.fileName())
                                       .arg(QString::number(bitrateKbps, 'f', 1)));
}

void MainWindow::startAudioMonitoring()
{
    stopAudioMonitoring();
    if (ui->checkAudioMonitoring && !ui->checkAudioMonitoring->isChecked())
    {
        setAudioMonitoringStatus(QStringLiteral("Disabled"), QStringLiteral("Audio monitoring is disabled by the user."));
        MainWindow::postLog(QStringLiteral("[AudioMonitoring] disabled by user"));
        return;
    }

    const QString videoDeviceName = currentDeviceText();
    if (videoDeviceName.isEmpty())
    {
        setAudioMonitoringStatus(QStringLiteral("No Video Source"));
        return;
    }

    gcap_audio_device_t audio{};
    const QByteArray videoNameUtf8 = videoDeviceName.toUtf8();
    if (!gcap_audio_find_device_for_capture(videoNameUtf8.constData(), &audio))
    {
        gcap_audio_device_t dshowAudio{};
        const int status = gcap_start_dshow_audio_monitoring(videoNameUtf8.constData(), &dshowAudio);
        if (status == GCAP_OK)
        {
            audioMonitoringActive_ = true;
            const QString format = dshowAudio.sample_rate > 0 && dshowAudio.channels > 0
                                       ? QStringLiteral("%1 Hz, %2 channels, %3-bit PCM")
                                             .arg(dshowAudio.sample_rate)
                                             .arg(dshowAudio.channels)
                                             .arg(dshowAudio.bits_per_sample)
                                       : QStringLiteral("PCM format reported by DirectShow");
            setAudioMonitoringStatus(QStringLiteral("Monitoring (DirectShow)"),
                                     QStringLiteral("%1\n%2").arg(QString::fromUtf8(dshowAudio.name), format));
            MainWindow::postLog(QStringLiteral("[AudioMonitoring][DShow] started filter=%1 (%2)")
                                    .arg(QString::fromUtf8(dshowAudio.name), format));
            return;
        }
        setAudioMonitoringStatus(QStringLiteral("Start Failed"),
                                 QStringLiteral("DirectShow audio filter matching/start failed for %1 (status %2).")
                                     .arg(videoDeviceName)
                                     .arg(status));
        MainWindow::postLog(QStringLiteral("[AudioMonitoring][DShow] matching/start failed for video device=%1 status=%2")
                                .arg(videoDeviceName)
                                .arg(status),
                            true);
        return;
    }

    gcap_audio_monitor_config_t config{};
    config.device_id = audio.id;
    config.sample_rate = audio.sample_rate;
    config.channels = audio.channels;
    const int status = gcap_start_audio_monitoring(&config);
    if (status != GCAP_OK)
    {
        setAudioMonitoringStatus(QStringLiteral("Start Failed"),
                                 QStringLiteral("%1 (status %2)").arg(QString::fromUtf8(audio.name)).arg(status));
        MainWindow::postLog(QStringLiteral("[AudioMonitoring] start failed for endpoint=%1 status=%2")
                                .arg(QString::fromUtf8(audio.name))
                                .arg(status),
                            true);
        return;
    }
    audioMonitoringActive_ = true;
    setAudioMonitoringStatus(QStringLiteral("Monitoring (WASAPI)"),
                             QStringLiteral("%1\n%2 Hz, %3 channels")
                                 .arg(QString::fromUtf8(audio.name))
                                 .arg(audio.sample_rate)
                                 .arg(audio.channels));
    MainWindow::postLog(QStringLiteral("[AudioMonitoring] started endpoint=%1 (%2 Hz, %3 ch)")
                            .arg(QString::fromUtf8(audio.name))
                            .arg(audio.sample_rate)
                            .arg(audio.channels));
}

void MainWindow::stopAudioMonitoring()
{
    const bool wasActive = audioMonitoringActive_;
    gcap_stop_audio_monitoring();
    audioMonitoringActive_ = false;
    if (wasActive)
        MainWindow::postLog(QStringLiteral("[AudioMonitoring] stopped"));
    if (ui && ui->checkAudioMonitoring && ui->checkAudioMonitoring->isChecked())
        setAudioMonitoringStatus(QStringLiteral("Standby"), QStringLiteral("Audio monitoring is enabled but not running."));
    else
        setAudioMonitoringStatus(QStringLiteral("Disabled"), QStringLiteral("Audio monitoring is disabled by the user."));
}

void MainWindow::refreshDirectShowCaptureInfo(bool throttleDeviceProps)
{
    if (!directShow_ || !directShow_->isOpen())
        return;

    captureInfo_.sdkVersion.clear();
    captureInfo_.backendLibraryVersion.clear();
    captureInfo_.devicePropertiesApplicable = true;

    gcap_signal_status_t signal{};
    if (directShow_->signalStatus(&signal) == GCAP_OK)
        captureInfo_.signal = signal;

    gcap_runtime_info_t runtime{};
    if (directShow_->runtimeInfo(&runtime) == GCAP_OK)
    {
        captureInfo_.signalProbe = runtime.signal_probe;
        captureInfo_.negotiated = runtime.negotiated;
        captureInfo_.backendName = QString::fromUtf8(runtime.backend_name);
        captureInfo_.frameSource = QString::fromUtf8(runtime.frame_source);
        captureInfo_.pathName = QString::fromUtf8(runtime.path_name);
        captureInfo_.captureFormat = runtime.negotiated_desc[0]
                                         ? QString::fromUtf8(runtime.negotiated_desc)
                                         : QString::fromUtf8(runtime.source_format);
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (throttleDeviceProps && nowMs - lastPropsQueryMs_ <= 1000)
        return;
    lastPropsQueryMs_ = nowMs;

    gcap_device_props_t props{};
    if (directShow_->deviceProperties(&props) == GCAP_OK)
    {
        captureInfo_.driverVersion = QString::fromUtf8(props.driver_version);
        captureInfo_.firmwareVersion = QString::fromUtf8(props.firmware_version);
        captureInfo_.serialNumber = QString::fromUtf8(props.serial_number);
    }
}

void MainWindow::refreshDirectShowDisplayPipeline()
{
    displayInfo_.pipe.path = DisplayOutputInfo::Pipeline::Path::DirectShow;
    if (!directShow_ || !directShow_->isOpen())
        return;

    gcap_runtime_info_t runtime{};
    if (directShow_->runtimeInfo(&runtime) == GCAP_OK)
    {
        displayInfo_.pipe.adapterName = QString::fromUtf8(runtime.preview_adapter_name);
        displayInfo_.pipe.adapterIndex = runtime.preview_adapter_index;
        displayInfo_.pipe.renderFormat = QString::fromUtf8(runtime.render_format);
    }
}
