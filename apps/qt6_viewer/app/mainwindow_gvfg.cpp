#include "mainwindow.h"
#include "gvfg/gvfg_source.h"

#include "./ui_mainwindow.h"
#include "ffmpeg_video_recorder.h"

#include <QFileInfo>
#include <QMessageBox>
#include <array>
#include <cmath>

#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
namespace
{
void gvfgRecordingRate(double measuredFps, int &fpsNum, int &fpsDen)
{
    struct StandardRate
    {
        int num;
        int den;
    };
    static constexpr std::array<StandardRate, 10> rates{{
        {24000, 1001}, {24, 1}, {25, 1}, {30000, 1001}, {30, 1},
        {50, 1}, {60000, 1001}, {60, 1}, {120000, 1001}, {120, 1},
    }};

    if (!(measuredFps > 0.0) || !std::isfinite(measuredFps))
    {
        fpsNum = 30;
        fpsDen = 1;
        return;
    }

    const StandardRate *best = &rates.front();
    double bestDifference = std::abs(measuredFps - static_cast<double>(best->num) / best->den);
    for (const auto &rate : rates)
    {
        const double difference = std::abs(measuredFps - static_cast<double>(rate.num) / rate.den);
        if (difference < bestDifference)
        {
            best = &rate;
            bestDifference = difference;
        }
    }

    if (bestDifference <= 1.0)
    {
        fpsNum = best->num;
        fpsDen = best->den;
    }
    else
    {
        fpsNum = qMax(1, qRound(measuredFps * 1000.0));
        fpsDen = 1000;
    }
}
} // namespace

bool MainWindow::startGvfgCapture(void *previewHwnd)
{
    if (!gvfg_)
    {
        QMessageBox::warning(this, QStringLiteral("GVFG"), QStringLiteral("GVFG source is not available."));
        return false;
    }

    const bool zeroCopyEnabled = ui->checkZeroCopy && ui->checkZeroCopy->isChecked();
    if (!gvfg_->open(deviceIndex_, zeroCopyEnabled))
    {
        QMessageBox::warning(this, QStringLiteral("GVFG"), QStringLiteral("Unable to open the GVFG device."));
        return false;
    }
    gvfg_->pollEvents();
    if (!gvfg_->refreshSignalStatus())
    {
        QMessageBox::warning(this, QStringLiteral("GVFG"), QStringLiteral("Unable to query the current input signal."));
        gvfg_->close();
        return false;
    }
    if (!gvfg_->signalStatus().connected)
    {
        QMessageBox::information(this, QStringLiteral("GVFG"), QStringLiteral("No locked input signal. Connect a signal and press Start again."));
        gvfg_->close();
        return false;
    }

    const auto requestedFormat = static_cast<gvfg_pixel_format_t>(
        ui->comboPixelFormat ? ui->comboPixelFormat->currentData().toInt() : static_cast<int>(GVFG_PIXFMT_YUY2));
    if (!gvfg_->setVideoFormat(requestedFormat))
    {
        gvfg_->close();
        return false;
    }

    currentProfile_ = {};
    currentProfile_.mode = GCAP_PROFILE_DEVICE_DEFAULT;
    currentProfile_.format = GCAP_FMT_YUY2;
    currentProfile_.fps_num = 0;
    currentProfile_.fps_den = 0;

    const bool audioEnabled = !ui->checkAudioMonitoring || ui->checkAudioMonitoring->isChecked();
    if (!gvfg_->start(previewHwnd, deviceIndex_,
                      zeroCopyEnabled, audioEnabled))
    {
        gvfg_->close();
        clearPreviewSurface();
        if (ui->statusbar)
            ui->statusbar->showMessage(QStringLiteral("GVFG start failed"), 5000);
        return false;
    }

    if (ui->checkZeroCopy)
        ui->checkZeroCopy->setEnabled(false);
    const gvfg_signal_status_t sig = gvfg_->signalStatus();
    if (sig.width > 0)
        currentProfile_.width = sig.width;
    if (sig.height > 0)
        currentProfile_.height = sig.height;
    if (sig.bit_depth >= 10)
        currentProfile_.format = GCAP_FMT_Y210;

    MainWindow::postLog(QStringLiteral("[GVFG] started deviceIndex=%1 channel=%2; waiting for first frame")
                            .arg(deviceIndex_)
                            .arg(GVFG_CHANNEL_0));
    if (gvfg_->audioEnabled())
    {
        const gvfg_audio_format_t audio = gvfg_->audioFormat();
        setAudioMonitoringStatus(QStringLiteral("Monitoring (GVFG WASAPI)"),
                                 QStringLiteral("Windows default audio output via GVFG Audio Playback Helper\n%1 Hz, %2 channels, %3-bit PCM\nA/V synchronization is handled by the helper.")
                                     .arg(audio.sample_rate)
                                     .arg(audio.channels)
                                     .arg(audio.bits_per_sample));
        MainWindow::postLog(QStringLiteral("[GVFG][Audio] WASAPI helper playback requested: %1 Hz, %2 ch, %3-bit PCM; helper A/V sync enabled")
                                .arg(audio.sample_rate)
                                .arg(audio.channels)
                                .arg(audio.bits_per_sample));
    }
    else
    {
        setAudioMonitoringStatus(QStringLiteral("Disabled"),
                                 QStringLiteral("GVFG audio playback is disabled by the user."));
    }

    updateRuntimeStatusUi();
    refreshCaptureInfoFromSdkAndRuntime(false);
    refreshDisplayInfoFromCurrentState();
    return true;
}

