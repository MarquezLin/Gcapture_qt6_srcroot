#include "mainwindow.h"
#include "gcapture.h"
#include "backend/capture_backend_catalog.h"
#include "backend/capture_source.h"
#include "dshow/directshow_source.h"
#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
#include "gvfg/gvfg_source.h"
#endif

#include "./ui_mainwindow.h"
#include <QVBoxLayout>
#include <QPushButton>
#include <QComboBox>
#include <QCheckBox>
#include <QSlider>
#include <QMessageBox>
#include <QPixmap>
#include <QWindow>
#include <QScreen>
#include <QGuiApplication>
#include <QCoreApplication>
#include <QDir>
#include "display_info.h"
#include <QDialog>
#include <QPlainTextEdit>
#include <QLabel>
#include <QDesktopServices>
#include <QUrl>
#include <vector>
#include <QSignalBlocker>
#include "edid_reader.h"
#include "rawinspectordialog.h"
#include "procamp.h"
#include "previewwindow.h"
#include "displayinfodialog.h"
#include "inputinfodialog.h"
#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_INTERNAL_TOOLS)
#include "registerdialog.h"
#endif
#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QTimer>
#include <QShortcut>
#include <QKeySequence>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

static MainWindow *g_mainWindow = nullptr;

#ifndef QT6_VIEWER_VERSION
#define QT6_VIEWER_VERSION "1.0.0"
#endif

