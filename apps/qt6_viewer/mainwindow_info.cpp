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
    case GCAP_FMT_ARGB:
        return "ARGB";
    default:
        return "UNKNOWN";
    }
}

static QString formatVideoCapDisplay(const gcap_video_cap_t &cap)
{
    const double fps = (cap.fps_den > 0) ? (double(cap.fps_num) / double(cap.fps_den)) : 0.0;
    QString text = QStringLiteral("%1 %2x%3")
                       .arg(QString::fromLatin1(packetFmtNameInfo(cap.pixfmt)))
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
    return QStringLiteral("%1 — %2")
        .arg(QString::fromUtf8(page.page_name))
        .arg(page.capture_pin ? QStringLiteral("Capture Pin") : QStringLiteral("Filter"));
}

static void fillDeviceCapabilitiesFromSdk(int deviceIndex, CaptureDeviceInfo &info)
{
    info.supportedFormats.clear();
    info.propertyPages.clear();

    const int capCount = gcap_enum_video_caps(deviceIndex, nullptr, 0);
    if (capCount > 0)
    {
        std::vector<gcap_video_cap_t> caps(static_cast<size_t>(capCount));
        const int written = gcap_enum_video_caps(deviceIndex, caps.data(), static_cast<int>(caps.size()));
        for (int i = 0; i < written; ++i)
            info.supportedFormats << formatVideoCapDisplay(caps[static_cast<size_t>(i)]);
    }

    const int pageCount = gcap_enum_property_pages(deviceIndex, nullptr, 0);
    if (pageCount > 0)
    {
        std::vector<gcap_property_page_t> pages(static_cast<size_t>(pageCount));
        const int written = gcap_enum_property_pages(deviceIndex, pages.data(), static_cast<int>(pages.size()));
        for (int i = 0; i < written; ++i)
            info.propertyPages << formatPropertyPageDisplay(pages[static_cast<size_t>(i)]);
    }
}
} // namespace

int MainWindow::currentDeviceIndex() const
{
    return (ui && ui->comboDevice) ? ui->comboDevice->currentIndex() : 0;
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

    fillDeviceCapabilitiesFromSdk(currentDeviceIndex(), captureInfo_);
    lastInfoText_ = formatCaptureDeviceInfo(captureInfo_, avgFps_);
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
    const bool isSc0710 = deviceText.contains(QStringLiteral("SC0710"), Qt::CaseInsensitive);
    if (isSc0710)
    {
        MainWindow::postLog(QStringLiteral("[SignalInfo] SC0710 detected, launching vendor property page. devIndex=%1 name=%2")
                                .arg(devIndex)
                                .arg(deviceText));
        const bool ok = (gcap_open_vendor_property_page(devIndex) != 0);
        if (ok)
            return;

        MainWindow::postLog(QStringLiteral("[SignalInfo] vendor property page launch failed, fallback to generic dialog. devIndex=%1")
                                .arg(devIndex),
                            true);
        QMessageBox::warning(this,
                             tr("Signal Info"),
                             tr("Open SC0710 vendor signal page failed.\nFallback to generic signal info dialog."));
    }
#endif
    ensureSignalInfoDialog();
    refreshSignalInfoDialog();
    showAndActivateDialog(infoDlg_);
}

void MainWindow::onOpenVendorPropertyPageTest()
{
#ifdef _WIN32
    const int devIndex = ui && ui->comboDevice ? ui->comboDevice->currentIndex() : 0;
    MainWindow::postLog(QStringLiteral("[VendorPageTest] begin devIndex=%1").arg(devIndex));

    const bool ok = (gcap_open_vendor_property_page(devIndex) != 0);
    MainWindow::postLog(QStringLiteral("[VendorPageTest] launch result=%1 devIndex=%2")
                            .arg(ok ? QStringLiteral("STARTED") : QStringLiteral("FAIL"))
                            .arg(devIndex),
                        !ok);
    if (!ok)
    {
        QMessageBox::warning(this,
                             tr("Vendor Property Page"),
                             tr("Open Vendor Property Page failed.\nPlease check debug log."));
    }
#else
    QMessageBox::information(this,
                             tr("Vendor Property Page"),
                             tr("This test is only available on Windows."));
#endif
}

void MainWindow::onShowDisplayInfo()
{
    ensureDisplayInfoDialog();
    refreshDisplayInfoDialog();
    showAndActivateDialog(DpinfoDlg_);
}
