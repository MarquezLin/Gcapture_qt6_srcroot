#include "mainwindow.h"
#include "backend/capture_source.h"
#include "backend/capture_backend_catalog.h"
#include "dshow/directshow_source.h"
#include "previewwindow.h"
#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
#include "gvfg/gvfg_source.h"
#endif

#include "./ui_mainwindow.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFileDialog>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QUrl>
void MainWindow::resetRuntimeTracking()
{
    runtime_.reset();
    lastGvfgRawStateKey_.clear();
    initialPreviewSizeApplied_ = false;
    lastFrameImage_ = QImage();
    rawSnapshot_.reset();
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
    stopAudioMonitoring();
    if (directShow_ && directShow_->isOpen())
        directShow_->close();
#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
    if (gvfg_ && gvfg_->isOpen())
        gvfg_->close();
#endif
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

    CaptureSource *source = activeCaptureSource();
    if (!source)
        source = selectedCaptureSource();
#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
    if (source && source->backend() == CaptureBackend::Gvfg)
    {
        stopGvfgRecording(showSummary);
        return;
    }
#endif

    if (source && source->backend() == CaptureBackend::DirectShow)
        stopDirectShowRecording(showSummary);
}
QString MainWindow::chooseRecordingPath(const QDateTime &suggestedTime)
{
    QString baseDir = QCoreApplication::applicationDirPath() + QStringLiteral("/recordings");
    if (!recordPath_.isEmpty())
        baseDir = QFileInfo(recordPath_).absolutePath();
    QDir().mkpath(baseDir);

    const QString suggestedName = QStringLiteral("capture_%1.mp4")
                                      .arg(suggestedTime.toString(QStringLiteral("yyyyMMdd_HHmmss")));
    QString path = QFileDialog::getSaveFileName(
        this,
        tr("Choose Recording File"),
        QDir(baseDir).filePath(suggestedName),
        tr("MP4 Video (*.mp4)"));
    if (path.isEmpty())
        return {};
    if (!path.endsWith(QStringLiteral(".mp4"), Qt::CaseInsensitive))
        path += QStringLiteral(".mp4");
    return QDir::toNativeSeparators(path);
}

void MainWindow::setAudioMonitoringStatus(const QString &status, const QString &detail)
{
    if (!ui || !ui->metricValueAudio)
        return;

    ui->metricValueAudio->setText(status);
    ui->metricValueAudio->setToolTip(detail);
}

void MainWindow::onStart()
{
    if (activeCaptureSource())
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

    CaptureSource *source = selectedCaptureSource();
    if (!source)
        return;
#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
    if (source->backend() == CaptureBackend::Gvfg)
    {
        startGvfgCapture(hwnd);
        return;
    }
#endif

    startDirectShowCapture(hwnd);
}

void MainWindow::onStop()
{
    CaptureSource *source = activeCaptureSource();
    if (!source)
        return;

    stopRecordingSession(false);
    stopAudioMonitoring();
    source->stop();
    closeCaptureSession();
    clearPreviewSurface();
    runtime_.stopCallbacks();
    invalidateDeviceCapabilityCache();
    refreshCaptureInfoFromSdkAndRuntime(false);
    refreshDisplayInfoFromCurrentState();
    updateRuntimeStatusUi();
}

void MainWindow::onOpenRecordFolder()
{
    QString baseDir = recordPath_.isEmpty()
                          ? QCoreApplication::applicationDirPath() + QStringLiteral("/recordings")
                          : QFileInfo(recordPath_).absolutePath();

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
    CaptureSource *source = activeCaptureSource();
    if (!source)
        source = selectedCaptureSource();
#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
    if (source && source->backend() == CaptureBackend::Gvfg)
    {
        toggleGvfgRecording();
        return;
    }
#endif

    if (source && source->backend() == CaptureBackend::DirectShow)
        toggleDirectShowRecording();
}

