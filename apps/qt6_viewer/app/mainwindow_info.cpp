#include "mainwindow.h"
#include "backend/capture_backend_catalog.h"
#include "dshow/directshow_source.h"
#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
#include "gvfg/gvfg_source.h"
#endif

#include "./ui_mainwindow.h"
#include <QGuiApplication>
#include <QMessageBox>
#include <QScreen>
#include <QWindow>
#include <QLabel>

#include "display_info.h"
#include "displayinfodialog.h"
#include "inputinfodialog.h"

namespace
{
const char *pixelFormatName(int format)
{
    return gcap_pixfmt_name(static_cast<gcap_pixfmt_t>(format));
}

#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
QString gvfgStatusLogLine(const gvfg_runtime_info_t &runtime, const gvfg_signal_status_t &signal)
{
    return QStringLiteral("[GVFG][signal]\n"
                          "  input: connected=%1 channel=%2 width=%3 height=%4 format=%5 bit_depth=%6\n"
                          "  runtime: fps=%7 frames=%8")
        .arg(signal.connected)
        .arg(signal.channel)
        .arg(signal.width)
        .arg(signal.height)
        .arg(QString::fromLatin1(gvfg_pixel_format_name(signal.pixel_format)))
        .arg(signal.bit_depth)
        .arg(runtime.capture_fps > 0.0 ? QString::number(runtime.capture_fps, 'f', 2) : QStringLiteral("--"))
        .arg(QString::number(static_cast<qulonglong>(runtime.delivered_frames)));
}

QString gvfgSignalStateKey(const gvfg_signal_status_t &signal)
{
    return QStringLiteral("%1|%2|%3|%4|%5|%6")
        .arg(signal.connected)
        .arg(signal.channel)
        .arg(signal.width)
        .arg(signal.height)
        .arg(signal.pixel_format)
        .arg(signal.bit_depth);
}
#endif
} // namespace

void MainWindow::ensureDeviceCapabilityCache(int deviceIndex)
{
    const CaptureBackend backend = selectedBackend();

    if (deviceIndex < 0)
    {
        cachedDeviceCapsBackend_.reset();
        cachedDeviceCapsIndex_ = -1;
        cachedSupportedFormats_.clear();
        cachedPropertyPages_.clear();
        return;
    }

    if (cachedDeviceCapsBackend_ == backend && cachedDeviceCapsIndex_ == deviceIndex &&
        (!cachedSupportedFormats_.isEmpty() || !cachedPropertyPages_.isEmpty()))
        return;

    cachedDeviceCapsBackend_ = backend;
    cachedDeviceCapsIndex_ = deviceIndex;
    cachedSupportedFormats_.clear();
    cachedPropertyPages_.clear();

    if (CaptureBackendCatalog *catalog = backendCatalog(backend))
    {
        const CaptureBackendCapabilities capabilities = catalog->capabilities(deviceIndex);
        cachedSupportedFormats_ = capabilities.formats;
        cachedPropertyPages_ = capabilities.propertyPages;
    }
}

void MainWindow::invalidateDeviceCapabilityCache()
{
    cachedDeviceCapsBackend_.reset();
    cachedDeviceCapsIndex_ = -1;
    cachedSupportedFormats_.clear();
    cachedPropertyPages_.clear();
}

