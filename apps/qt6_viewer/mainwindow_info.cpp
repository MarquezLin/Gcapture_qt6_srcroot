#include "mainwindow.h"

#include "./ui_mainwindow.h"
#include <QDateTime>
#include <QGuiApplication>
#include <QLabel>
#include <QMessageBox>
#include <QScreen>
#include <QWindow>
#include <vector>

#include "display_info.h"

namespace
{
static const char *packetFmtNameInfo(int fmt)
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
    case GCAP_FMT_V210:
        return "V210";
    case GCAP_FMT_ARGB:
        return "ARGB";
    default:
        return "UNKNOWN";
    }
}

static gcap_pixfmt_t pixFmtFromNameInfo(const char *name)
{
    const QString fmt = QString::fromUtf8(name ? name : "").trimmed().toUpper();
    if (fmt == QStringLiteral("NV12"))
        return GCAP_FMT_NV12;
    if (fmt == QStringLiteral("P010"))
        return GCAP_FMT_P010;
    if (fmt == QStringLiteral("Y210"))
        return GCAP_FMT_Y210;
    if (fmt == QStringLiteral("V210"))
        return GCAP_FMT_V210;
    if (fmt == QStringLiteral("ARGB") || fmt == QStringLiteral("ARGB32") || fmt == QStringLiteral("BGRA") ||
        fmt == QStringLiteral("BGRA8") || fmt == QStringLiteral("RGB32"))
        return GCAP_FMT_ARGB;
    return GCAP_FMT_YUY2;
}

static QString formatVideoCapDisplay(const gcap_video_cap_t &cap)
{
    const double fps = (cap.fps_den > 0) ? (double(cap.fps_num) / double(cap.fps_den)) : 0.0;
    const QString nativeName = cap.subtype_name[0]
                                   ? QString::fromUtf8(cap.subtype_name)
                                   : QString::fromLatin1(packetFmtNameInfo(cap.pixfmt));
    QString text = QStringLiteral("%1 %2x%3")
                       .arg(nativeName)
                       .arg(cap.width)
                       .arg(cap.height);
    if (fps > 0.0)
        text += QStringLiteral(" %1 fps").arg(fps, 0, 'f', 2);
    if (cap.bit_depth > 0)
        text += QStringLiteral(" (%1-bit)").arg(cap.bit_depth);
    return text;
}

static QString formatPropertyPageDisplay(const gcap_property_page_t &page)
{
    return QStringLiteral("%1 - %2")
        .arg(QString::fromUtf8(page.page_name))
        .arg(page.capture_pin ? QStringLiteral("Capture Pin") : QStringLiteral("Filter"));
}

} // namespace

