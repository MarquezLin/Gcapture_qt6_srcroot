#pragma once
#include <QString>
#include <QStringList>
#include <gcapture.h>

struct CaptureDeviceInfo
{
    gcap_signal_status_t signal{};       // best-effort signal info
    gcap_signal_status_t signalProbe{};  // raw DShow/MF probe result
    gcap_signal_status_t negotiated{};   // actual format delivered to app
    QString deviceName;
    QString backendName;
    QString frameSource;
    QString pathName;
    QString captureFormat;
    QString renderFormat;
    QString audioInfo;
    QStringList supportedFormats;
    QStringList propertyPages;
    QString driverVersion;
    QString firmwareVersion;
    QString serialNumber;

    bool hasSignal() const { return signal.width > 0 && signal.height > 0; }
    bool hasSignalProbe() const { return signalProbe.width > 0 && signalProbe.height > 0; }
    bool hasNegotiated() const { return negotiated.width > 0 && negotiated.height > 0; }
};

QString formatCaptureDeviceInfo(const CaptureDeviceInfo &info, double fallbackFps);
