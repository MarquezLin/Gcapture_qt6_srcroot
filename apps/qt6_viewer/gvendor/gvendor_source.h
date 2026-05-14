#pragma once

#include <QObject>
#include <QImage>
#include <QMutex>
#include <QString>
#include <cstdint>
#include <atomic>
#include <thread>

#include "gvendor.h"

// A Qt-friendly wrapper for the vendor-direct gvendor backend.
// - Frames are copied into QImage and delivered via the frameReady() signal.
class GVendorSource : public QObject
{
    Q_OBJECT

public:
    explicit GVendorSource(QObject *parent = nullptr);
    ~GVendorSource() override;

    bool isLoaded() const;
    QString lastError() const;
    QString deviceName() const;

    // Start capturing.
    // width/height: pass 0,0 for "SDK default"; wrapper will fallback if needed.
    bool start(int width = 0, int height = 0);
    void stop();

signals:
    void frameReady(const QImage &img);
    void errorOccurred(const QString &msg);

private:
    void setError(const QString &msg);

    void captureLoop();
    void onFrame(const gv_frame_t &frame);
    QImage convertFrameToImage(const gv_frame_t &frame) const;

    gv_handle handle_ = nullptr;
    bool capturing_ = false;
    QString deviceName_;

    std::atomic<bool> running_{false};
    std::thread pumpThread_;

    mutable QMutex mtx_;
    QString lastError_;
};