void MainWindow::ensureDeviceCapabilityCache(int deviceIndex)
{
    const int backend = ui && ui->comboBackend ? ui->comboBackend->currentData().toInt() : GCAP_BACKEND_DSHOW;

    if (deviceIndex < 0)
    {
        cachedDeviceCapsBackend_ = -1;
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

#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
    if (backend == kQtViewerGvfgBackend)
    {
        cachedSupportedFormats_ << QStringLiteral("YUY2");
        return;
    }
#endif

    // Detailed StreamCaps and vendor property pages are DirectShow-specific APIs.
    // Do not query them with a WinMF device index, because backend indexes can differ.
    if (backend != GCAP_BACKEND_DSHOW)
    {
        const int fmtCount = gcap_enum_supported_pixel_formats(backend, deviceIndex, nullptr, 0);
        if (fmtCount > 0)
        {
            std::vector<gcap_pixfmt_t> formats(static_cast<size_t>(fmtCount));
            const int written = gcap_enum_supported_pixel_formats(backend, deviceIndex, formats.data(), static_cast<int>(formats.size()));
            for (int i = 0; i < written; ++i)
                cachedSupportedFormats_ << QString::fromLatin1(packetFmtNameInfo(formats[static_cast<size_t>(i)]));
        }
        return;
    }

    const int capCount = gcap_enum_video_caps(deviceIndex, nullptr, 0);
    if (capCount > 0)
    {
        std::vector<gcap_video_cap_t> caps(static_cast<size_t>(capCount));
        const int written = gcap_enum_video_caps(deviceIndex, caps.data(), static_cast<int>(caps.size()));
        for (int i = 0; i < written; ++i)
            cachedSupportedFormats_ << formatVideoCapDisplay(caps[static_cast<size_t>(i)]);
    }

    const int pageCount = gcap_enum_property_pages(deviceIndex, nullptr, 0);
    if (pageCount > 0)
    {
        std::vector<gcap_property_page_t> pages(static_cast<size_t>(pageCount));
        const int written = gcap_enum_property_pages(deviceIndex, pages.data(), static_cast<int>(pages.size()));
        for (int i = 0; i < written; ++i)
            cachedPropertyPages_ << formatPropertyPageDisplay(pages[static_cast<size_t>(i)]);
    }
}

void MainWindow::invalidateDeviceCapabilityCache()
{
    cachedDeviceCapsBackend_ = -1;
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

    // Prefer the active provider's open-graph selectable list. This is the list
    // actually used by SetFormat retry after SDK filtering/policy. For WDM devices
    // such as Blackmagic, a separate capability enumeration can disagree with the
    // active graph and show misleading modes.
    if (h_)
    {
        gcap_runtime_info_t rt{};
        if (gcap_get_runtime_info(h_, &rt) == GCAP_OK && rt.selectable_caps_inline[0])
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
    if (infoDlg_)
        return;

    infoDlg_ = new inputinfodialog(this);
    infoDlg_->setWindowTitle(tr("Signal Info"));

    connect(infoDlg_, &inputinfodialog::audioDeviceSelected,
            this, [this](const QString &id)
            { recordAudioDeviceIdUtf8_ = id; });

    connect(infoDlg_, &inputinfodialog::openPropertyPageRequested,
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
    if (DpinfoDlg_)
        return;

    DpinfoDlg_ = new DisplayInfoDialog(this);
    DpinfoDlg_->setWindowTitle(tr("Display Info"));
}

void MainWindow::showAndActivateDialog(QWidget *dialog)
{
    if (!dialog)
        return;

    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void MainWindow::refreshCaptureRuntimeInfo()
{
#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
    if (usingGvfg_ && gvfg_)
    {
        const gvfg_runtime_info_t rt = gvfg_->runtimeInfo();
        const gvfg_preview_info_t pv = gvfg_->previewInfo();
#ifdef QT6_VIEWER_USE_STANDALONE_GVFG
        const auto &signal = rt.input_signal;
        const auto &frame = rt.last_frame;

        gcap_signal_status_t fpgaSignal{};
        fpgaSignal.width = signal.width;
        fpgaSignal.height = signal.height;
        fpgaSignal.fps_num = 0;
        fpgaSignal.fps_den = 0;
        fpgaSignal.pixfmt = (signal.bit_depth >= 10) ? GCAP_FMT_Y210 : GCAP_FMT_YUY2;
        fpgaSignal.bit_depth = signal.bit_depth;
        fpgaSignal.csp = GCAP_CSP_BT709;
        fpgaSignal.range = GCAP_RANGE_LIMITED;

        gcap_signal_status_t dmaBuffer{};
        if (frame.valid)
        {
            dmaBuffer.width = frame.width;
            dmaBuffer.height = frame.height;
            dmaBuffer.pixfmt = pixFmtFromNameInfo(frame.pixel_format);
            dmaBuffer.bit_depth = frame.bit_depth;
            dmaBuffer.csp = GCAP_CSP_BT709;
            dmaBuffer.range = GCAP_RANGE_LIMITED;
        }

        captureInfo_.signal = fpgaSignal;
        captureInfo_.signalProbe = fpgaSignal;
        captureInfo_.negotiated = dmaBuffer;
        captureInfo_.backendName = QStringLiteral("GVFG");
        captureInfo_.frameSource = QStringLiteral("GVFG capture frame");
        captureInfo_.pathName = QStringLiteral("FPGA reported signal -> gvfg_read_frame");
        captureInfo_.captureFormat = frame.valid ? QString::fromUtf8(frame.pixel_format) : QStringLiteral("--");
        captureInfo_.renderFormat = pv.active
                                        ? QStringLiteral("gvfg_preview %1x%2 %3 %4bit")
                                              .arg(pv.width)
                                              .arg(pv.height)
                                              .arg(QString::fromUtf8(pv.pixel_format))
                                              .arg(pv.bit_depth)
                                        : QStringLiteral("App-owned render");
#else
        const auto &fpga = rt.input_signal.fpga;
        const auto &delivered = rt.delivered_frame;

        gcap_signal_status_t fpgaSignal{};
        fpgaSignal.width = fpga.width_valid ? static_cast<int>(fpga.width_raw) : rt.input_signal.width;
        fpgaSignal.height = fpga.height_valid ? static_cast<int>(fpga.height_raw) : rt.input_signal.height;
        fpgaSignal.fps_num = 0;
        fpgaSignal.fps_den = 0;
        fpgaSignal.pixfmt = (fpga.bit_depth_valid && fpga.bit_depth >= 10) ? GCAP_FMT_Y210 : GCAP_FMT_YUY2;
        fpgaSignal.bit_depth = fpga.bit_depth_valid ? fpga.bit_depth : rt.input_signal.bit_depth;
        fpgaSignal.csp = GCAP_CSP_BT709;
        fpgaSignal.range = GCAP_RANGE_LIMITED;

        gcap_signal_status_t dmaBuffer{};
        if (delivered.valid)
        {
            dmaBuffer.width = delivered.width;
            dmaBuffer.height = delivered.height;
            dmaBuffer.pixfmt = pixFmtFromNameInfo(delivered.pixel_format);
            dmaBuffer.bit_depth = delivered.bit_depth;
            dmaBuffer.csp = GCAP_CSP_BT709;
            dmaBuffer.range = GCAP_RANGE_LIMITED;
        }

        captureInfo_.signal = fpgaSignal;
        captureInfo_.signalProbe = fpgaSignal;
        captureInfo_.negotiated = dmaBuffer;
        captureInfo_.backendName = QStringLiteral("GVFG");
        captureInfo_.frameSource = QStringLiteral("GVFG capture frame");
        captureInfo_.pathName = QStringLiteral("FPGA reported signal -> delivered frame callback");
        captureInfo_.captureFormat = delivered.valid ? QString::fromUtf8(delivered.pixel_format) : QStringLiteral("--");
        captureInfo_.renderFormat = pv.active ? QString::fromUtf8(pv.render_path) : QStringLiteral("App-owned render");
#endif
        return;
    }
#endif

    if (!h_)
        return;

    gcap_signal_status_t sig{};
    if (gcap_get_signal_status(h_, &sig) == GCAP_OK)
        captureInfo_.signal = sig;

    gcap_runtime_info_t rt{};
    if (gcap_get_runtime_info(h_, &rt) != GCAP_OK)
        return;

    captureInfo_.signalProbe = rt.signal_probe;
    captureInfo_.negotiated = rt.negotiated;
    captureInfo_.backendName = QString::fromUtf8(rt.backend_name);
    captureInfo_.frameSource = QString::fromUtf8(rt.frame_source);
    captureInfo_.pathName = QString::fromUtf8(rt.path_name);
    captureInfo_.captureFormat = rt.negotiated_desc[0] ? QString::fromUtf8(rt.negotiated_desc)
                                                       : (rt.source_format[0] ? QString::fromUtf8(rt.source_format) : QString());
    captureInfo_.renderFormat = QString::fromUtf8(rt.render_format);
}

void MainWindow::refreshCaptureDeviceProps(bool throttleDeviceProps)
{
#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
    if (usingGvfg_)
        return;
#endif

    if (!h_)
        return;

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (throttleDeviceProps && (nowMs - lastPropsQueryMs_ <= 1000))
        return;

    lastPropsQueryMs_ = nowMs;

    gcap_device_props_t props{};
    if (gcap_get_device_props(h_, &props) != GCAP_OK)
        return;

    captureInfo_.driverVersion = QString::fromUtf8(props.driver_version);
    captureInfo_.firmwareVersion = QString::fromUtf8(props.firmware_version);
    captureInfo_.serialNumber = QString::fromUtf8(props.serial_number);
}

QString MainWindow::currentAudioInfoText() const
{
    if (infoDlg_)
    {
        if (QLabel *audioLabel = infoDlg_->findChild<QLabel *>("labelAudioInfo"))
            return audioLabel->text();
    }
    return captureInfo_.audioInfo;
}

void MainWindow::refreshCaptureInfoFromSdkAndRuntime(bool throttleDeviceProps)
{
    captureInfo_.deviceName = currentDeviceText();
    captureInfo_.audioInfo = currentAudioInfoText();

    refreshCaptureRuntimeInfo();
    refreshCaptureDeviceProps(throttleDeviceProps);

    ensureDeviceCapabilityCache(currentDeviceIndex());
    captureInfo_.supportedFormats = cachedSupportedFormats_;
    captureInfo_.propertyPages = cachedPropertyPages_;
    lastInfoText_ = formatCaptureDeviceInfo(captureInfo_, avgFps_);
    updateCapabilityLabel();
}

void MainWindow::refreshDisplayInfoFromFrame(const QImage &img)
{
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
#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
    else if (backend == kQtViewerGvfgBackend || usingGvfg_)
    {
        displayInfo_.pipe.path = DisplayOutputInfo::Pipeline::Path::Gvfg;
        displayInfo_.pipe.adapterName = gpuName_;
        displayInfo_.pipe.adapterIndex = gpuIndex_;
    }
#endif
    else if (actualBackend == GCAP_BACKEND_WINMF_CPU)
    {
        displayInfo_.pipe.path = DisplayOutputInfo::Pipeline::Path::WinMFCpu;
    }
    else
    {
        displayInfo_.pipe.path = DisplayOutputInfo::Pipeline::Path::DirectShow;
    }
}

void MainWindow::refreshDisplayInfoFromCurrentState()
{
    if (lastFrameImage_.isNull())
    {
        QImage fallback(1, 1, QImage::Format_ARGB32_Premultiplied);
        refreshDisplayInfoFromFrame(fallback);
        displayInfo_.video.size = QSize();
        displayInfo_.video.fps = avgFps_;
        return;
    }

    refreshDisplayInfoFromFrame(lastFrameImage_);
}

void MainWindow::refreshSignalInfoDialog()
{
    refreshCaptureInfoFromSdkAndRuntime(false);
    if (!infoDlg_)
        return;

    infoDlg_->setWindowTitle(tr("Signal Info"));
    infoDlg_->setInfoText(lastInfoText_);
    infoDlg_->setPropertyPages(captureInfo_.propertyPages);
    infoDlg_->setCurrentAudioDevice(recordAudioDeviceIdUtf8_);
}

void MainWindow::refreshDisplayInfoDialog()
{
    refreshDisplayInfoFromCurrentState();
    if (!DpinfoDlg_)
        return;

    DpinfoDlg_->setWindowTitle(tr("Display Info"));
    DpinfoDlg_->setInfoText(formatDisplayOutputInfo(displayInfo_));
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
    showAndActivateDialog(infoDlg_);
}

void MainWindow::onShowDisplayInfo()
{
    ensureDisplayInfoDialog();
    refreshDisplayInfoDialog();
    showAndActivateDialog(DpinfoDlg_);
}
