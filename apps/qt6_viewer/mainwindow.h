#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QImage>
#include <QLabel>
#include <QDateTime>
#include <QString>
#include <QDockWidget>
#include <QTimer>
#include <QPlainTextEdit>
#include <QByteArray>
#include <QFrame>
#include <QMargins>
#include <QMutex>
#include <QWaitCondition>
#include <gcapture.h>
#include <cstdint>
#include "inputinfodialog.h"
#include "displayinfodialog.h"
#include "procamp.h"
#include "info/capture_device_info.h"
#include "info/display_output_info.h"
#include "previewwindow.h"

#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
#include "gvfg/gvfg_source.h"
static constexpr int kQtViewerGvfgBackend = 100;
#endif

QT_BEGIN_NAMESPACE
namespace Ui
{
    class MainWindow;
}
QT_END_NAMESPACE

class inputinfodialog;
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
    // QLabel *view_;
    gcap_handle h_{};
    int deviceIndex_ = 0;
    bool recording_ = false;
    bool previewAudioActive_ = false;
#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
    GvfgSource *gvfg_ = nullptr;
    bool usingGvfg_ = false;
#endif

    // Frame-source state updated by arriving frames.
    uint64_t lastFramePtsNs_ = 0;
    int lastFrameWidth_ = 0;
    int lastFrameHeight_ = 0;
    double avgFps_ = 0.0;

    // Recording state shown in the UI.
    gcap_profile_t currentProfile_{};
    QDateTime recordStartTime_;
    QString recordPath_;
    QString recordEncoderName_;
    QImage lastFrameImage_;
    QString selectedAudioDeviceIdUtf8_;

    // Debug log panel.
    QDockWidget *debugDock_ = nullptr;
    QPlainTextEdit *debugText_ = nullptr;

    static void s_vcb(const gcap_frame_t *f, void *u);
    static void s_pcb(const gcap_frame_packet_t *pkt, void *u);
    static void s_ecb(gcap_status_t c, const char *m, void *u);
    // Shared log entry point for UI and DLL callbacks.
    static void postLog(const QString &line, bool isError = false);

    inputinfodialog *infoDlg_ = nullptr;
    DisplayInfoDialog *DpinfoDlg_ = nullptr;
    ProcAmp *procampDlg_ = nullptr;
    gcap_procamp_t m_currentProcAmp{};
    QString lastInfoText_;
    QString lastCapturePropsText_;
    CaptureDeviceInfo captureInfo_;
    DisplayOutputInfo displayInfo_;
    previewwindow *previewWindow_ = nullptr;
    class RawInspectorDialog *rawInspectorDlg_ = nullptr;

    qint64 lastPropsQueryMs_ = 0;
    int cachedDeviceCapsBackend_ = -1;
    int cachedDeviceCapsIndex_ = -1;
    QStringList cachedSupportedFormats_;
    QStringList cachedPropertyPages_;
    bool rawPacketCallbackEnabled_ = false;
    uint64_t lastVideoCallbackPtsNs_ = 0;
    uint64_t lastPacketCallbackPtsNs_ = 0;
    uint64_t framePacketLogCount_ = 0;
    uint64_t framePacketSessionId_ = 0;
    QMutex rawSnapshotMutex_;
    QWaitCondition rawSnapshotCv_;
    bool rawSnapshotPending_ = false;
    QByteArray latestRawFrame_;
    int latestRawWidth_ = 0;
    int latestRawHeight_ = 0;
    int latestRawStride_ = 0;
    int latestRawFormat_ = -1;
    uint64_t lastWatchdogFrameCounter_ = 0;
    int frameStallTicks_ = 0;
    bool frameStallWarningActive_ = false;
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
    QString currentAudioInfoText() const;
    void ensureSignalInfoDialog();
    void ensureDisplayInfoDialog();
    void showAndActivateDialog(QWidget *dialog);
    void refreshCaptureRuntimeInfo();
    void refreshCaptureDeviceProps(bool throttleDeviceProps);
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
    void setupBackendControls();
    void setupPreviewBitDepthControls();
    int selectedPreviewBitDepthMode() const;
    QString selectedPreviewBitDepthText() const;
    void applyPreviewSettingsToActiveSession();
    void refreshPixelFormatOptions(bool showFailurePrompt = false);
    void notifyPixelFormatEnumerationFailure(int backend);
    void initializeDeviceList();
    void setupConnections();
#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
    void refreshGvfgMonitoring();
#endif
    void logStartupInfo();
    void resetRuntimeTracking();
    void clearPreviewSurface();
    void closeCaptureSession();
    void startPreviewAudio();
    void stopPreviewAudio();
    bool showCaptureErrorAndClose(const QString &action, gcap_status_t st, const char *apiName = nullptr);
    void stopRecordingSession(bool showSummary);
    QString buildRecordingPath(const QDateTime &now) const;
    void applySelectedRecordingAudioDevice();
    void updateFrameSourceState(uint64_t ptsNs, int width, int height, uint64_t &lastPtsTracker);
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
