#pragma once

#include <gxdma_capture.h>

#include <QWidget>

#include <array>
#include <atomic>
#include <cstdint>

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

private:
    void refreshDevices();
    void startCapture();
    void stopCapture();
    void updateSignalStatus();
    void setRunningUi(bool running);
    void showError(const QString &apiName, gxdma_status_t status);
    void appendLog(const QString &message);

    static void onFrame(const gxdma_frame_t *frame, void *user);
    static void onError(gxdma_status_t status, const char *message, void *user);

    Ui::MainWindow *ui_ = nullptr;
    std::array<gxdma_device_info_t, GXDMA_MAX_DEVICES> devices_{};
    int deviceCount_ = 0;
    gxdma_handle handle_ = nullptr;
    std::atomic<uint64_t> frameCount_{0};
};