void MainWindow::updateCapabilityLabel()
{
    if (!ui || !ui->labelinfo1)
        return;

    ui->labelinfo1->setEnabled(true);
    ui->labelinfo1->setTextInteractionFlags(Qt::TextSelectableByMouse);
    ui->labelinfo1->setWordWrap(false);

#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
    if (selectedBackend() == CaptureBackend::Gvfg)
    {
        QString formatText = QStringLiteral("Hardware Auto");
        int bitDepth = 0;

        if (gvfg_ && gvfg_->isRunning())
        {
            const gvfg_signal_status_t signal = gvfg_->signalStatus();
            if (signal.pixel_format != GVFG_PIXFMT_UNKNOWN)
            {
                formatText = QString::fromLatin1(
                    gvfg_pixel_format_name(signal.pixel_format));
                bitDepth = signal.bit_depth;
            }
        }

        ui->labelinfo1->setText(
            (bitDepth > 0)
                ? tr("GVFG Runtime: %1 (%2-bit)").arg(formatText).arg(bitDepth)
                : tr("GVFG: %1").arg(formatText));
        ui->labelinfo1->setToolTip(
            tr("GVFG uses the native pixel format delivered by the capture hardware."));
        return;
    }
#endif

    // Prefer the active provider's open-graph selectable list. This is the list
    // actually used by SetFormat retry after SDK filtering/policy. For WDM devices
    // such as Blackmagic, a separate capability enumeration can disagree with the
    // active graph and show misleading modes.
    if (directShow_ && directShow_->isOpen())
    {
        gcap_runtime_info_t rt{};
        if (directShow_->runtimeInfo(&rt) == GCAP_OK && rt.selectable_caps_inline[0])
        {
            const QString inlineText = QString::fromUtf8(rt.selectable_caps_inline);
            const QString tooltipText = rt.selectable_caps_tooltip[0]
                                            ? QString::fromUtf8(rt.selectable_caps_tooltip)
                                            : inlineText;
            ui->labelinfo1->setText(tr("Caps(Open): %1").arg(inlineText));
            ui->labelinfo1->setToolTip(tr("Active open-graph selectable modes after SDK filtering/policy:\n%1")
                                           .arg(tooltipText));
            return;
        }
    }

    ensureDeviceCapabilityCache(currentDeviceIndex());

    QStringList display = cachedSupportedFormats_;
    display.removeDuplicates();

    if (display.isEmpty())
    {
        ui->labelinfo1->setText(tr("Caps: --"));
        ui->labelinfo1->setToolTip(tr("No capability list available for the selected backend/device."));
        return;
    }

    const int maxInline = 6;
    QStringList inlineItems = display.mid(0, maxInline);
    QString inlineText = inlineItems.join(QStringLiteral(" | "));
    if (display.size() > maxInline)
        inlineText += tr(" | ... +%1 more").arg(display.size() - maxInline);

    ui->labelinfo1->setText(tr("Caps(Enum): %1").arg(inlineText));
    ui->labelinfo1->setToolTip(tr("Capability list from a separate enumeration query. The active open graph may differ on some drivers:\n%1")
                                   .arg(display.join(QStringLiteral("\n"))));
}

int MainWindow::currentDeviceIndex() const
{
    if (!ui || !ui->comboDevice || ui->comboDevice->currentIndex() < 0)
        return -1;

    bool ok = false;
    const int devIndex = ui->comboDevice->currentData().toInt(&ok);
    return ok ? devIndex : -1;
}

QString MainWindow::currentDeviceText() const
{
    return (ui && ui->comboDevice) ? ui->comboDevice->currentText() : QString();
}

