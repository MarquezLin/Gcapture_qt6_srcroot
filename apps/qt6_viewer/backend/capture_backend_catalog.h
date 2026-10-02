#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <memory>
#include "capture_backend.h"

struct CaptureBackendDevice
{
    QString name;
    int index = -1;
};

struct CaptureBackendFormat
{
    QString label;
    int value = -1;
};

struct CaptureBackendFormats
{
    QList<CaptureBackendFormat> items;
    bool querySucceeded = true;
};

struct CaptureBackendCapabilities
{
    QStringList formats;
    QStringList propertyPages;
};

class CaptureBackendCatalog
{
public:
    virtual ~CaptureBackendCatalog() = default;

    virtual CaptureBackend backend() const = 0;
    virtual QString displayName() const = 0;
    virtual QList<CaptureBackendDevice> devices() = 0;
    virtual CaptureBackendFormats formats(int deviceIndex) = 0;
    virtual CaptureBackendCapabilities capabilities(int deviceIndex) = 0;
    virtual QString formatToolTip() const = 0;
};

std::unique_ptr<CaptureBackendCatalog> createDirectShowBackendCatalog();

#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
std::unique_ptr<CaptureBackendCatalog> createGvfgBackendCatalog();
#endif
