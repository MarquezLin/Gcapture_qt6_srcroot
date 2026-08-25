#include "registerdialog.h"

#include "gvfg/gvfg_source.h"
#include "ui_registerdialog.h"

#include <gvfg_debug.h>

#include <QMessageBox>

RegisterDialog::RegisterDialog(GvfgSource *source, QWidget *parent)
    : QDialog(parent), ui_(new Ui::RegisterDialog), source_(source)
{
    ui_->setupUi(this);
    connect(ui_->refreshButton, &QPushButton::clicked, this, &RegisterDialog::refreshDevices);
    connect(ui_->openButton, &QPushButton::clicked, this, &RegisterDialog::openDevice);
    connect(ui_->closeDeviceButton, &QPushButton::clicked, this, &RegisterDialog::closeDevice);
    connect(ui_->readButton, &QPushButton::clicked, this, &RegisterDialog::readRegister);
    connect(ui_->writeButton, &QPushButton::clicked, this, &RegisterDialog::writeRegister);
    connect(ui_->closeButton, &QPushButton::clicked, this, &QDialog::accept);
    refreshDevices();
    updateAccessState();
}

RegisterDialog::~RegisterDialog()
{
    closeDevice();
    delete ui_;
}

void RegisterDialog::refreshDevices()
{
    const int previousIndex = ui_->deviceCombo->currentData().toInt();
    gvfg_device_info_t devices[GVFG_MAX_DEVICES] = {};
    const int count = gvfg_enumerate_devices(devices, GVFG_MAX_DEVICES);

    ui_->deviceCombo->clear();
    for (int i = 0; i < count; ++i)
    {
        QString name = QString::fromUtf8(devices[i].name);
        if (name.isEmpty())
            name = tr("GVFG Capture %1").arg(i);
        ui_->deviceCombo->addItem(name, i);
    }

    const int restoreIndex = ui_->deviceCombo->findData(previousIndex);
    if (restoreIndex >= 0)
        ui_->deviceCombo->setCurrentIndex(restoreIndex);
    updateAccessState();
}

void RegisterDialog::openDevice()
{
    if (source_ && source_->isRunning())
    {
        ui_->resultLabel->setText(tr("Active capture handle is already available. Stop capture before opening an independent handle."));
        updateAccessState();
        return;
    }
    if (debugHandle_)
        closeDevice();

    bool ok = false;
    const int deviceIndex = ui_->deviceCombo->currentData().toInt(&ok);
    if (!ok || deviceIndex < 0)
    {
        ui_->resultLabel->setText(tr("Select a GVFG device first."));
        return;
    }

    gvfg_status_t status = gvfg_create(&debugHandle_);
    if (status == GVFG_OK)
        status = gvfg_open_channel(debugHandle_, deviceIndex, GVFG_CHANNEL_0);
    if (status != GVFG_OK)
    {
        const QString error = QString::fromUtf8(gvfg_strerror(status));
        if (debugHandle_)
        {
            gvfg_destroy(debugHandle_);
            debugHandle_ = nullptr;
        }
        ui_->resultLabel->setText(tr("Open failed: %1").arg(error));
        emit logMessage(QStringLiteral("[GVFG][REG] independent device open failed index=%1: %2")
                            .arg(deviceIndex)
                            .arg(error),
                        true);
        updateAccessState();
        return;
    }

    ui_->resultLabel->setText(tr("Device opened without starting capture."));
    emit logMessage(QStringLiteral("[GVFG][REG] independent device opened index=%1").arg(deviceIndex), false);
    updateAccessState();
}

void RegisterDialog::closeDevice()
{
    if (debugHandle_)
    {
        gvfg_destroy(debugHandle_);
        debugHandle_ = nullptr;
        if (ui_)
        {
            ui_->resultLabel->setText(tr("Independent device handle closed."));
            emit logMessage(QStringLiteral("[GVFG][REG] independent device closed"), false);
        }
    }
    if (ui_)
        updateAccessState();
}

