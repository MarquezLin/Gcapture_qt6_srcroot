#include "mainwindow.h"

#include "./ui_mainwindow.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QMessageBox>
#include <QStringList>
#include <cstring>

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
        case GCAP_FMT_V210:
            return "V210";
        case GCAP_FMT_ARGB:
            return "ARGB";
        default:
            return "UNKNOWN";
        }
    }
} // namespace

void MainWindow::applyInitialPreviewSizeFromSource(int width, int height)
{
    if (initialPreviewSizeApplied_ || width <= 0 || height <= 0 || !previewWindow_)
        return;

    // Only auto-size once after opening capture, using the actual frame size
    // delivered by the backend callback. Later source mode changes should not
    // resize the window; users can manually resize the preview as needed.
    const QSize appliedContent = previewWindow_->resizeToSourceContent(width, height);
    initialPreviewSizeApplied_ = true;

    MainWindow::postLog(QStringLiteral("[Preview] initial window size applied from source=%1x%2 content=%3x%4 window=%5x%6")
                            .arg(width)
                            .arg(height)
                            .arg(appliedContent.width())
                            .arg(appliedContent.height())
                            .arg(previewWindow_->width())
                            .arg(previewWindow_->height()));
}

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
    Q_UNUSED(img);
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

QString MainWindow::buildSnapshotBasePath() const
{
    const QString baseDir = QCoreApplication::applicationDirPath() + "/snapshots";
    QDir().mkpath(baseDir);

    const QDateTime now = QDateTime::currentDateTime();
    const QString ts = now.toString("yyyyMMdd_HHmmss_zzz");
    return baseDir + "/" + QStringLiteral("snapshot_%1").arg(ts);
}

QString MainWindow::buildSnapshotPath() const
{
    return buildSnapshotBasePath() + ".png";
}

bool MainWindow::saveSnapshotImage(QString *outPath, const QString &fullPath)
{
    if (lastFrameImage_.isNull())
        return false;

    const QString finalPath = fullPath.isEmpty() ? buildSnapshotPath() : fullPath;
    const bool ok = lastFrameImage_.save(finalPath, "PNG");
    if (ok && outPath)
        *outPath = finalPath;
    return ok;
}

bool MainWindow::saveSceneExports(const QString &basePath, gcap_snapshot_export_result_t *result)
{
    if (!h_ || basePath.isEmpty() || !result)
        return false;

    constexpr bool kSnapshotTiffExportEnabled = true;

    const QByteArray baseUtf8 = QDir::toNativeSeparators(basePath).toUtf8();
    gcap_snapshot_export_desc_t desc{};
    desc.base_path_utf8 = baseUtf8.constData();
    desc.flags = GCAP_EXPORT_RAW_NATIVE | GCAP_EXPORT_RAW_GIGABYTE_HEADER | GCAP_EXPORT_RAW_RGB10_U16 | GCAP_EXPORT_RAW_RGBA8 | GCAP_EXPORT_STATS | GCAP_EXPORT_PNG;
    if (kSnapshotTiffExportEnabled)
        desc.flags |= GCAP_EXPORT_TIFF;

    std::memset(result, 0, sizeof(*result));
    const gcap_status_t st = gcap_export_snapshot(h_, &desc, result);
    return st == GCAP_OK;
}

void MainWindow::onFrameArrived(const QImage &img)
{
    lastFrameImage_ = img;
#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
    if (usingGvfg_)
    {
        if (!img.isNull())
            applyInitialPreviewSizeFromSource(img.width(), img.height());
        return;
    }
#endif
    if (!img.isNull() && previewWindow_)
        previewWindow_->setFrame(img);
    if (!img.isNull())
        applyInitialPreviewSizeFromSource(img.width(), img.height());
}