void MainWindow::setupBackendControls()
{
    if (ui->comboRecordingSegment && ui->comboRecordingSegment->count() == 0)
    {
        ui->comboRecordingSegment->addItem(QStringLiteral("5 minutes"), 5 * 60);
        ui->comboRecordingSegment->addItem(QStringLiteral("10 minutes"), 10 * 60);
        ui->comboRecordingSegment->setCurrentIndex(1);
    }

    if (!ui->comboBackend)
        return;

    const QSignalBlocker blocker(ui->comboBackend);
    ui->comboBackend->clear();
    for (const auto &backend : backendCatalogs_)
        ui->comboBackend->addItem(backend->displayName(), captureBackendUiValue(backend->backend()));

    int defaultIndex = ui->comboBackend->findData(captureBackendUiValue(CaptureBackend::Gvfg));
    if (defaultIndex < 0)
        defaultIndex = ui->comboBackend->findData(captureBackendUiValue(CaptureBackend::DirectShow));
    ui->comboBackend->setCurrentIndex(defaultIndex >= 0 ? defaultIndex : 0);

    const bool gvfgSelected = selectedBackend() == CaptureBackend::Gvfg;
    if (ui->checkZeroCopy)
        ui->checkZeroCopy->setVisible(gvfgSelected);
    if (ui->labelGvfgVolume)
        ui->labelGvfgVolume->setVisible(gvfgSelected);
    if (ui->sliderGvfgVolume)
    {
        ui->sliderGvfgVolume->setVisible(gvfgSelected);
        ui->sliderGvfgVolume->setEnabled(gvfgSelected && ui->checkAudioMonitoring &&
                                         ui->checkAudioMonitoring->isChecked());
    }

    refreshPixelFormatOptions(false);
}

uint32_t MainWindow::recordingSegmentSeconds() const
{
    if (!ui || !ui->comboRecordingSegment)
        return 10 * 60;
    const int seconds = ui->comboRecordingSegment->currentData().toInt();
    return seconds > 0 ? static_cast<uint32_t>(seconds) : 10U * 60U;
}

void MainWindow::setRecordingControlsEnabled(bool enabled)
{
    if (ui && ui->comboRecordingSegment)
        ui->comboRecordingSegment->setEnabled(enabled);
}

CaptureBackend MainWindow::selectedBackend() const
{
    if (!ui || !ui->comboBackend)
        return CaptureBackend::DirectShow;
    return captureBackendFromUiValue(ui->comboBackend->currentData().toInt());
}

CaptureBackendCatalog *MainWindow::backendCatalog(CaptureBackend selected) const
{
    for (const auto &backend : backendCatalogs_)
    {
        if (backend->backend() == selected)
            return backend.get();
    }
    return nullptr;
}

CaptureSource *MainWindow::selectedCaptureSource() const
{
#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
    if (selectedBackend() == CaptureBackend::Gvfg)
        return gvfg_;
#endif
    return directShow_.get();
}

CaptureSource *MainWindow::activeCaptureSource() const
{
#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
    if (gvfg_ && (gvfg_->isRunning() || gvfg_->isOpen()))
        return gvfg_;
#endif
    if (directShow_ && (directShow_->isRunning() || directShow_->isOpen()))
        return directShow_.get();
    return nullptr;
}

void MainWindow::notifyPixelFormatEnumerationFailure(CaptureBackend backend)
{
    if (!ui->comboBackend)
        return;

    QString title = QStringLiteral("Pixel Format");
    QString message;
    bool showDialog = true;
    switch (backend)
    {
    case CaptureBackend::DirectShow:
        message = QStringLiteral("DirectShow format enumeration failed; keeping Format: Auto.");
        showDialog = false;
        break;
    default:
        return;
    }

    const QString warningKey = QStringLiteral("%1:%2").arg(deviceIndex_).arg(captureBackendUiValue(backend));
    if (lastPixelFormatWarningKey_ == warningKey)
        return;

    lastPixelFormatWarningKey_ = warningKey;
    if (showDialog)
    {
        QMessageBox::warning(this, title, message);
    }
    else
    {
        MainWindow::postLog(QStringLiteral("[PixelFormat] %1").arg(message), true);
        if (ui->statusbar)
            ui->statusbar->showMessage(message, 6000);
    }
}

