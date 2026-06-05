#include "mainwindow.h"
#include "previewwindow.h"
#include "ui_mainwindow.h"

#include <QCloseEvent>
#include <QDateTime>
#include <QMetaObject>
#include <QTimer>

namespace
{
    QString fpgaFieldText(bool valid, const QString &text)
    {
        return valid ? text : QStringLiteral("--");
    }

    QString hex32(uint32_t value)
    {
        return QStringLiteral("0x") + QString::number(value, 16).rightJustified(8, QLatin1Char('0')).toUpper();
    }

    QString eventTypeText(gvfg_event_type_t type)
    {
        switch (type)
        {
        case GVFG_EVENT_VIDEO_IRQ:
            return QStringLiteral("VIDEO_IRQ");
        case GVFG_EVENT_PLUG_IN:
            return QStringLiteral("PLUG_IN");
        case GVFG_EVENT_PLUG_OUT:
            return QStringLiteral("PLUG_OUT");
        case GVFG_EVENT_CAPTURE_PAUSED:
            return QStringLiteral("CAPTURE_PAUSED");
        case GVFG_EVENT_CAPTURE_RESUMED:
            return QStringLiteral("CAPTURE_RESUMED");
        default:
            return QStringLiteral("UNKNOWN");
        }
    }

    QString fpgaRawLogLine(const gvfg_runtime_info_t &info)
    {
        const auto &fpga = info.input_signal.fpga;
        return QStringLiteral("fpga_raw\n"
                              "  valid_mask=%1\n"
                              "  resolution:   width_valid=%2 width_raw=%3 height_valid=%4 height_raw=%5\n"
                              "  video_format: valid=%6 raw=%7 code=%8 name=%9\n"
                              "  frame_rate:   valid=%10 raw=%11 code=%12 bits=%13 name=%14\n"
                              "  bit_depth:    valid=%15 raw=%16 value=%17\n"
                              "  status:       valid=%18 raw=%19 sdi_lock=%20 sdi_ddr=%21 hdmi_lock=%22 hdmi_ddr=%23")
            .arg(hex32(fpga.valid_mask))
            .arg(fpga.width_valid)
            .arg(fpga.width_raw)
            .arg(fpga.height_valid)
            .arg(fpga.height_raw)
            .arg(fpga.video_format_valid)
            .arg(hex32(fpga.video_format_raw))
            .arg(fpga.video_format_code)
            .arg(QString::fromLatin1(fpga.video_format))
            .arg(fpga.frame_rate_valid)
            .arg(hex32(fpga.frame_rate_raw))
            .arg(fpga.frame_rate_code)
            .arg(QString::fromLatin1(fpga.frame_rate_bits))
            .arg(QString::fromLatin1(fpga.frame_rate_name))
            .arg(fpga.bit_depth_valid)
            .arg(hex32(fpga.bit_depth_raw))
            .arg(fpga.bit_depth)
            .arg(fpga.status_valid)
            .arg(hex32(fpga.status_raw))
            .arg(fpga.sdi_locked)
            .arg(fpga.sdi_ddr_ok)
            .arg(fpga.hdmi_locked)
            .arg(fpga.hdmi_ddr_ok);
    }

    QString fpgaRawStateKey(const gvfg_runtime_info_t &info)
    {
        const auto &fpga = info.input_signal.fpga;
        return QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8|%9|%10|%11|%12|%13|%14|%15|%16|%17|%18|%19|%20|%21|%22|%23")
            .arg(fpga.valid_mask)
            .arg(fpga.width_valid)
            .arg(fpga.width_raw)
            .arg(fpga.height_valid)
            .arg(fpga.height_raw)
            .arg(fpga.video_format_valid)
            .arg(fpga.video_format_raw)
            .arg(fpga.video_format_code)
            .arg(QString::fromLatin1(fpga.video_format))
            .arg(fpga.frame_rate_valid)
            .arg(fpga.frame_rate_raw)
            .arg(fpga.frame_rate_code)
            .arg(QString::fromLatin1(fpga.frame_rate_bits))
            .arg(QString::fromLatin1(fpga.frame_rate_name))
            .arg(fpga.bit_depth_valid)
            .arg(fpga.bit_depth_raw)
            .arg(fpga.bit_depth)
            .arg(fpga.status_valid)
            .arg(fpga.status_raw)
            .arg(fpga.sdi_locked)
            .arg(fpga.sdi_ddr_ok)
            .arg(fpga.hdmi_locked)
            .arg(fpga.hdmi_ddr_ok);
    }
}

