#include "mainwindow.h"
#include "previewwindow.h"
#include "ui_mainwindow.h"

#include <QCloseEvent>
#include <QDateTime>
#include <QMetaObject>

MainWindow::MainWindow(QWidget *parent)
    : QWidget(parent), ui_(new Ui::MainWindow)
{
    ui_->setupUi(this);

    previewWindow_ = new PreviewWindow();
    ui_->logEdit->setMaximumBlockCount(300);

    connect(ui_->refreshButton, &QPushButton::clicked, this, [this]() { refreshDevices(); });
    connect(ui_->showPreviewButton, &QPushButton::clicked, this, [this]() { showPreviewWindow(); });
    connect(ui_->startButton, &QPushButton::clicked, this, [this]() { startCapture(); });
    connect(ui_->stopButton, &QPushButton::clicked, this, [this]() { stopCapture(); });

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
    updateSignalStatus();
}

void MainWindow::stopCapture()
{
    if (handle_)
    {
        gvfg_stop(handle_);
        gvfg_destroy(handle_);
        handle_ = nullptr;
        appendLog(QStringLiteral("Stopped"));
    }

    setRunningUi(false);
    ui_->statusLabel->setText(QStringLiteral("Idle"));
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

void MainWindow::updateSignalStatus()
{
    if (!handle_)
        return;

    gvfg_runtime_info_t info{};
    if (gvfg_get_runtime_info(handle_, &info) != GVFG_OK)
        return;

    ui_->statusLabel->setText(QStringLiteral("%1x%2 %3 fps %4 | capture %5 fps | frames %6")
                                  .arg(info.input_signal.width)
                                  .arg(info.input_signal.height)
                                  .arg(info.input_signal.fps > 0.0 ? QString::number(info.input_signal.fps, 'f', 2)
                                                                    : QStringLiteral("--"))
                                  .arg(QString::fromUtf8(info.input_signal.pixel_format))
                                  .arg(info.capture_fps > 0.0 ? QString::number(info.capture_fps, 'f', 2)
                                                              : QStringLiteral("--"))
                                  .arg(static_cast<qulonglong>(info.delivered_frames)));
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
                                      self->appendLog(QStringLiteral("frame #%1 id=%2 %3x%4")
                                                          .arg(static_cast<qulonglong>(count))
                                                          .arg(static_cast<qulonglong>(frameId))
                                                          .arg(width)
                                                          .arg(height));
                                      self->updateSignalStatus();
                                  },
                                  Qt::QueuedConnection);
    }
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
