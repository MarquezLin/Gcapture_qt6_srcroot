#pragma once
#include <QString>
#include <QStringList>
#include <gcapture.h>

struct CaptureDeviceInfo
{
    gcap_signal_status_t signal{}; // negotiated input signal
    QString driverVersion;
    QString firmwareVersion;
    QString serialNumber;

    bool hasSignal() const
    {
        return signal.width > 0 && signal.height > 0;
    }
};

QString formatCaptureDeviceInfo(const CaptureDeviceInfo &info, double fallbackFps);