static void sdkLogCallback(gcap_log_level_t level, const char *message_utf8, void *user)
{
    Q_UNUSED(user);
    const QString prefix = [level]() -> QString
    {
        switch (level)
        {
        case GCAP_LOG_TRACE:
            return QStringLiteral("[SDK][TRACE]");
        case GCAP_LOG_DEBUG:
            return QStringLiteral("[SDK][DEBUG]");
        case GCAP_LOG_INFO:
            return QStringLiteral("[SDK][INFO]");
        case GCAP_LOG_WARN:
            return QStringLiteral("[SDK][WARN]");
        case GCAP_LOG_ERROR:
            return QStringLiteral("[SDK][ERROR]");
        default:
            return QStringLiteral("[SDK]");
        }
    }();
    QString msg = QString::fromUtf8(message_utf8 ? message_utf8 : "").trimmed();
    if (msg.isEmpty())
        return;
    const QString line = prefix + QLatin1Char(' ') + msg;
    if (level >= GCAP_LOG_WARN)
        qWarning().noquote() << line;
    else
        qInfo().noquote() << line;
    if (g_mainWindow)
    {
        QMetaObject::invokeMethod(
            g_mainWindow,
            "appendDebugLog",
            Qt::QueuedConnection,
            Q_ARG(QString, line));
    }
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), ui(new Ui::MainWindow), directShow_(std::make_unique<DirectShowSource>())
{
    ui->setupUi(this);
    resize(1180, 740);
    setMinimumSize(980, 680);
    ui->controlPanel->setMinimumWidth(390);
    ui->controlPanel->setMaximumWidth(450);
    setWindowTitle(QStringLiteral("GIGABYTE Video Capture utility v%1").arg(QString::fromLatin1(QT6_VIEWER_VERSION)));

    if (ui->labelGvfgSnapshotFormat)
        ui->labelGvfgSnapshotFormat->setVisible(false);
    if (ui->comboGvfgSnapshotFormat)
        ui->comboGvfgSnapshotFormat->setVisible(false);
#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
    if (ui->comboGvfgSnapshotFormat)
    {
        ui->comboGvfgSnapshotFormat->addItem(QStringLiteral("Source RAW (YUY2/Y210)"), 0);
        ui->comboGvfgSnapshotFormat->addItem(QStringLiteral("RGB10A2"), GVFG_GPU_OUTPUT_RGB10A2);
        ui->comboGvfgSnapshotFormat->addItem(QStringLiteral("BGRA8"), GVFG_GPU_OUTPUT_BGRA8);
        ui->comboGvfgSnapshotFormat->addItem(QStringLiteral("NV12"), GVFG_GPU_OUTPUT_NV12);
    }
#endif

#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
    gvfg_ = new GvfgSource(this);
    connect(gvfg_, &GvfgSource::frameReady, this, &MainWindow::sigFrame, Qt::QueuedConnection);
    connect(gvfg_, &GvfgSource::eventOccurred, this, [](const QString &message)
            { MainWindow::postLog(QStringLiteral("[GVFG] %1").arg(message)); }, Qt::QueuedConnection);
    connect(gvfg_, &GvfgSource::errorOccurred, this, [this](const QString &message)
            {
                MainWindow::postLog(QStringLiteral("[GVFG] %1").arg(message), true);
                if (message.contains(QStringLiteral("audio"), Qt::CaseInsensitive))
                    setAudioMonitoringStatus(QStringLiteral("Playback Error"), message);
                if (message.startsWith(QStringLiteral("GVFG recording stopped:")))
                {
                    recording_ = false;
                    if (ui->btnRecord)
                        ui->btnRecord->setText(QStringLiteral("Record"));
                    if (ui->statusbar)
                        ui->statusbar->showMessage(message, 8000);
                    QMessageBox::warning(this, QStringLiteral("Record"), message);
                } }, Qt::QueuedConnection);
    connect(gvfg_, &GvfgSource::signalStatusChanged, this, [this](bool connected)
            {
                if (!connected && gvfg_ && gvfg_->isRunning())
                {
                    gvfg_->clearPreview();
                    clearPreviewSurface();
                }
                if (selectedBackend() == CaptureBackend::Gvfg && gvfg_ && !gvfg_->isRunning() && ui->btnStart)
                    ui->btnStart->setEnabled(gvfg_ && gvfg_->isOpen());
                updateRuntimeStatusUi(); });
#endif

    backendCatalogs_.push_back(createDirectShowBackendCatalog());
#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
    backendCatalogs_.push_back(createGvfgBackendCatalog());
#endif

    setupRuntimeStatusTimer();
    setupDebugDock();
    setupProcAmpAction();
    setupRegisterTools();
    setInspectorTools();
    setupBackendControls();
    setupPreviewBitDepthControls();
    initializeDeviceList();
    gcap_set_d3d_adapter(-1);
    setupConnections();

    auto *exitFullscreenShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    exitFullscreenShortcut->setContext(Qt::ApplicationShortcut);
    connect(exitFullscreenShortcut, &QShortcut::activated, this, [this]()
            {
                if (previewFullscreen_)
                    setPreviewFullscreen(false); });

    g_mainWindow = this;
    gcap_set_log_callback(sdkLogCallback, this);
    logStartupInfo();
}

MainWindow::~MainWindow()
{
    stopRecordingSession(false);
    closeCaptureSession();
    if (g_mainWindow == this)
    {
        gcap_set_log_callback(nullptr, nullptr);
        g_mainWindow = nullptr;
    }
    delete rawInspectorDlg_;
    delete previewWindow_;
    delete ui;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (previewFullscreen_)
        setPreviewFullscreen(false);

    if (previewWindow_)
    {
        previewWindow_->close();
    }

    stopRecordingSession(false);
    closeCaptureSession();

    QMainWindow::closeEvent(event);
}

void MainWindow::s_vcb(const gcap_frame_t *f, void *u)
{
    auto *self = static_cast<MainWindow *>(u);
    if (!self || !f || !f->data[0] || f->width <= 0 || f->height <= 0)
        return;
    // Copy the frame before crossing from the callback thread to the UI thread.
    QImage img((const uchar *)f->data[0], f->width, f->height, f->stride[0], QImage::Format_ARGB32);
    const QImage safeImg = img.copy();
    const uint64_t ptsNs = f->pts_ns;
    const uint64_t frameId = f->frame_id;
    const int width = f->width;
    const int height = f->height;

    QMetaObject::invokeMethod(
        self,
        [self, ptsNs, frameId, width, height, safeImg]()
        {
            self->runtime_.onFrame(CaptureRuntimeTracker::Stream::Video, ptsNs, frameId, width, height);
            self->dispatchFrameImage(safeImg);
        },
        Qt::QueuedConnection);
}