MainWindow::MainWindow(QWidget *parent)
    : QWidget(parent), ui_(new Ui::MainWindow)
{
    ui_->setupUi(this);

    previewWindow_ = new PreviewWindow();
    ui_->logEdit->setMaximumBlockCount(300);
    ui_->statusLabel->setWordWrap(true);
    signalStatusTimer_ = new QTimer(this);
    signalStatusTimer_->setInterval(1000);

    connect(ui_->refreshButton, &QPushButton::clicked, this, [this]() { refreshDevices(); });
    connect(ui_->showPreviewButton, &QPushButton::clicked, this, [this]() { showPreviewWindow(); });
    connect(ui_->startButton, &QPushButton::clicked, this, [this]() { startCapture(); });
    connect(ui_->stopButton, &QPushButton::clicked, this, [this]() { stopCapture(); });
    connect(signalStatusTimer_, &QTimer::timeout, this, [this]() { updateSignalStatus(true); });

    setRunningUi(false);
    refreshDevices();
}

MainWindow::~MainWindow()
{
    stopCapture();
    delete previewWindow_;
    delete ui_;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    stopCapture();
    if (previewWindow_)
        previewWindow_->closePreview();

    QWidget::closeEvent(event);
}

void MainWindow::refreshDevices()
{
    ui_->deviceCombo->clear();
    devices_ = {};

    const int count = gvfg_enumerate_devices(devices_.data(), GVFG_MAX_DEVICES);
    deviceCount_ = count > 0 ? count : 0;

    for (int i = 0; i < deviceCount_; ++i)
    {
        const QString name = QString::fromUtf8(devices_[i].name);
        ui_->deviceCombo->addItem(name.isEmpty() ? QStringLiteral("GVFG Capture") : name, devices_[i].index);
    }

    if (deviceCount_ <= 0)
    {
        ui_->deviceCombo->addItem(QStringLiteral("No GVFG device found"), -1);
        appendLog(QStringLiteral("No GVFG device found"));
    }
    else
    {
        appendLog(QStringLiteral("Found %1 XDMA device(s)").arg(deviceCount_));
    }
}

void MainWindow::showPreviewWindow()
{
    previewWindow_->showPreview();

    if (handle_ && !applyPreview())
        appendLog(QStringLiteral("Show Preview failed: unable to update preview window"));
}

void MainWindow::startCapture()
{
    if (handle_)
        stopCapture();

    if (ui_->deviceCombo->currentData().toInt() < 0)
    {
        appendLog(QStringLiteral("Start skipped: no device selected"));
        return;
    }

    gvfg_status_t st = gvfg_create(&handle_);
    if (st != GVFG_OK || !handle_)
    {
        showError(QStringLiteral("gvfg_create"), st);
        handle_ = nullptr;
        return;
    }

    gvfg_set_callbacks(handle_, &MainWindow::onFrame, &MainWindow::onError, this);
    gvfg_set_event_callback(handle_, &MainWindow::onEvent, this, GVFG_EVENT_MASK_DEFAULT);

    previewWindow_->showPreview();
    if (!applyPreview())
    {
        stopCapture();
        return;
    }

    const int deviceIndex = ui_->deviceCombo->currentData().toInt();
    st = gvfg_open(handle_, deviceIndex);
    if (st != GVFG_OK)
    {
        showError(QStringLiteral("gvfg_open"), st);
        stopCapture();
        return;
    }

    lastSignalStatusText_.clear();
    lastFpgaRawStateKey_.clear();
    appendLog(QStringLiteral("FPGA signal before stream start"));
    updateSignalStatus(true);

    st = gvfg_start(handle_);
    if (st != GVFG_OK)
    {
        showError(QStringLiteral("gvfg_start"), st);
        stopCapture();
        return;
    }

    frameCount_ = 0;
    setRunningUi(true);
    appendLog(QStringLiteral("Started device index %1").arg(deviceIndex));
    updateSignalStatus(true);
    signalStatusTimer_->start();
}

