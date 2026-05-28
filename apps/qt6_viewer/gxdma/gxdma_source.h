#pragma once

#include <QObject>
#include <QImage>
#include <QString>
#include <QStringList>

#include <gxdma_capture.h>

class GxdmaSource : public QObject
{
    Q_OBJECT

public:
    explicit GxdmaSource(QObject *parent = nullptr);
    ~GxdmaSource() override;

    static QStringList enumerateDevices();

    bool start(void *previewHwnd, int deviceIndex, int previewBitDepthMode);
    bool setPreview(void *previewHwnd, int previewBitDepthMode);
    void stop();
    bool isRunning() const { return running_; }

    gxdma_signal_status_t signalStatus() const;
    gxdma_runtime_info_t runtimeInfo() const;
    gxdma_preview_info_t previewInfo() const;

signals:
    void frameReady(const QImage &image);
    void errorOccurred(const QString &message);

private:
    static void onFrame(const gxdma_frame_t *frame, void *user);
    static void onError(gxdma_status_t status, const char *message, void *user);

    gxdma_handle handle_ = nullptr;
    bool running_ = false;
};