void MainWindow::ensureSignalInfoDialog()
{
    if (signalInfoDialog_)
        return;

    signalInfoDialog_ = new inputinfodialog(this);
    signalInfoDialog_->setWindowTitle(tr("Signal Info"));

    connect(signalInfoDialog_, &inputinfodialog::openPropertyPageRequested,
            this, [this](const QString &pageNameUtf8, bool capturePin)
            {
#ifdef _WIN32
                const int devIndex = currentDeviceIndex();
                MainWindow::postLog(QStringLiteral("[SignalInfo] open property page request: devIndex=%1 page=%2 target=%3")
                                        .arg(devIndex)
                                        .arg(pageNameUtf8)
                                        .arg(capturePin ? QStringLiteral("Capture Pin") : QStringLiteral("Filter")));
                const bool ok = (gcap_open_named_property_page(devIndex,
                                                               pageNameUtf8.toUtf8().constData(),
                                                               capturePin ? 1 : 0) != 0);
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

void MainWindow::ensureDisplayInfoDialog()
{
    if (displayInfoDialog_)
        return;

    displayInfoDialog_ = new DisplayInfoDialog(this);
    displayInfoDialog_->setWindowTitle(tr("Display Info"));
}

void MainWindow::showAndActivateDialog(QWidget *dialog)
{
    if (!dialog)
        return;

    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void MainWindow::refreshCaptureInfoFromSdkAndRuntime(bool throttleDeviceProps)
{
    captureInfo_.deviceName = currentDeviceText();

#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
    if (gvfg_ && gvfg_->isRunning())
        refreshGvfgCaptureInfo();
    else
#endif
        refreshDirectShowCaptureInfo(throttleDeviceProps);

    ensureDeviceCapabilityCache(currentDeviceIndex());
    captureInfo_.supportedFormats = cachedSupportedFormats_;
    captureInfo_.propertyPages = cachedPropertyPages_;
    lastInfoText_ = formatCaptureDeviceInfo(captureInfo_, runtime_.fps());
    updateCapabilityLabel();
}

void MainWindow::refreshDisplayInfoFromFrame(const QImage &img)
{
    displayInfo_.video.size = img.size();
    displayInfo_.video.fps = runtime_.fps();

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
        displayInfo_.desktop = {};
    }

    displayInfo_.color = {};
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

    displayInfo_.pipe = {};
    const CaptureBackend backend = selectedBackend();

#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
    if (backend == CaptureBackend::Gvfg || (gvfg_ && gvfg_->isRunning()))
        refreshGvfgDisplayPipeline();
    else
#endif
        refreshDirectShowDisplayPipeline();
}

void MainWindow::refreshDisplayInfoFromCurrentState()
{
    if (lastFrameImage_.isNull())
    {
        QImage fallback(1, 1, QImage::Format_ARGB32_Premultiplied);
        refreshDisplayInfoFromFrame(fallback);
        displayInfo_.video.size = QSize();
        displayInfo_.video.fps = runtime_.fps();
        return;
    }

    refreshDisplayInfoFromFrame(lastFrameImage_);
}

void MainWindow::refreshSignalInfoDialog()
{
    refreshCaptureInfoFromSdkAndRuntime(false);
    if (!signalInfoDialog_)
        return;

    signalInfoDialog_->setWindowTitle(tr("Signal Info"));
    signalInfoDialog_->setInfoText(lastInfoText_);
    signalInfoDialog_->setPropertyPages(captureInfo_.propertyPages);
}

void MainWindow::refreshDisplayInfoDialog()
{
    refreshDisplayInfoFromCurrentState();
    if (!displayInfoDialog_)
        return;

    displayInfoDialog_->setWindowTitle(tr("Display Info"));
    displayInfoDialog_->setInfoText(formatDisplayOutputInfo(displayInfo_));
}

void MainWindow::onShowInputInfo()
{
#ifdef _WIN32
    const QString deviceText = currentDeviceText();
    const int devIndex = currentDeviceIndex();
    MainWindow::postLog(QStringLiteral("[SignalInfo] opening generic signal info dialog. devIndex=%1 name=%2")
                            .arg(devIndex)
                            .arg(deviceText));
#endif
    ensureSignalInfoDialog();
    refreshSignalInfoDialog();
    showAndActivateDialog(signalInfoDialog_);
}

void MainWindow::onShowDisplayInfo()
{
    ensureDisplayInfoDialog();
    refreshDisplayInfoDialog();
    showAndActivateDialog(displayInfoDialog_);
}

void MainWindow::updateBrandDashboard()
{
    const auto setLabel = [this](const char *name, const QString &text)
    {
        if (QLabel *label = findChild<QLabel *>(QString::fromLatin1(name)))
            label->setText(text);
    };

    const bool active = activeCaptureSource() != nullptr;

    QString backendText = tr("Idle");
    QString renderPathText;
    bool previewActive = false;
    QString signalText = (runtime_.width() > 0 && runtime_.height() > 0)
                             ? QStringLiteral("%1 x %2").arg(runtime_.width()).arg(runtime_.height())
                             : (active ? tr("Active") : tr("Waiting"));
    double dashboardFps = runtime_.fps();
    QString colorText = selectedPreviewBitDepthText();
    uint64_t frameCounter = runtime_.deliveredFrames();
    bool inputConnected = active;

#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
    if (gvfg_ && gvfg_->isRunning())
    {
        const gvfg_runtime_info_t rt = gvfg_->runtimeInfo();
        const gvfg_signal_status_t signal = gvfg_->signalStatus();
        const gvfg_preview_info_t pv = gvfg_->previewInfo();
        inputConnected = signal.connected != 0;
        backendText = QStringLiteral("GVFG");
        previewActive = inputConnected && pv.active != 0;
        renderPathText = previewActive
                             ? QStringLiteral("gvfg_preview %1x%2 %3 %4bit")
                                   .arg(pv.width)
                                   .arg(pv.height)
                                   .arg(QString::fromUtf8(pv.pixel_format))
                                   .arg(pv.bit_depth)
                             : tr("preview inactive");
        frameCounter = rt.delivered_frames;
        if (inputConnected)
        {
            dashboardFps = (rt.capture_fps > 0.0) ? rt.capture_fps : dashboardFps;
            if (signal.width > 0 && signal.height > 0)
            {
                signalText = QStringLiteral("%1 x %2").arg(signal.width).arg(signal.height);
                if (signal.bit_depth > 0)
                    colorText = QStringLiteral("%1 · %2-bit")
                                    .arg(QString::fromLatin1(gvfg_pixel_format_name(signal.pixel_format)))
                                    .arg(signal.bit_depth);
            }
        }
        else
        {
            signalText = tr("Disconnected");
            dashboardFps = 0.0;
            colorText = QStringLiteral("--");
        }
    }
#endif

    if (directShow_ && directShow_->isRunning())
    {
        gcap_runtime_info_t rt{};
        if (directShow_->runtimeInfo(&rt) == GCAP_OK)
        {
            backendText = QString::fromUtf8(rt.backend_name);
            renderPathText = QString::fromUtf8(rt.render_format);
            previewActive = true;
            dashboardFps = (rt.runtime_fps > 0.0) ? rt.runtime_fps : dashboardFps;
            if (rt.negotiated.width > 0 && rt.negotiated.height > 0)
                signalText = QStringLiteral("%1 x %2").arg(rt.negotiated.width).arg(rt.negotiated.height);
            if (rt.render_format[0])
                colorText = QString::fromUtf8(rt.render_format);
        }
    }

    const auto watchdog = runtime_.observeWatchdog(frameCounter, active && inputConnected && frameCounter > 0);
    if (watchdog == CaptureRuntimeTracker::WatchdogTransition::Stalled)
    {
        MainWindow::postLog(QStringLiteral("[Watchdog] frame counter stalled for %1 ms | backend=%2 | counter=%3 | fps=%4 | previewActive=%5 | render=%6")
                                .arg(runtime_.stallTicks() * (runtimeStatusTimer_ ? runtimeStatusTimer_->interval() : 500))
                                .arg(backendText)
                                .arg(QString::number(static_cast<qulonglong>(frameCounter)))
                                .arg(dashboardFps > 0.0 ? QString::number(dashboardFps, 'f', 2) : QStringLiteral("--"))
                                .arg(previewActive ? QStringLiteral("yes") : QStringLiteral("no"))
                                .arg(renderPathText.isEmpty() ? QStringLiteral("--") : renderPathText),
                            true);
    }
    else if (watchdog == CaptureRuntimeTracker::WatchdogTransition::Recovered)
    {
        MainWindow::postLog(QStringLiteral("[Watchdog] frame counter recovered | backend=%1 | counter=%2")
                                .arg(backendText)
                                .arg(QString::number(static_cast<qulonglong>(frameCounter))));
    }

    setLabel("statusBadge", active && !inputConnected
                                ? tr("NO SIGNAL")
                                : (runtime_.stalled() ? tr("FRAME STALL")
                                                            : (active ? tr("CAPTURING") : tr("READY FOR SIGNAL"))));
    setLabel("previewTitle", active && !inputConnected
                                 ? tr("Waiting for Signal")
                                 : (active ? tr("Capture Pipeline Active") : tr("Hardware Signal Pipeline")));
    setLabel("previewHint", active && !inputConnected
                                ? tr("Capture is active and waiting for the input signal.")
                                : (active ? tr("Live render is active in the center preview.")
                                          : tr("No active capture session.")));

    const QString fpsText = (dashboardFps > 0.0)
                                ? QStringLiteral("%1 fps").arg(QString::number(dashboardFps, 'f', 2))
                                : QStringLiteral("-- fps");
    const QString frameSuffix = frameCounter > 0
                                    ? QStringLiteral(" | #%1").arg(QString::number(static_cast<qulonglong>(frameCounter)))
                                    : QString();

    setLabel("metricValue", signalText);
    setLabel("metricValue2", fpsText + frameSuffix);
    setLabel("metricValue3", colorText.isEmpty() ? tr("10-bit Ready") : colorText);
    setLabel("metricValue4", recording_ ? tr("Recording") : tr("Standby"));
}

void MainWindow::updateRuntimeStatusUi()
{
    bool gvfgStreamActive = false;
    bool gvfgSelected = false;
#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
    const CaptureBackend selectedCaptureBackend = selectedBackend();
    gvfgSelected = selectedCaptureBackend == CaptureBackend::Gvfg;
    gvfgStreamActive = selectedCaptureBackend == CaptureBackend::Gvfg && gvfg_ && gvfg_->isRunning();
#endif
    if (ui->comboPixelFormat)
        ui->comboPixelFormat->setEnabled(!gvfgStreamActive);
    // GVFG preview always uses gvfg_preview's native render path. The generic
    // DirectShow preview bit-depth selector is intentionally not applied there.
    if (ui->labelBitDepth)
        ui->labelBitDepth->setVisible(!gvfgSelected);
    if (ui->comboPreviewBitDepth)
        ui->comboPreviewBitDepth->setVisible(!gvfgSelected);
    if (ui->labelGvfgSnapshotFormat)
        ui->labelGvfgSnapshotFormat->setVisible(gvfgSelected);
    if (ui->comboGvfgSnapshotFormat)
        ui->comboGvfgSnapshotFormat->setVisible(gvfgSelected);
    if (ui->labelinfo1)
        ui->labelinfo1->setVisible(!gvfgSelected && !previewFullscreen_);
    if (ui->metricLabel3)
        ui->metricLabel3->setText(gvfgSelected ? tr("Format") : tr("Color Path"));
    if (ui->checkZeroCopy)
    {
#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
        ui->checkZeroCopy->setVisible(gvfgSelected);
        ui->checkZeroCopy->setEnabled(gvfgSelected && gvfg_ && !gvfg_->isRunning());
#else
        ui->checkZeroCopy->setVisible(false);
#endif
    }
    if (ui->checkAudioMonitoring)
        ui->checkAudioMonitoring->setEnabled(!gvfgStreamActive);
    if (ui->labelGvfgVolume)
        ui->labelGvfgVolume->setVisible(gvfgSelected);
    if (ui->sliderGvfgVolume)
    {
        ui->sliderGvfgVolume->setVisible(gvfgSelected);
        ui->sliderGvfgVolume->setEnabled(gvfgSelected && ui->checkAudioMonitoring &&
                                         ui->checkAudioMonitoring->isChecked());
    }

    updateBrandDashboard();

    if (!ui->statusbar)
        return;

    if (!directShow_ || !directShow_->isRunning())
    {
#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
        if (gvfg_ && gvfg_->isRunning())
        {
            const gvfg_runtime_info_t rt = gvfg_->runtimeInfo();
            const gvfg_signal_status_t signal = gvfg_->signalStatus();
            const QString rawStateKey = gvfgSignalStateKey(signal);
            const bool waitingForFirstFrame = rt.delivered_frames == 0 &&
                                              !signal.connected &&
                                              signal.width == 0 &&
                                              signal.height == 0;
            if (!waitingForFirstFrame && lastGvfgRawStateKey_ != rawStateKey)
            {
                MainWindow::postLog(gvfgStatusLogLine(rt, signal));
                lastGvfgRawStateKey_ = rawStateKey;
            }
            const double runtimeFps = (rt.capture_fps > 0.0) ? rt.capture_fps : runtime_.fps();
            const QString transferMode = ui->checkZeroCopy && ui->checkZeroCopy->isChecked()
                                             ? QStringLiteral("Zero-copy")
                                             : QStringLiteral("Copy");
            const QString sb = QStringLiteral("GVFG | %1 fps | frames=%2 | %3")
                                   .arg(runtimeFps > 0.0 ? QString::number(runtimeFps, 'f', 2) : QStringLiteral("--"))
                                   .arg(QString::number(static_cast<qulonglong>(rt.delivered_frames)))
                                   .arg(transferMode);
            if (lastRuntimeStatusText_ != sb)
            {
                ui->statusbar->showMessage(sb);
                lastRuntimeStatusText_ = sb;
            }
            return;
        }
#endif
        const QString sb = QStringLiteral("Idle");
        if (lastRuntimeStatusText_ != sb)
        {
            ui->statusbar->showMessage(sb);
            lastRuntimeStatusText_ = sb;
        }
        return;
    }

    gcap_runtime_info_t rt{};
    if (directShow_->runtimeInfo(&rt) != GCAP_OK)
        return;

    const double negotiatedFps = (rt.negotiated.fps_den > 0) ? (double(rt.negotiated.fps_num) / double(rt.negotiated.fps_den)) : 0.0;
    const double runtimeFps = (rt.runtime_fps > 0.0) ? rt.runtime_fps : runtime_.fps();
    const QString backend = QString::fromUtf8(rt.backend_name);
    const QString source = QString::fromUtf8(rt.frame_source);

    QString negotiatedFmt;
    const QString backendLower = QString::fromUtf8(rt.backend_name).toLower();
    if (backendLower.contains(QStringLiteral("dshow")) && rt.source_format[0])
        negotiatedFmt = QString::fromUtf8(rt.source_format);
    else if (rt.negotiated_desc[0])
        negotiatedFmt = QString::fromUtf8(rt.negotiated_desc);
    else if (rt.source_format[0])
        negotiatedFmt = QString::fromUtf8(rt.source_format);
    else
        negotiatedFmt = QString::fromUtf8(pixelFormatName(rt.negotiated.pixfmt));
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
    QString probeFmt = QString::fromUtf8(pixelFormatName(rt.signal_probe.pixfmt));
    const double probeFps = (rt.signal_probe.fps_den > 0) ? (double(rt.signal_probe.fps_num) / double(rt.signal_probe.fps_den)) : 0.0;

    const QString sb = QStringLiteral("Backend: %1 | Source: %2 | %3 | %4 | AppInternal %5 | Runtime %6fps")
                           .arg(backend)
                           .arg(source)
                           .arg(statusBlock("InputProbe", rt.signal_probe, probeFps, probeFmt))
                           .arg(statusBlock("BackendFmt", rt.negotiated, negotiatedFps, negotiatedFmt))
                           .arg(renderFmt.isEmpty() ? QStringLiteral("--") : renderFmt)
                           .arg(runtimeFps > 0.0 ? QString::number(runtimeFps, 'f', 2) : QStringLiteral("--"));
    if (lastRuntimeStatusText_ != sb)
    {
        ui->statusbar->showMessage(sb);
        lastRuntimeStatusText_ = sb;
    }
}
