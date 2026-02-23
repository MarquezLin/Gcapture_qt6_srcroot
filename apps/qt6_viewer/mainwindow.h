#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QImage>
#include <QLabel>
#include <QDateTime>
#include <QString>
#include <QDockWidget>
#include <QTextEdit>
#include <gcapture.h>
#include <cstdint>
#include "inputinfodialog.h"
#include "displayinfodialog.h"
#include "procamp.h"
#include "info/capture_device_info.h"
#include "info/display_output_info.h"

#ifdef _WIN32
#include "capturesdk/capture_sdk_source.h"
#endif

QT_BEGIN_NAMESPACE
namespace Ui
{
    class MainWindow;
}
QT_END_NAMESPACE

class inputinfodialog;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void onStart();
    void onStop();
    void onRecord();
    void onShowEdid();
    void onFrameArrived(const QImage &);
    void onToggleDebugLog(bool checked);
    void appendDebugLog(const QString &line);
    void onOpenLogFolder();
    void onOpenRecordFolder();
    void onOpenSnapshot();
    void onShowInputInfo();
    void onShowDisplayInfo();
    void onSnapshot();

private:
    Ui::MainWindow *ui;
    QLabel *view_;
    gcap_handle h_{};
    int deviceIndex_ = 0;
    bool recording_ = false;

#ifdef _WIN32
    CaptureSdkSource *capSdk_ = nullptr;
    bool usingCaptureSdk_ = false;
#endif

    // ---- 實際輸入來源資訊（由每一幀 frame 更新）----
    uint64_t lastFramePtsNs_ = 0; // 最後一幀的 pts
    int lastFrameWidth_ = 0;      // 最後一幀寬
    int lastFrameHeight_ = 0;     // 最後一幀高
    double avgFps_ = 0.0;         // 平滑後的實際 FPS

    // ---- 錄影相關資訊（給 UI 顯示）----
    gcap_profile_t currentProfile_{}; // onStart 時記錄目前用的 profile
    QDateTime recordStartTime_;       // 開始錄影時間（用來估算 bitrate）
    QString recordPath_;              // 錄影檔完整路徑
    QString recordEncoderName_;       // encoder 名稱（H.264/HEVC …）
    QImage lastFrameImage_;
    QString recordAudioDeviceIdUtf8_;

    // ---- GPU / Adapter 選擇（給 NV12→RGBA 用）----
    int gpuIndex_ = -1; // 對應 DXGI EnumAdapters1 的 index，-1 = default
    QString gpuName_;   // UI 上顯示的 GPU 名稱（與 ComboBox 同步）

    // ---- Debug Log 視窗 ----
    QDockWidget *debugDock_ = nullptr;
    QTextEdit *debugText_ = nullptr;

    static void s_vcb(const gcap_frame_t *f, void *u);
    static void s_ecb(gcap_status_t c, const char *m, void *u);
    // === 全專案共用的集中 log 入口（UI + DLL callback 都用這個）===
    static void postLog(const QString &line, bool isError = false);

    inputinfodialog *infoDlg_ = nullptr;
    DisplayInfoDialog *DpinfoDlg_ = nullptr;
    ProcAmp *procampDlg_ = nullptr;
    gcap_procamp_t m_currentProcAmp{};
    QString lastInfoText_;
    QString lastCapturePropsText_;
    CaptureDeviceInfo captureInfo_;
    DisplayOutputInfo displayInfo_;

    qint64 lastPropsQueryMs_ = 0;

signals:
    void sigFrame(const QImage &);
};
#endif // MAINWINDOW_H