void MainWindow::onSnapshot()
{
#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
    if (usingGvfg_)
    {
        if (!gvfg_ || !gvfg_->isRunning())
        {
            QMessageBox::information(this, "Snapshot",
                                     "There is currently no screenshot available (please start capturing first).");
            return;
        }

        QString error;
        const QImage snapshot = gvfg_->captureSnapshot(2000, &error);
        if (snapshot.isNull())
        {
            QMessageBox::warning(this, "Snapshot",
                                 QStringLiteral("GVFG snapshot failed.\n%1").arg(error));
            return;
        }

        const QString pngPath = buildSnapshotPath();
        if (!snapshot.save(pngPath, "PNG"))
        {
            QMessageBox::warning(this, "Snapshot",
                                 QStringLiteral("Snapshot save failed.\nPath: %1").arg(pngPath));
            return;
        }

        lastFrameImage_ = snapshot;
        if (ui->statusbar)
            ui->statusbar->showMessage(QStringLiteral("Snapshot saved: %1").arg(QFileInfo(pngPath).fileName()), 6000);
        QMessageBox::information(this, "Snapshot",
                                 QStringLiteral("Saved files:\n%1").arg(pngPath));
        return;
    }
#endif

    if (lastFrameImage_.isNull())
    {
        QMessageBox::information(this, "Snapshot",
                                 "There is currently no screenshot available (please start capturing first).");
        return;
    }

    const QString basePath = buildSnapshotBasePath();
    gcap_snapshot_export_result_t result{};
    const bool sceneOk = saveSceneExports(basePath, &result);

    QString pngPath = result.png_path[0] ? QString::fromUtf8(result.png_path) : QString();
    bool pngOk = !pngPath.isEmpty();
    if (!pngOk)
    {
        pngPath = basePath + ".png";
        pngOk = saveSnapshotImage(&pngPath, pngPath);
    }

    if (!pngOk && !sceneOk)
    {
        QMessageBox::warning(this, "Snapshot",
                             QStringLiteral("Snapshot / scene export failed.\nBase path: %1").arg(basePath));
        return;
    }

    QStringList saved;
    const auto appendPath = [&saved](const char *p)
    {
        if (p && p[0])
            saved << QString::fromUtf8(p);
    };

    if (pngOk)
        saved << pngPath;

    if (sceneOk)
    {
        appendPath(result.native_raw_path);
        appendPath(result.gigabyte_native_raw_path);
        appendPath(result.rgb10_u16_path);
        appendPath(result.rgba8_path);
        appendPath(result.tiff_path);
        appendPath(result.stats_path);

        const QString source = result.source_bit_depth >= 10 ? QStringLiteral("10-bit") : QStringLiteral("8-bit");
        QStringList logParts;
        logParts << QStringLiteral("source=%1").arg(source)
                 << QStringLiteral("PNG=%1").arg(pngPath)
                 << QStringLiteral("nativeRAW=%1").arg(QString::fromUtf8(result.native_raw_path))
                 << QStringLiteral("gigaNativeRAW=%1").arg(QString::fromUtf8(result.gigabyte_native_raw_path))
                 << QStringLiteral("RGB10=%1").arg(QString::fromUtf8(result.rgb10_u16_path))
                 << QStringLiteral("RGBA8=%1").arg(QString::fromUtf8(result.rgba8_path))
                 << QStringLiteral("TIFF=%1").arg(result.tiff_path[0] ? QString::fromUtf8(result.tiff_path) : QStringLiteral("disabled"))
                 << QStringLiteral("STATS=%1").arg(QString::fromUtf8(result.stats_path))
                 << QStringLiteral("flags=0x%1").arg(QString::number(result.generated_flags, 16));
        MainWindow::postLog(QStringLiteral("[SceneExport] %1").arg(logParts.join(QStringLiteral(" "))));
    }

    if (ui->statusbar)
    {
        const QString statusFile = sceneOk && result.native_raw_path[0]
                                       ? QFileInfo(QString::fromUtf8(result.native_raw_path)).fileName()
                                       : QFileInfo(pngPath).fileName();
        ui->statusbar->showMessage(
            sceneOk
                ? QStringLiteral("Snapshot + scene export saved: %1").arg(statusFile)
                : QStringLiteral("Snapshot saved: %1").arg(statusFile),
            6000);
    }

    saved.removeDuplicates();
    QMessageBox::information(this, "Snapshot",
                             QStringLiteral("Saved files:\n%1").arg(saved.join("\n")));
}