void MainWindow::refreshPixelFormatOptions(bool showFailurePrompt)
{
    if (!ui->comboPixelFormat)
        return;

    const QSignalBlocker blocker(ui->comboPixelFormat);
    const int previousData = ui->comboPixelFormat->currentData().toInt();
    const CaptureBackend backend = selectedBackend();
    ui->comboPixelFormat->clear();

    CaptureBackendCatalog *catalog = backendCatalog(backend);
    if (!catalog)
        return;

    const CaptureBackendFormats formats = catalog->formats(deviceIndex_);
    for (const auto &format : formats.items)
        ui->comboPixelFormat->addItem(format.label, format.value);
    ui->comboPixelFormat->setToolTip(catalog->formatToolTip());

#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
    ui->comboPixelFormat->setEnabled(!(backend == CaptureBackend::Gvfg && gvfg_ && gvfg_->isRunning()));
#else
    ui->comboPixelFormat->setEnabled(true);
#endif
    if (formats.querySucceeded)
        lastPixelFormatWarningKey_.clear();
    else if (showFailurePrompt && deviceIndex_ >= 0)
        notifyPixelFormatEnumerationFailure(backend);

    const int restoreIndex = ui->comboPixelFormat->findData(previousData);
    ui->comboPixelFormat->setCurrentIndex(restoreIndex >= 0 ? restoreIndex : 0);
}

void MainWindow::initializeDeviceList()
{
    if (!ui->comboDevice)
        return;

    const CaptureBackend backend = selectedBackend();
    const QString previousDeviceName = ui->comboDevice->currentText();

    const QSignalBlocker blocker(ui->comboDevice);
    ui->comboDevice->clear();

    if (CaptureBackendCatalog *catalog = backendCatalog(backend))
    {
        const QList<CaptureBackendDevice> devices = catalog->devices();
        for (const auto &device : devices)
            ui->comboDevice->addItem(device.name, device.index);
    }

    int restoreIndex = -1;
    if (!previousDeviceName.isEmpty())
        restoreIndex = ui->comboDevice->findText(previousDeviceName);
    if (restoreIndex < 0 && ui->comboDevice->count() > 0)
        restoreIndex = 0;

    if (restoreIndex >= 0)
    {
        ui->comboDevice->setCurrentIndex(restoreIndex);
        deviceIndex_ = ui->comboDevice->itemData(restoreIndex).toInt();
    }
    else
    {
        deviceIndex_ = -1;
    }
    ui->comboDevice->setToolTip(ui->comboDevice->currentText());

    invalidateDeviceCapabilityCache();
    lastPixelFormatWarningKey_.clear();

    MainWindow::postLog(QStringLiteral("[DeviceList] backend=%1 devices=%2 selectedIndex=%3 selectedName=%4")
                            .arg(captureBackendUiValue(backend))
                            .arg(ui->comboDevice->count())
                            .arg(deviceIndex_)
                            .arg(ui->comboDevice->currentText()));

    refreshPixelFormatOptions(true);
#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
    refreshGvfgMonitoring();
#endif
}

#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
void MainWindow::refreshGvfgMonitoring()
{
    if (!gvfg_ || gvfg_->isRunning())
        return;
    // Idle monitoring must not open the device. Opening a GVFG channel can
    // prevent another process from receiving frames even when capture is stopped.
    if (gvfg_->isOpen())
        gvfg_->close();
    if (ui->btnStart)
        ui->btnStart->setEnabled(selectedBackend() == CaptureBackend::Gvfg && deviceIndex_ >= 0);
}
#endif
