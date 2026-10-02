#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QImage>
#include <QDateTime>
#include <QString>
#include <QDockWidget>
#include <QTimer>
#include <QPlainTextEdit>
#include <QByteArray>
#include <QFrame>
#include <QMargins>
#include <gcapture.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>
#include "backend/capture_backend.h"
#include "state/raw_snapshot_buffer.h"
#include "state/capture_runtime_tracker.h"
#include "info/capture_device_info.h"
#include "info/display_output_info.h"

class CaptureBackendCatalog;
class CaptureSource;
class DirectShowSource;
class GvfgSource;

QT_BEGIN_NAMESPACE
namespace Ui
{
    class MainWindow;
}
QT_END_NAMESPACE

class inputinfodialog;
class DisplayInfoDialog;
class ProcAmp;
class previewwindow;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

protected:
    void closeEvent(QCloseEvent *event) override;

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
    void onOpenRawInspect();

    void on_btnPreview_clicked();

private:
    Ui::MainWindow *ui;
    std::unique_ptr<DirectShowSource> directShow_;
    int deviceIndex_ = 0;
    bool recording_ = false;
    bool audioMonitoringActive_ = false;
    std::vector<std::unique_ptr<CaptureBackendCatalog>> backendCatalogs_;
#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
    GvfgSource *gvfg_ = nullptr;
#endif

    CaptureRuntimeTracker runtime_;

    // Recording state shown in the UI.
    gcap_profile_t currentProfile_{};
    QDateTime recordStartTime_;
    QString recordPath_;
    QString recordEncoderName_;
    QImage lastFrameImage_;

    // Debug log panel.
    QDockWidget *debugDock_ = nullptr;
    QPlainTextEdit *debugText_ = nullptr;

    static void s_vcb(const gcap_frame_t *f, void *u);
    static void s_pcb(const gcap_frame_packet_t *pkt, void *u);
    static void s_ecb(gcap_status_t c, const char *m, void *u);
    // Shared log entry point for UI and DLL callbacks.
    static void postLog(const QString &line, bool isError = false);

    inputinfodialog *signalInfoDialog_ = nullptr;
    DisplayInfoDialog *displayInfoDialog_ = nullptr;
    ProcAmp *procAmpDialog_ = nullptr;
    gcap_procamp_t procAmpSettings_{};
    QString lastInfoText_;
    CaptureDeviceInfo captureInfo_;
    DisplayOutputInfo displayInfo_;
    previewwindow *previewWindow_ = nullptr;
    class RawInspectorDialog *rawInspectorDlg_ = nullptr;

    qint64 lastPropsQueryMs_ = 0;
    std::optional<CaptureBackend> cachedDeviceCapsBackend_;
    int cachedDeviceCapsIndex_ = -1;
    QStringList cachedSupportedFormats_;
    QStringList cachedPropertyPages_;
    RawSnapshotBuffer rawSnapshot_;
    bool previewFullscreen_ = false;
    bool previewRestoreMaximized_ = false;
    QMargins previewPanelMargins_;
    int previewPanelSpacing_ = 10;
    QFrame::Shape previewPanelFrameShape_ = QFrame::StyledPanel;
    bool debugDockWasVisible_ = false;
    QTimer *runtimeStatusTimer_ = nullptr;
    QString lastRuntimeStatusText_;
    QString lastGvfgRawStateKey_;
    bool suppressAuxDialogRefresh_ = false;
    bool openingRawInspector_ = false;
    QString lastPixelFormatWarningKey_;
    bool initialPreviewSizeApplied_ = false;
    void updateRuntimeStatusUi();
    void updateBrandDashboard();
    void applyInitialPreviewSizeFromSource(int width, int height);
    int currentDeviceIndex() const;
    QString currentDeviceText() const;
    void ensureSignalInfoDialog();
    void ensureDisplayInfoDialog();
    void showAndActivateDialog(QWidget *dialog);
    void refreshDirectShowCaptureInfo(bool throttleDeviceProps);
    void refreshDirectShowDisplayPipeline();
    CaptureBackend selectedBackend() const;
    CaptureBackendCatalog *backendCatalog(CaptureBackend backend) const;
    CaptureSource *activeCaptureSource() const;
    CaptureSource *selectedCaptureSource() const;
    void ensureDeviceCapabilityCache(int deviceIndex);
    void invalidateDeviceCapabilityCache();
    void updateCapabilityLabel();
    void refreshCaptureInfoFromSdkAndRuntime(bool throttleDeviceProps);
    void refreshDisplayInfoFromFrame(const QImage &img);
    void refreshDisplayInfoFromCurrentState();
    void refreshSignalInfoDialog();
    void refreshDisplayInfoDialog();
    void setupPreviewWindow();
    void setPreviewFullscreen(bool enabled);
    void setupRuntimeStatusTimer();
    void setupDebugDock();
    void setupProcAmpAction();
    void setupRegisterTools();
    void setInspectorTools();
    void setupBackendControls();
    void setupPreviewBitDepthControls();
    int selectedPreviewBitDepthMode() const;
    QString selectedPreviewBitDepthText() const;
    void applyPreviewSettingsToActiveSession();
    void refreshPixelFormatOptions(bool showFailurePrompt = false);
    void notifyPixelFormatEnumerationFailure(CaptureBackend backend);
    void initializeDeviceList();
    void setupConnections();
#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
    void refreshGvfgMonitoring();
    void refreshGvfgCaptureInfo();
    void refreshGvfgDisplayPipeline();
    bool startGvfgCapture(void *previewHwnd);
    void toggleGvfgRecording();
    void stopGvfgRecording(bool showSummary);
#endif
    void logStartupInfo();
    void resetRuntimeTracking();
    void clearPreviewSurface();
    void closeCaptureSession();
    void startAudioMonitoring();
    void stopAudioMonitoring();
    bool startDirectShowCapture(void *previewHwnd);
    void toggleDirectShowRecording();
    void stopDirectShowRecording(bool showSummary);
    void setAudioMonitoringStatus(const QString &status, const QString &detail = QString());
    bool showCaptureErrorAndClose(const QString &action, gcap_status_t st, const char *apiName = nullptr);
    void stopRecordingSession(bool showSummary);
    QString chooseRecordingPath(const QDateTime &suggestedTime);
    void dispatchFrameImage(const QImage &img);
    void refreshFrameDependentUi(const QImage &img);
    void logFramePacketIfNeeded(const gcap_frame_packet_t &pkt);
    QString buildSnapshotPath() const;
    QString buildSnapshotBasePath() const;
    bool saveSnapshotImage(QString *outPath, const QString &fullPath = QString());
    bool saveSceneExports(const QString &basePath, gcap_snapshot_export_result_t *result);
    QString saveLatestSourceRaw(const QString &basePath);

signals:
    void sigFrame(const QImage &);
};
#endif // MAINWINDOW_H