void MainWindow::s_pcb(const gcap_frame_packet_t *pkt, void *u)
{
    auto *self = static_cast<MainWindow *>(u);
    if (!self || !pkt)
        return;

    const gcap_frame_packet_t pktCopy = *pkt;
    if (pkt->data[0] && pkt->stride[0] > 0 && pkt->height > 0)
    self->rawSnapshot_.submit(*pkt);

    QMetaObject::invokeMethod(
        self,
        [self, pktCopy]()
        {
            self->runtime_.onFrame(CaptureRuntimeTracker::Stream::Packet, pktCopy.pts_ns, pktCopy.frame_id, pktCopy.width, pktCopy.height);
            self->logFramePacketIfNeeded(pktCopy);
        },
        Qt::QueuedConnection);
}

void MainWindow::s_ecb(gcap_status_t c, const char *m, void *u)
{
    Q_UNUSED(u);

    const QByteArray ba = m ? QByteArray(m) : QByteArray();

    // SDK error text can be UTF-8 or Windows ACP depending on the source.
    QString msg = QString::fromUtf8(ba);
    if (msg.contains(QChar(0xFFFD)))
        msg = QString::fromLocal8Bit(ba);

    // Centralized log sink for UI and file logging.
    MainWindow::postLog(
        QStringLiteral("[gcapture][%1] %2").arg(int(c)).arg(msg),
        c != GCAP_OK);

    // Show a dialog only for errors.
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

    // Convert raw EDID to a hex dump.
    QString hex;
    const QByteArray &raw = res.raw;
    for (int i = 0; i < raw.size(); ++i)
    {
        if (i % 16 == 0)
            hex += QString("\n%1: ").arg(i, 4, 16, QLatin1Char('0')).toUpper();

        hex += QString("%1 ").arg(static_cast<quint8>(raw[i]), 2, 16, QLatin1Char('0')).toUpper();
    }

    // Build the EDID summary.
    EdidSummary sum = summarizeEdid(raw, res.decoded);

    // Compose the dialog HTML.
    QString html;

    // 1) High-level summary, already formatted as HTML.
    if (!sum.highLevelText.isEmpty())
    {
        html += sum.highLevelText;
        html += "<br><br>";
    }

    // 2) Basic summary, already formatted as HTML.
    if (!sum.basicText.isEmpty())
    {
        html += sum.basicText;
        html += "<br><br>";
    }

    // 3) Source and size.
    html += tr("Source: %1<br>Size: %2 bytes<br>")
                .arg(res.sourceName.toHtmlEscaped())
                .arg(raw.size());
    html += "<br><br>";

    // 4) Raw EDID hex dump.
    html += "<b>Raw EDID</b>";
    html += hex.toHtmlEscaped().replace("\n", "<br>");
    html += "<br><br>";

    // 5) Raw edid-decode output.
    html += "<b>Decoded by edid-decode</b><br>";
    if (res.decoded.isEmpty())
    {
        html += tr("(No output from edid-decode)");
    }
    else
    {
        html += res.decoded.toHtmlEscaped().replace("\n", "<br>");
    }

    // Show the dialog with a QTextEdit using HTML content.
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

    previewWindow_ = new previewwindow(ui->previewPanel);
    previewWindow_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    connect(previewWindow_, &previewwindow::doubleClicked, this, [this]()
            {
        if (previewFullscreen_)
            setPreviewFullscreen(false); });
    ui->previewArt->hide();
    ui->previewContainerLayout->addWidget(previewWindow_);
    previewWindow_->show();
}