bool RegisterDialog::parseHex32(const QString &input, uint32_t &outValue)
{
    QString text = input.trimmed();
    if (text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
        text.remove(0, 2);

    bool ok = false;
    const qulonglong parsed = text.toULongLong(&ok, 16);
    if (!ok || parsed > UINT32_MAX)
        return false;

    outValue = static_cast<uint32_t>(parsed);
    return true;
}

bool RegisterDialog::validateOffset(uint32_t &outOffset)
{
    if (!parseHex32(ui_->offsetEdit->text(), outOffset) || (outOffset & 0x3u) != 0)
    {
        ui_->resultLabel->setText(tr("Offset must be an aligned 32-bit hexadecimal value."));
        return false;
    }
    return true;
}

bool RegisterDialog::accessReady()
{
    if ((!source_ || !source_->isRunning()) && !debugHandle_)
    {
        ui_->resultLabel->setText(tr("Start GVFG capture or open an independent device handle first."));
        return false;
    }
    return true;
}

gvfg_status_t RegisterDialog::readRegisterValue(uint32_t offset, uint32_t *outValue) const
{
    if (source_ && source_->isRunning())
        return source_->readRegister(offset, outValue);
    return debugHandle_ ? gvfg_debug_read_register(debugHandle_, offset, outValue) : GVFG_ESTATE;
}

gvfg_status_t RegisterDialog::writeRegisterValue(uint32_t offset, uint32_t value) const
{
    if (source_ && source_->isRunning())
        return source_->writeRegister(offset, value);
    return debugHandle_ ? gvfg_debug_write_register(debugHandle_, offset, value) : GVFG_ESTATE;
}

void RegisterDialog::updateAccessState()
{
    const bool usingCapture = source_ && source_->isRunning();
    const bool usingIndependent = !usingCapture && debugHandle_;
    if (usingCapture)
        ui_->accessStatusLabel->setText(tr("Access: active GVFG capture handle (streaming)"));
    else if (usingIndependent)
        ui_->accessStatusLabel->setText(tr("Access: independent device handle (capture not started)"));
    else
        ui_->accessStatusLabel->setText(tr("Access: no device handle"));

    ui_->openButton->setEnabled(!usingCapture && !debugHandle_ && ui_->deviceCombo->count() > 0);
    ui_->closeDeviceButton->setEnabled(debugHandle_ != nullptr);
    ui_->deviceCombo->setEnabled(!usingCapture && !debugHandle_);
    ui_->refreshButton->setEnabled(!debugHandle_);
}

void RegisterDialog::readRegister()
{
    uint32_t offset = 0;
    updateAccessState();
    if (!validateOffset(offset) || !accessReady())
        return;

    uint32_t value = 0;
    const gvfg_status_t status = readRegisterValue(offset, &value);
    if (status != GVFG_OK)
    {
        const QString error = QString::fromUtf8(gvfg_strerror(status));
        ui_->resultLabel->setText(tr("Read failed: %1").arg(error));
        emit logMessage(QStringLiteral("[GVFG][REG] read offset=0x%1 failed: %2")
                            .arg(offset, 8, 16, QLatin1Char('0'))
                            .arg(error),
                        true);
        return;
    }

    ui_->readValueEdit->setText(QStringLiteral("0x%1").arg(value, 8, 16, QLatin1Char('0')).toUpper());
    ui_->resultLabel->setText(tr("Read succeeded."));
    emit logMessage(QStringLiteral("[GVFG][REG] read offset=0x%1 value=0x%2")
                        .arg(offset, 8, 16, QLatin1Char('0'))
                        .arg(value, 8, 16, QLatin1Char('0')),
                    false);
}

void RegisterDialog::writeRegister()
{
    uint32_t offset = 0;
    uint32_t value = 0;
    if (!validateOffset(offset))
        return;
    if (!parseHex32(ui_->writeValueEdit->text(), value))
    {
        ui_->resultLabel->setText(tr("Value must be a 32-bit hexadecimal value."));
        return;
    }
    updateAccessState();
    if (!accessReady())
        return;

    const QString question = tr("Write 0x%1 to register 0x%2?\n\nThis may disrupt active capture.")
                                 .arg(value, 8, 16, QLatin1Char('0'))
                                 .arg(offset, 8, 16, QLatin1Char('0'))
                                 .toUpper();
    if (QMessageBox::warning(this,
                             tr("Confirm Register Write"),
                             question,
                             QMessageBox::Yes | QMessageBox::No,
                             QMessageBox::No) != QMessageBox::Yes)
        return;

    const gvfg_status_t status = writeRegisterValue(offset, value);
    if (status != GVFG_OK)
    {
        const QString error = QString::fromUtf8(gvfg_strerror(status));
        ui_->resultLabel->setText(tr("Write failed: %1").arg(error));
        emit logMessage(QStringLiteral("[GVFG][REG] write offset=0x%1 value=0x%2 failed: %3")
                            .arg(offset, 8, 16, QLatin1Char('0'))
                            .arg(value, 8, 16, QLatin1Char('0'))
                            .arg(error),
                        true);
        return;
    }

    ui_->resultLabel->setText(tr("Write succeeded."));
    emit logMessage(QStringLiteral("[GVFG][REG] write offset=0x%1 value=0x%2")
                        .arg(offset, 8, 16, QLatin1Char('0'))
                        .arg(value, 8, 16, QLatin1Char('0')),
                    false);
}