void MainWindow::toggleGvfgRecording()
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

    const QString fullPath = chooseRecordingPath(QDateTime::currentDateTime());
    if (fullPath.isEmpty())
        return;
    const QDateTime now = QDateTime::currentDateTime();
    const gvfg_runtime_info_t rt = gvfg_->runtimeInfo();
    const gvfg_signal_status_t signal = gvfg_->signalStatus();
    int fpsNum = 30;
    int fpsDen = 1;
    gvfgRecordingRate(rt.capture_fps, fpsNum, fpsDen);
    currentProfile_.fps_num = fpsNum;
    currentProfile_.fps_den = fpsDen;
    MainWindow::postLog(QStringLiteral("[GVFG][record] source_fps=%1 configured_fps=%2/%3")
                            .arg(rt.capture_fps, 0, 'f', 3)
                            .arg(fpsNum)
                            .arg(fpsDen));

    const int width = signal.width > 0
                          ? signal.width
                          : (runtime_.width() > 0 ? runtime_.width() : currentProfile_.width);
    const int height = signal.height > 0
                           ? signal.height
                           : (runtime_.height() > 0 ? runtime_.height() : currentProfile_.height);
    currentProfile_.width = width;
    currentProfile_.height = height;
    const int bitrateKbps = ffmpegRecommendedBitrateKbps(
        width, height, fpsNum, fpsDen, FfmpegVideoPixelFormat::Nv12);
    const uint32_t segmentSeconds = recordingSegmentSeconds();
    MainWindow::postLog(QStringLiteral("[GVFG][record] segment_limit=%1 s / 2 GiB")
                            .arg(segmentSeconds));

    QString error;
    if (!gvfg_->startRecording(fullPath, fpsNum, fpsDen, bitrateKbps, segmentSeconds, &error))
    {
        QMessageBox::warning(this, QStringLiteral("Record"),
                             QStringLiteral("Start GVFG recording failed: %1").arg(error));
        return;
    }

    recording_ = true;
    setRecordingControlsEnabled(false);
    ui->btnRecord->setText(QStringLiteral("Stop Rec"));
    recordStartTime_ = now;
    recordPath_ = fullPath;
    recordEncoderName_ = gvfg_->audioEnabled()
                             ? QStringLiteral("FFmpeg H.264 + AAC")
                             : QStringLiteral("FFmpeg H.264 / AVC Compatible");

    if (ui->statusbar)
    {
        ui->statusbar->showMessage(QStringLiteral("Record mode: GVFG + FFmpeg MP4 | Encoder: %1 | %2 x %3 | Segment: %4 min / 2 GiB")
                                       .arg(recordEncoderName_)
                                       .arg(width)
                                       .arg(height)
                                       .arg(segmentSeconds / 60));
    }
}