void MainWindow::setPreviewFullscreen(bool enabled)
{
    if (previewFullscreen_ == enabled)
        return;

    previewFullscreen_ = enabled;
    if (enabled)
    {
        previewRestoreMaximized_ = isMaximized();
        debugDockWasVisible_ = debugDock_ && debugDock_->isVisible();
        previewPanelMargins_ = ui->previewPanelLayout->contentsMargins();
        previewPanelSpacing_ = ui->previewPanelLayout->spacing();
        previewPanelFrameShape_ = ui->previewPanel->frameShape();

        if (debugDock_)
            debugDock_->hide();
        ui->heroHeader->hide();
        ui->controlPanel->hide();
        ui->statusPanel->hide();
        ui->previewTitle->hide();
        ui->previewHint->hide();
        ui->labelinfo1->hide();
        ui->sectionTitlePreview->hide();
        ui->btnPreview->hide();
        ui->menubar->hide();
        ui->statusbar->hide();
        ui->mainLayout->setContentsMargins(0, 0, 0, 0);
        ui->previewPanelLayout->setContentsMargins(0, 0, 0, 0);
        ui->previewPanelLayout->setSpacing(0);
        ui->previewPanel->setFrameShape(QFrame::NoFrame);
        ui->previewPanel->setStyleSheet(QStringLiteral("QFrame#previewPanel { background: #000000; border: 0; border-radius: 0; }"));
        showFullScreen();
    }
    else
    {
        showNormal();
        if (previewRestoreMaximized_)
            showMaximized();

        ui->heroHeader->show();
        ui->controlPanel->show();
        ui->statusPanel->show();
        ui->previewTitle->show();
        ui->previewHint->show();
        ui->labelinfo1->show();
        ui->sectionTitlePreview->show();
        ui->btnPreview->show();
        ui->menubar->show();
        ui->statusbar->show();
        ui->mainLayout->setContentsMargins(16, 16, 16, 16);
        ui->previewPanelLayout->setContentsMargins(previewPanelMargins_);
        ui->previewPanelLayout->setSpacing(previewPanelSpacing_);
        ui->previewPanel->setFrameShape(previewPanelFrameShape_);
        ui->previewPanel->setStyleSheet(QString());
        ui->btnPreview->setText(tr("Fullscreen"));

        if (debugDock_ && debugDockWasVisible_)
            debugDock_->show();
    }
}

void MainWindow::setupRuntimeStatusTimer()
{
    runtimeStatusTimer_ = new QTimer(this);
    runtimeStatusTimer_->setInterval(200);
    connect(runtimeStatusTimer_, &QTimer::timeout, this, [this]()
            {
#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
                refreshGvfgMonitoring();
                if (gvfg_)
                    gvfg_->pollEvents();
#endif
                updateRuntimeStatusUi();
                refreshCaptureInfoFromSdkAndRuntime(true);
                refreshDisplayInfoFromCurrentState();

                if (!suppressAuxDialogRefresh_)
                {
                    if (signalInfoDialog_ && signalInfoDialog_->isVisible())
                    {
                        signalInfoDialog_->setInfoText(lastInfoText_);
                        signalInfoDialog_->setPropertyPages(captureInfo_.propertyPages);
                    }

                    if (displayInfoDialog_ && displayInfoDialog_->isVisible())
                        displayInfoDialog_->setInfoText(formatDisplayOutputInfo(displayInfo_));
                } });
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
    procAmpSettings_ = {128, 128, 128, 128, 128};
    if (!ui->actionProcAmp)
        return;

    connect(ui->actionProcAmp, &QAction::triggered, this, [this]()
            {
                if (!procAmpDialog_)
                {
                    procAmpDialog_ = new ProcAmp(this);
                    procAmpDialog_->setWindowTitle(tr("ProcAmp"));
                    procAmpDialog_->setValues(procAmpSettings_);
                    connect(procAmpDialog_, &ProcAmp::valuesChanged,
                            this,
                            [this](const gcap_procamp_t &p)
                            {
                                procAmpSettings_ = p;
                                if (directShow_ && directShow_->isOpen())
                                    directShow_->setProcAmp(p);
                            });
                }

                bool supported = false;
                if (directShow_ && directShow_->isOpen())
                {
                    gcap_procamp_caps_t caps{};
                    if (directShow_->procAmpCapabilities(&caps) == GCAP_OK)
                    {
                        procAmpDialog_->setCaps(caps);
                        supported = true;

                        gcap_procamp_t current{};
                        if (directShow_->procAmp(&current) == GCAP_OK)
                            procAmpSettings_ = current;
                    }
                }
                else
                {
                    // No active SDK handle yet. Keep the dialog usable for pre-start values;
                    // the values are applied in startCapture() after gcap_open().
                    supported = selectedBackend() == CaptureBackend::DirectShow;
                }

                procAmpDialog_->setControlsEnabled(supported);
                procAmpDialog_->setValues(procAmpSettings_);
                procAmpDialog_->show();
                procAmpDialog_->raise();
                procAmpDialog_->activateWindow(); });
}

