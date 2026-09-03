#pragma once

#include <QDialog>

#include <gvfg_capture.h>

#include <cstdint>

class GvfgSource;

namespace Ui
{
class RegisterDialog;
}

class RegisterDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit RegisterDialog(GvfgSource *source, QWidget *parent = nullptr);
    ~RegisterDialog() override;

signals:
    void logMessage(const QString &message, bool isError);

private slots:
    void refreshDevices();
    void openDevice();
    void closeDevice();
    void readRegister();
    void writeRegister();

private:
    static bool parseHex32(const QString &text, uint32_t &outValue);
    bool validateOffset(uint32_t &outOffset);
    bool accessReady();
    gvfg_status_t readRegisterValue(uint32_t offset, uint32_t *outValue) const;
    gvfg_status_t writeRegisterValue(uint32_t offset, uint32_t value) const;
    void updateAccessState();

    Ui::RegisterDialog *ui_ = nullptr;
    GvfgSource *source_ = nullptr;
    gvfg_handle debugHandle_ = nullptr;
};