void MainWindow::stopCapture()
{
    if (signalStatusTimer_)
        signalStatusTimer_->stop();

    if (handle_)
    {
        gvfg_stop(handle_);
        gvfg_destroy(handle_);
        handle_ = nullptr;
        appendLog(QStringLiteral("Stopped"));
    }

    setRunningUi(false);
    ui_->statusLabel->setText(QStringLiteral("Idle"));
    lastSignalStatusText_.clear();
    lastFpgaRawStateKey_.clear();
}

bool MainWindow::applyPreview()
{
    if (!handle_)
        return false;

    gvfg_preview_desc_t preview{};
    preview.hwnd = previewWindow_->nativePreviewHandle();
    preview.enable_preview = 1;
    preview.swapchain_bitdepth = GVFG_PREVIEW_BITDEPTH_AUTO;

    const gvfg_status_t st = gvfg_set_preview(handle_, &preview);
    if (st != GVFG_OK)
    {
        showError(QStringLiteral("gvfg_set_preview"), st);
        return false;
    }
    return true;
}

void MainWindow::updateSignalStatus(bool writeLog)
{
    if (!handle_)
        return;

    gvfg_runtime_info_t info{};
    if (gvfg_get_runtime_info(handle_, &info) != GVFG_OK)
        return;

    const auto &sig = info.input_signal;
    const auto &fpga = sig.fpga;

    const QString line0 = QStringLiteral("%1x%2 stream fps=%3 buffer=%4 bitdepth=%5 | capture %6 fps | delivered %7")
                              .arg(sig.width)
                              .arg(sig.height)
                              .arg(sig.fps > 0.0 ? QString::number(sig.fps, 'f', 2)
                                                 : QStringLiteral("--"))
                              .arg(QString::fromUtf8(sig.pixel_format))
                              .arg(sig.bit_depth)
                              .arg(info.capture_fps > 0.0 ? QString::number(info.capture_fps, 'f', 2)
                                                          : QStringLiteral("--"))
                              .arg(static_cast<qulonglong>(info.delivered_frames));
    const QString line1 = QStringLiteral("FPGA format=%1 | fps code=%2 | bitdepth=%3")
                              .arg(fpgaFieldText(fpga.video_format_valid != 0,
                                                 QStringLiteral("%1 (%2)")
                                                     .arg(fpga.video_format_code)
                                                     .arg(QString::fromLatin1(fpga.video_format))))
                              .arg(fpgaFieldText(fpga.frame_rate_valid != 0,
                                                 QStringLiteral("%1 (%2)")
                                                     .arg(QString::fromLatin1(fpga.frame_rate_bits))
                                                     .arg(QString::fromLatin1(fpga.frame_rate_name))))
                              .arg(fpgaFieldText(fpga.bit_depth_valid != 0,
                                                 QStringLiteral("%1").arg(fpga.bit_depth)));
    const QString line2 = QStringLiteral("FPGA status | SDI lock=%1 SDI DDR=%2 HDMI lock=%3 HDMI DDR=%4")
                              .arg(fpgaFieldText(fpga.status_valid != 0, QString::number(fpga.sdi_locked)))
                              .arg(fpgaFieldText(fpga.status_valid != 0, QString::number(fpga.sdi_ddr_ok)))
                              .arg(fpgaFieldText(fpga.status_valid != 0, QString::number(fpga.hdmi_locked)))
                              .arg(fpgaFieldText(fpga.status_valid != 0, QString::number(fpga.hdmi_ddr_ok)));

    const QString statusText = line0 + QLatin1Char('\n') + line1 + QLatin1Char('\n') + line2;
    if (lastSignalStatusText_ != statusText)
    {
        ui_->statusLabel->setText(statusText);
        lastSignalStatusText_ = statusText;
    }

    const QString stateKey = fpgaRawStateKey(info);
    if (writeLog && lastFpgaRawStateKey_ != stateKey)
    {
        appendLog(fpgaRawLogLine(info));
        lastFpgaRawStateKey_ = stateKey;
    }
}