void MainWindow::setupPreviewBitDepthControls()
{
    if (!ui->comboPreviewBitDepth)
        return;

    const QSignalBlocker blocker(ui->comboPreviewBitDepth);
    ui->comboPreviewBitDepth->clear();
    ui->comboPreviewBitDepth->addItem(QStringLiteral("Preview: Auto"), GCAP_PREVIEW_BITDEPTH_AUTO);
    ui->comboPreviewBitDepth->addItem(QStringLiteral("Preview: 10-bit"), GCAP_PREVIEW_BITDEPTH_10BIT);
    ui->comboPreviewBitDepth->addItem(QStringLiteral("Preview: 8-bit"), GCAP_PREVIEW_BITDEPTH_8BIT);
    ui->comboPreviewBitDepth->setCurrentIndex(0);
}

int MainWindow::selectedPreviewBitDepthMode() const
{
    if (!ui->comboPreviewBitDepth)
        return GCAP_PREVIEW_BITDEPTH_AUTO;

    const int mode = ui->comboPreviewBitDepth->currentData().toInt();
    if (mode == GCAP_PREVIEW_BITDEPTH_8BIT ||
        mode == GCAP_PREVIEW_BITDEPTH_10BIT ||
        mode == GCAP_PREVIEW_BITDEPTH_AUTO)
    {
        return mode;
    }
    return GCAP_PREVIEW_BITDEPTH_AUTO;
}

QString MainWindow::selectedPreviewBitDepthText() const
{
    if (!ui->comboPreviewBitDepth)
        return QStringLiteral("Preview: Auto");
    return ui->comboPreviewBitDepth->currentText();
}

void MainWindow::applyPreviewSettingsToActiveSession()
{
#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
    if (gvfg_ && gvfg_->isRunning())
    {
        const bool ok = gvfg_->setPreview(previewWindow_ ? previewWindow_->previewHwnd() : nullptr,
                                          selectedPreviewBitDepthMode());
        MainWindow::postLog(QStringLiteral("[Preview] GVFG requested %1%2")
                                .arg(selectedPreviewBitDepthText())
                                .arg(ok ? QString() : QStringLiteral(" failed")),
                            !ok);
        updateRuntimeStatusUi();
        return;
    }
#endif
    if (!directShow_ || !directShow_->isRunning())
        return;

    void *hwnd = previewWindow_ ? previewWindow_->previewHwnd() : nullptr;

    gcap_preview_desc_t pv{};
    pv.hwnd = hwnd;
    pv.enable_preview = (hwnd != nullptr) ? 1 : 0;
    pv.use_fp16_pipeline = 1;
    pv.swapchain_10bit = selectedPreviewBitDepthMode();

    const gcap_status_t st = directShow_->setPreview(pv);
    if (st != GCAP_OK)
    {
        MainWindow::postLog(QStringLiteral("[Preview] apply bit depth failed: mode=%1 status=%2")
                                .arg(pv.swapchain_10bit)
                                .arg(int(st)),
                            true);
        return;
    }

    MainWindow::postLog(QStringLiteral("[Preview] requested %1 mode=%2; swapchain will be recreated on next frame")
                            .arg(selectedPreviewBitDepthText())
                            .arg(pv.swapchain_10bit));
    updateRuntimeStatusUi();
}

