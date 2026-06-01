#pragma once

#include <QObject>
#include <QImage>
#include <QString>
#include <QStringList>

#include <gvfg_capture.h>

class GvfgSource : public QObject
{
    Q_OBJECT

public:
    explicit GvfgSource(QObject *parent = nullptr);
    ~GvfgSource() override;

    static QStringList enumerateDevices();

    bool start(void *previewHwnd, int deviceIndex, int previewBitDepthMode);
    bool setPreview(void *previewHwnd, int previewBitDepthMode);
    void stop();
    bool isRunning() const { return running_; }

    gvfg_signal_status_t signalStatus() const;
    gvfg_runtime_info_t runtimeInfo() const;
    gvfg_preview_info_t previewInfo() const;

signals:
    void frameReady(const QImage &image);
    void errorOccurred(const QString &message);

private:
    static void onFrame(const gvfg_frame_t *frame, void *user);
    static void onError(gvfg_status_t status, const char *message, void *user);

    gvfg_handle handle_ = nullptr;
    bool running_ = false;
};
