#include "mainwindow.h"

#include "./ui_mainwindow.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QMessageBox>
#include <QFile>
#include <QtEndian>
#include <vector>

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

static bool savePngFromRgba10Raw(const QString &rawPath, const QString &pngPath, int width, int height)
{
    if (width <= 0 || height <= 0)
        return false;

    QFile f(rawPath);
    if (!f.open(QIODevice::ReadOnly))
        return false;

    const qsizetype pixelCount = static_cast<qsizetype>(width) * static_cast<qsizetype>(height);
    const qsizetype wordCount = pixelCount * 4;
    const qint64 expectedBytes = static_cast<qint64>(wordCount) * static_cast<qint64>(sizeof(quint16));
    if (f.size() < expectedBytes)
        return false;

    const QByteArray payload = f.read(expectedBytes);
    if (payload.size() < expectedBytes)
        return false;

    QImage img(width, height, QImage::Format_RGB888);
    if (img.isNull())
        return false;

    const auto *src = reinterpret_cast<const quint16 *>(payload.constData());
    for (int y = 0; y < height; ++y)
    {
        uchar *dst = img.scanLine(y);
        const qsizetype rowBase = static_cast<qsizetype>(y) * static_cast<qsizetype>(width) * 4;
        for (int x = 0; x < width; ++x)
        {
            const qsizetype si = rowBase + static_cast<qsizetype>(x) * 4;
            const quint16 r16 = qFromLittleEndian(src[si + 0]);
            const quint16 g16 = qFromLittleEndian(src[si + 1]);
            const quint16 b16 = qFromLittleEndian(src[si + 2]);
            dst[x * 3 + 0] = static_cast<uchar>((static_cast<unsigned>(r16) * 255u + 32767u) / 65535u);
            dst[x * 3 + 1] = static_cast<uchar>((static_cast<unsigned>(g16) * 255u + 32767u) / 65535u);
            dst[x * 3 + 2] = static_cast<uchar>((static_cast<unsigned>(b16) * 255u + 32767u) / 65535u);
        }
    }
    return img.save(pngPath, "PNG");
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

bool MainWindow::saveRgb10Exports(const QString &basePath, QString *rawPath, QString *tiffPath, QString *statsPath)
{
    if (!h_ || basePath.isEmpty())
        return false;

    const QByteArray baseUtf8 = QDir::toNativeSeparators(basePath).toUtf8();
    const gcap_status_t st = gcap_export_preview_scene_rgb10(h_, baseUtf8.constData(), 1, 1, 1);
    if (st != GCAP_OK)
        return false;

    if (rawPath)
        *rawPath = basePath + "_rgba16_expanded.raw";
    if (tiffPath)
        *tiffPath = basePath + ".tiff";
    if (statsPath)
        *statsPath = basePath + ".stats.txt";
    return true;
}

void MainWindow::onFrameArrived(const QImage &img)
{
    lastFrameImage_ = img;

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (previewWindow_ && previewWindow_->isVisible() && (lastPreviewPushMs_ == 0 || (nowMs - lastPreviewPushMs_) >= 33))
    {
        previewWindow_->setFrame(img);
        lastPreviewPushMs_ = nowMs;
    }
}



void MainWindow::onSnapshot()
{
    if (lastFrameImage_.isNull())
    {
        QMessageBox::information(this, "Snapshot",
                                 "There is currently no screenshot available (please start capturing first).");
        return;
    }

    const QString basePath = buildSnapshotBasePath();
    QString rawPath, tiffPath, statsPath;
    const bool rgb10Ok = saveRgb10Exports(basePath, &rawPath, &tiffPath, &statsPath);

    QString pngPath = basePath + ".png";
    bool pngOk = false;
    if (rgb10Ok)
    {
        pngOk = savePngFromRgba10Raw(rawPath, pngPath, lastFrameWidth_, lastFrameHeight_);
        if (!pngOk)
            pngOk = saveSnapshotImage(&pngPath, pngPath);
    }
    else
    {
        pngOk = saveSnapshotImage(&pngPath, pngPath);
    }

    if (!pngOk && !rgb10Ok)
    {
        QMessageBox::warning(this, "Snapshot",
                             QStringLiteral("Snapshot / scene RAW export failed.\nBase path: %1").arg(basePath));
        return;
    }

    QStringList saved;
    if (pngOk)
        saved << pngPath;
    if (rgb10Ok)
    {
        const QString fp16RawPath = basePath + "_fp16_rgba16f.raw";
        const QString rgb10RawPath = basePath + "_rgb10_u16.raw";
        const QString rgba16RawPath = basePath + "_rgba16_expanded.raw";
        saved << fp16RawPath << rgb10RawPath << rgba16RawPath << tiffPath << statsPath;
        MainWindow::postLog(QStringLiteral("[SceneRawExport] PNG=%1 FP16=%2 RGB10=%3 RGBA16=%4 TIFF=%5 STATS=%6")
                                .arg(pngPath, fp16RawPath, rgb10RawPath, rgba16RawPath, tiffPath, statsPath));
    }

    if (ui->statusbar)
    {
        ui->statusbar->showMessage(
            rgb10Ok
                ? QStringLiteral("Snapshot + FP16/RGB10/RGBA16 RAW saved: %1").arg(QFileInfo(rawPath).fileName())
                : QStringLiteral("Snapshot saved: %1").arg(QFileInfo(pngPath).fileName()),
            6000);
    }

    QMessageBox::information(this, "Snapshot",
                             QStringLiteral("Saved files:\n%1").arg(saved.join("\n")));
}