void MainWindow::setupConnections()
{
    if (ui->comboDevice)
    {
        connect(ui->comboDevice, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int idx)
                {
                    ui->comboDevice->setToolTip(ui->comboDevice->currentText());
                    if (idx < 0)
                    {
                        deviceIndex_ = -1;
                        invalidateDeviceCapabilityCache();
                        refreshPixelFormatOptions(false);
                        return;
                    }

                    bool ok = false;
                    const int selectedDeviceIndex = ui->comboDevice->itemData(idx).toInt(&ok);
                    deviceIndex_ = ok ? selectedDeviceIndex : -1;
                    invalidateDeviceCapabilityCache();
                    refreshPixelFormatOptions(true);
#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
                    refreshGvfgMonitoring();
#endif
                    refreshCaptureInfoFromSdkAndRuntime(false);
                    if (signalInfoDialog_ && signalInfoDialog_->isVisible())
                    {
                        signalInfoDialog_->setInfoText(lastInfoText_);
                        signalInfoDialog_->setPropertyPages(captureInfo_.propertyPages);
                    } });
    }

    if (ui->comboBackend)
    {
        connect(ui->comboBackend, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int)
                {
                    bool gvfgRunning = false;
#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
                    gvfgRunning = gvfg_ && gvfg_->isRunning();
#endif
                    const bool isGvfgBackend = selectedBackend() == CaptureBackend::Gvfg;
                    if (ui->comboDevice)
                        ui->comboDevice->setEnabled(true);
                    if (ui->checkZeroCopy)
                    {
                        ui->checkZeroCopy->setVisible(isGvfgBackend);
                        ui->checkZeroCopy->setEnabled(isGvfgBackend && !gvfgRunning);
                    }

                    // Re-enumerate devices when backend changes. Device index is only
                    // meaningful within the backend that produced the list.
                    initializeDeviceList();
                    refreshCaptureInfoFromSdkAndRuntime(false);
                    if (signalInfoDialog_ && signalInfoDialog_->isVisible())
                    {
                        signalInfoDialog_->setInfoText(lastInfoText_);
                        signalInfoDialog_->setPropertyPages(captureInfo_.propertyPages);
                    }
                    updateRuntimeStatusUi(); });
    }

#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
    if (ui->sliderGvfgVolume && gvfg_)
    {
        gvfg_->setAudioVolume(static_cast<float>(ui->sliderGvfgVolume->value()) / 100.0f);
        connect(ui->sliderGvfgVolume, &QSlider::valueChanged, this, [this](int value)
                {
                    if (gvfg_)
                        gvfg_->setAudioVolume(static_cast<float>(value) / 100.0f); });
    }

    if (ui->checkZeroCopy)
        connect(ui->checkZeroCopy, &QCheckBox::toggled, this, [this](bool)
                {
                    if (gvfg_ && gvfg_->isRunning())
                        return;
                    if (selectedBackend() == CaptureBackend::Gvfg)
                        refreshGvfgMonitoring(); });

    if (ui->comboPixelFormat)
        connect(ui->comboPixelFormat, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int)
                {
                    if (selectedBackend() != CaptureBackend::Gvfg || !gvfg_ || gvfg_->isRunning())
                        return;
                    refreshGvfgMonitoring(); });
#endif

    if (ui->checkAudioMonitoring)
    {
        connect(ui->checkAudioMonitoring, &QCheckBox::toggled, this, [this](bool enabled)
                {
#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
                    if (ui->sliderGvfgVolume)
                        ui->sliderGvfgVolume->setEnabled(selectedBackend() == CaptureBackend::Gvfg && enabled);
#endif
                    const bool captureRunning = activeCaptureSource() != nullptr;
                    if (!captureRunning)
                        return;
                    if (enabled)
                        startAudioMonitoring();
                    else
                        stopAudioMonitoring(); });
    }

    if (ui->comboPreviewBitDepth)
    {
        connect(ui->comboPreviewBitDepth, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int)
                { applyPreviewSettingsToActiveSession(); });
    }

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
    if (ui->btnOpenRawInspector)
        connect(ui->btnOpenRawInspector, &QPushButton::clicked, this, &MainWindow::onOpenRawInspect);
    if (ui->actionOpenRawInspector)
        connect(ui->actionOpenRawInspector, &QAction::triggered, this, &MainWindow::onOpenRawInspect);
}