void MainWindow::setRunningUi(bool running)
{
    ui_->startButton->setEnabled(!running);
    ui_->stopButton->setEnabled(running);
    ui_->refreshButton->setEnabled(!running);
    ui_->deviceCombo->setEnabled(!running);
}

void MainWindow::showError(const QString &apiName, gvfg_status_t status)
{
    appendLog(QStringLiteral("%1 failed: %2").arg(apiName, QString::fromUtf8(gvfg_strerror(status))));
}

void MainWindow::appendLog(const QString &message)
{
    const QString line = QStringLiteral("[%1] %2")
                             .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")))
                             .arg(message);
    ui_->logEdit->appendPlainText(line);
}

void MainWindow::onFrame(const gvfg_frame_t *frame, void *user)
{
    MainWindow *self = static_cast<MainWindow *>(user);
    if (!self || !frame)
        return;

    const uint64_t count = ++self->frameCount_;
    if (count <= 5 || (count % 60) == 0)
    {
        const int width = frame->width;
        const int height = frame->height;
        const uint64_t frameId = frame->frame_id;
        QMetaObject::invokeMethod(self,
                                  [self, count, frameId, width, height]()
                                  {
                                      self->appendLog(QStringLiteral("callback #%1 source id=%2 %3x%4")
                                                          .arg(static_cast<qulonglong>(count))
                                                          .arg(static_cast<qulonglong>(frameId))
                                                          .arg(width)
                                                          .arg(height));
                                  },
                                  Qt::QueuedConnection);
    }
}

void MainWindow::onEvent(const gvfg_event_t *event, void *user)
{
    MainWindow *self = static_cast<MainWindow *>(user);
    if (!self || !event)
        return;

    const QString type = eventTypeText(event->type);
    const uint32_t channel = event->channel;
    const uint32_t irqBit = event->irq_bit;
    const uint32_t irqMask = event->irq_mask;
    const uint64_t timestampNs = event->timestamp_ns;
    QMetaObject::invokeMethod(self,
                              [self, type, channel, irqBit, irqMask, timestampNs]()
                              {
                                  self->appendLog(QStringLiteral("event %1 ch=%2 irq=%3 mask=%4 ts=%5")
                                                      .arg(type)
                                                      .arg(channel)
                                                      .arg(irqBit)
                                                      .arg(hex32(irqMask))
                                                      .arg(static_cast<qulonglong>(timestampNs)));
                              },
                              Qt::QueuedConnection);
}

void MainWindow::onError(gvfg_status_t status, const char *message, void *user)
{
    MainWindow *self = static_cast<MainWindow *>(user);
    if (!self)
        return;

    const QString text = message && message[0]
                             ? QString::fromUtf8(message)
                             : QString::fromUtf8(gvfg_strerror(status));

    QMetaObject::invokeMethod(self,
                              [self, status, text]()
                              {
                                  self->appendLog(QStringLiteral("message %1: %2")
                                                      .arg(static_cast<int>(status))
                                                      .arg(text));
                              },
                              Qt::QueuedConnection);
}
