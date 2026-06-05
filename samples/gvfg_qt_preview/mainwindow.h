#pragma once

#include <gvfg_capture.h>

#include <QString>
#include <QWidget>

#include <array>
#include <atomic>
#include <cstdint>

class QCloseEvent;
class PreviewWindow;
class QTimer;

QT_BEGIN_NAMESPACE
namespace Ui
{
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow final : public QWidget
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void refreshDevices();
    void showPreviewWindow();
    bool openDevice();
    void closeDevice();
    void startCapture();
    void stopCapture();
    bool applyPreview();
    void updateSignalStatus(bool writeLog);
    void updateUiState();
    void showError(const QString &apiName, gvfg_status_t status);
    void appendLog(const QString &message);

    static void onFrame(const gvfg_frame_t *frame, void *user);
    static void onEvent(const gvfg_event_t *event, void *user);
    static void onError(gvfg_status_t status, const char *message, void *user);

    Ui::MainWindow *ui_ = nullptr;
    PreviewWindow *previewWindow_ = nullptr;
    std::array<gvfg_device_info_t, GVFG_MAX_DEVICES> devices_{};
    int deviceCount_ = 0;
    gvfg_handle handle_ = nullptr;
    bool captureRunning_ = false;
    std::atomic<uint64_t> frameCount_{0};
    QTimer *signalStatusTimer_ = nullptr;
    QString lastSignalStatusText_;
    QString lastFpgaRawStateKey_;
};