void MainWindow::logStartupInfo()
{
    MainWindow::postLog(QStringLiteral("Viewer version: %1").arg(QString::fromLatin1(QT6_VIEWER_VERSION)));
    MainWindow::postLog(QStringLiteral("gcapture SDK version: %1").arg(QString::fromUtf8(gcap_version_string())));
#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
    MainWindow::postLog(QStringLiteral("GVFG SDK version: %1").arg(QString::fromLatin1(gvfg_get_version())));
#endif

    const QString logPath = qApp ? qApp->property("logPath").toString() : QString();
    if (!logPath.isEmpty())
        MainWindow::postLog(QStringLiteral("Log file: %1").arg(logPath));
}

void MainWindow::postLog(const QString &line, bool isError)
{
    // 1) Write through the Qt message handler.
    if (isError)
        qWarning().noquote() << line;
    else
        qInfo().noquote() << line;

    // 2) Mirror to the UI when available.
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

void MainWindow::onOpenRawInspect()
{
    if (openingRawInspector_)
        return;

    openingRawInspector_ = true;
    suppressAuxDialogRefresh_ = true;

    const bool wasTimerActive = runtimeStatusTimer_ && runtimeStatusTimer_->isActive();
    if (wasTimerActive)
        runtimeStatusTimer_->stop();

    const QString path = QFileDialog::getOpenFileName(
        this,
        tr("Open Pixel Data"),
        QString(),
        tr("Pixel Data (*.raw *.dcm *.dicom);;"
           "RAW Frame Files (*.raw);;"
           "DICOM Files (*.dcm *.dicom);;"
           "All Files (*.*)"));

    if (wasTimerActive && runtimeStatusTimer_)
        runtimeStatusTimer_->start();
    suppressAuxDialogRefresh_ = false;
    openingRawInspector_ = false;

    if (path.isEmpty())
        return;

    if (!rawInspectorDlg_)
        rawInspectorDlg_ = new RawInspectorDialog(this);
    rawInspectorDlg_->openFile(path);
    rawInspectorDlg_->show();
    rawInspectorDlg_->raise();
    rawInspectorDlg_->activateWindow();
    MainWindow::postLog(QStringLiteral("[RAW Inspector] opened %1").arg(path));
}

void MainWindow::setupRegisterTools()
{
#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND) && defined(QT6_VIEWER_ENABLE_GVFG_INTERNAL_TOOLS)
    if (!ui->actionGvfgRegisters ||
        !QCoreApplication::arguments().contains(QStringLiteral("--gvfg-registers")))
        return;

    ui->actionGvfgRegisters->setVisible(true);
    connect(ui->actionGvfgRegisters, &QAction::triggered, this, [this]()
            {
                RegisterDialog dialog(gvfg_, this);
                connect(&dialog, &RegisterDialog::logMessage, this,
                        [](const QString &message, bool isError)
                        { MainWindow::postLog(message, isError); });
                dialog.exec(); });
#endif
}

void MainWindow::setInspectorTools()
{
    const bool enabled =
        QCoreApplication::arguments().contains(
            QStringLiteral("--inspector"));

    if (ui->actionOpenRawInspector)
    {
        ui->actionOpenRawInspector->setVisible(enabled);
        ui->actionOpenRawInspector->setEnabled(enabled);
    }

    if (ui->btnOpenRawInspector)
    {
        ui->btnOpenRawInspector->setVisible(enabled);
        ui->btnOpenRawInspector->setEnabled(enabled);
    }
}

void MainWindow::on_btnPreview_clicked()
{
    setPreviewFullscreen(!previewFullscreen_);
}
