#include "mainwindow.h"

#include "./ui_mainwindow.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>

namespace
{
static const char *packetFmtNameFrame(int fmt)
{
    switch (fmt)
    {
    case GCAP_FMT_NV12:
        return "NV12";
    case GCAP_FMT_P010:
        return "P010";
    case GCAP_FMT_YUY2:
        return "YUY2";
    case GCAP_FMT_Y210:
        return "Y210";
    case GCAP_FMT_ARGB:
        return "ARGB";
    default:
        return "UNKNOWN";
    }
}
} // namespace

void MainWindow::updateFrameSourceState(uint64_t ptsNs, int width, int height, uint64_t &lastPtsTracker)
{
    lastFramePtsNs_ = ptsNs;
    lastFrameWidth_ = width;
    lastFrameHeight_ = height;

    if (lastPtsTracker != 0 && lastFramePtsNs_ > lastPtsTracker)
    {
        const uint64_t delta = lastFramePtsNs_ - lastPtsTracker;
        const double fps = 1e9 / double(delta);
        if (fps > 0.0)
            avgFps_ = (avgFps_ <= 0.0) ? fps : (avgFps_ * 0.9 + fps * 0.1);
    }
    lastPtsTracker = lastFramePtsNs_;
}

void MainWindow::dispatchFrameImage(const QImage &img)
{
    sigFrame(img.copy());
}

void MainWindow::refreshFrameDependentUi(const QImage &img)
{
    if (previewWindow_)
        previewWindow_->setFrame(img);

    updateRuntimeStatusUi();
    refreshCaptureInfoFromSdkAndRuntime(true);
    refreshDisplayInfoFromFrame(img);

    if (infoDlg_)
        infoDlg_->setInfoText(lastInfoText_);

    if (DpinfoDlg_)
        refreshDisplayInfoDialog();
}

void MainWindow::logFramePacketIfNeeded(const gcap_frame_packet_t &pkt)
{
    ++framePacketLogCount_;
    const bool shouldLogPacket = (framePacketLogCount_ <= 5) || ((framePacketLogCount_ % 60) == 0);
    if (!shouldLogPacket)
        return;

    const auto sourceName = [](int s) -> const char *
    {
        switch (s)
        {
        case GCAP_SOURCE_DSHOW_RAWSINK:
            return "DShowRawSink";
        case GCAP_SOURCE_DSHOW_RENDERER:
            return "DShowRenderer";
        case GCAP_SOURCE_WINMF_GPU:
            return "WinMFGPU";
        case GCAP_SOURCE_WINMF_CPU:
            return "WinMFCPU";
        default:
            return "Unknown";
        }
    };

    const QString line = QStringLiteral("[FramePacket] session=%1 #%2 backend=%3 source=%4 fmt=%5 %6x%7 planes=%8 gpu=%9 pts=%10")
                             .arg(framePacketSessionId_)
                             .arg(framePacketLogCount_)
                             .arg(pkt.backend)
                             .arg(QString::fromLatin1(sourceName(pkt.source_kind)))
                             .arg(QString::fromLatin1(packetFmtNameFrame(pkt.format)))
                             .arg(pkt.width)
                             .arg(pkt.height)
                             .arg(pkt.plane_count)
                             .arg(pkt.gpu_backed)
                             .arg(QString::number(pkt.pts_ns));
    MainWindow::postLog(line);
}

QString MainWindow::buildSnapshotPath() const
{
    const QString baseDir = QCoreApplication::applicationDirPath() + "/snapshots";
    QDir().mkpath(baseDir);

    const QDateTime now = QDateTime::currentDateTime();
    const QString ts = now.toString("yyyyMMdd_HHmmss_zzz");
    return baseDir + "/" + QStringLiteral("snapshot_%1.png").arg(ts);
}

bool MainWindow::saveSnapshotImage(QString *outPath)
{
    if (lastFrameImage_.isNull())
        return false;

    const QString fullPath = buildSnapshotPath();
    const bool ok = lastFrameImage_.save(fullPath, "PNG");
    if (ok && outPath)
        *outPath = fullPath;
    return ok;
}

void MainWindow::onFrameArrived(const QImage &img)
{
    lastFrameImage_ = img;

    // 這條路徑是 callback/QImage 顯示入口：
    // - CaptureSDK frameReady
    // - MainWindow::sigFrame（例如 packet callback / CPU fallback）
    // 真正的 native GPU preview 不一定會經過這裡。
    refreshFrameDependentUi(img);
}

void MainWindow::onSnapshot()
{
    if (lastFrameImage_.isNull())
    {
        QMessageBox::information(this, "Snapshot",
                                 "There is currently no screenshot available (please start capturing first).");
        return;
    }

    QString fullPath;
    const bool ok = saveSnapshotImage(&fullPath);
    const QString fileName = QFileInfo(fullPath).fileName();
    if (!ok)
    {
        QMessageBox::warning(this, "Snapshot",
                             QString("Snapshot storage failed:%1").arg(fullPath));
        return;
    }

    if (ui->statusbar)
    {
        ui->statusbar->showMessage(
            QStringLiteral("Snapshot saved: %1").arg(fileName),
            5000);
    }

    QMessageBox::information(this, "Snapshot",
                             QString("Snapshot already saved:\n%1").arg(fullPath));
}