void MainWindow::stopGvfgRecording(bool showSummary)
{
    const gvfg_signal_status_t signal = gvfg_->signalStatus();
    gvfg_->stopRecording();
    const uint64_t framesWritten = gvfg_->recordingFrames();
    const uint64_t framesDropped = gvfg_->recordingDroppedFrames();
    recording_ = false;
    setRecordingControlsEnabled(true);
    ui->btnRecord->setText(QStringLiteral("Record"));
    MainWindow::postLog(QStringLiteral("[GVFG][record] stopped frames_written=%1 frames_dropped=%2")
                            .arg(static_cast<qulonglong>(framesWritten))
                            .arg(static_cast<qulonglong>(framesDropped)),
                        framesDropped > 0);

    if (!showSummary || recordPath_.isEmpty() || !recordStartTime_.isValid())
        return;

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
    const double captureFps = seconds > 0.0 ? double(framesWritten) / seconds : runtime_.fps();
    const int srcW = signal.width > 0
                         ? signal.width
                         : (runtime_.width() > 0 ? runtime_.width() : currentProfile_.width);
    const int srcH = signal.height > 0
                         ? signal.height
                         : (runtime_.height() > 0 ? runtime_.height() : currentProfile_.height);

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

void MainWindow::refreshGvfgCaptureInfo()
{
    captureInfo_.backendName = QStringLiteral("GVFG");
    const char *sdkVersion = gvfg_get_version();
    const char *gigabyteLibVersion = gvfg_get_gigabyte_lib_version();
    captureInfo_.sdkVersion = sdkVersion ? QString::fromLatin1(sdkVersion) : QString();
    captureInfo_.backendLibraryVersion = gigabyteLibVersion ? QString::fromLatin1(gigabyteLibVersion) : QString();
    captureInfo_.driverVersion.clear();
    captureInfo_.firmwareVersion.clear();
    captureInfo_.serialNumber.clear();
    captureInfo_.devicePropertiesApplicable = false;

    if (!gvfg_ || !gvfg_->isRunning())
    {
        captureInfo_.signal = {};
        captureInfo_.signalProbe = {};
        captureInfo_.negotiated = {};
        captureInfo_.frameSource.clear();
        captureInfo_.pathName.clear();
        captureInfo_.captureFormat.clear();
        return;
    }

    const gvfg_runtime_info_t runtime = gvfg_->runtimeInfo();
    const gvfg_signal_status_t signal = gvfg_->signalStatus();

    gcap_signal_status_t fpgaSignal{};
    fpgaSignal.width = signal.width;
    fpgaSignal.height = signal.height;
    fpgaSignal.pixfmt = signal.bit_depth >= 10 ? GCAP_FMT_Y210 : GCAP_FMT_YUY2;
    fpgaSignal.bit_depth = signal.bit_depth;
    fpgaSignal.csp = GCAP_CSP_BT709;
    fpgaSignal.range = GCAP_RANGE_LIMITED;

    if (runtime.capture_fps > 0.0 && std::isfinite(runtime.capture_fps))
    {
        gvfgRecordingRate(runtime.capture_fps, fpgaSignal.fps_num, fpgaSignal.fps_den);
    }

    gcap_signal_status_t dmaBuffer{};
    if (signal.connected && runtime.delivered_frames > 0)
    {
        dmaBuffer.width = signal.width;
        dmaBuffer.height = signal.height;
        dmaBuffer.pixfmt = signal.pixel_format == GVFG_PIXFMT_Y210 ? GCAP_FMT_Y210 : GCAP_FMT_YUY2;
        dmaBuffer.bit_depth = signal.bit_depth;
        dmaBuffer.csp = GCAP_CSP_BT709;
        dmaBuffer.range = GCAP_RANGE_LIMITED;
        dmaBuffer.fps_num = fpgaSignal.fps_num;
        dmaBuffer.fps_den = fpgaSignal.fps_den;
    }

    captureInfo_.signal = fpgaSignal;
    captureInfo_.signalProbe = {};
    captureInfo_.negotiated = dmaBuffer;
    captureInfo_.frameSource = QStringLiteral("GVFG capture frame");
    captureInfo_.pathName = QStringLiteral("FPGA reported signal -> gvfg_read_frame");
    captureInfo_.captureFormat = signal.connected && runtime.delivered_frames > 0
                                     ? QString::fromLatin1(gvfg_pixel_format_name(signal.pixel_format))
                                     : QStringLiteral("--");
}

void MainWindow::refreshGvfgDisplayPipeline()
{
    displayInfo_.pipe.path = DisplayOutputInfo::Pipeline::Path::Gvfg;
    if (!gvfg_ || !gvfg_->isRunning())
        return;

    const gvfg_preview_info_t preview = gvfg_->previewInfo();
    displayInfo_.pipe.adapterName = QString::fromUtf8(preview.adapter_name);
    displayInfo_.pipe.adapterIndex = preview.adapter_index;
    displayInfo_.pipe.renderFormat = preview.active
                                         ? QStringLiteral("gvfg_preview %1x%2 %3 %4bit")
                                               .arg(preview.width)
                                               .arg(preview.height)
                                               .arg(QString::fromUtf8(preview.pixel_format))
                                               .arg(preview.bit_depth)
                                         : QStringLiteral("App-owned render");
}
#endif
