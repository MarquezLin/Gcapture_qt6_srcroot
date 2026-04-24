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

struct RawRgb10HeaderView
{
    quint32 magic = 0;
    quint32 width = 0;
    quint32 height = 0;
    quint32 channels = 0;
    quint32 bitDepth = 0;
};

static bool savePngFromRg10Raw(const QString &rawPath, const QString &pngPath)
{
    QFile f(rawPath);
    if (!f.open(QIODevice::ReadOnly))
        return false;

    if (f.size() < static_cast<qint64>(sizeof(RawRgb10HeaderView)))
        return false;

    RawRgb10HeaderView hdr{};
    if (f.read(reinterpret_cast<char *>(&hdr), sizeof(hdr)) != static_cast<qint64>(sizeof(hdr)))
        return false;

    if (qFromLittleEndian(hdr.magic) != 0x30314752u) // 'RG10'
        return false;

    const quint32 width = qFromLittleEndian(hdr.width);
    const quint32 height = qFromLittleEndian(hdr.height);
    const quint32 channels = qFromLittleEndian(hdr.channels);
    const quint32 bitDepth = qFromLittleEndian(hdr.bitDepth);
    if (width == 0 || height == 0 || channels != 3u || bitDepth != 10u)
        return false;

    const qsizetype pixelCount = static_cast<qsizetype>(width) * static_cast<qsizetype>(height);
    const qsizetype wordCount = pixelCount * 3;
    const QByteArray payload = f.readAll();
    if (payload.size() < wordCount * static_cast<qsizetype>(sizeof(quint16)))
        return false;

    QImage img(static_cast<int>(width), static_cast<int>(height), QImage::Format_RGB888);
    if (img.isNull())
        return false;

    const auto *src = reinterpret_cast<const quint16 *>(payload.constData());
    for (quint32 y = 0; y < height; ++y)
    {
        uchar *dst = img.scanLine(static_cast<int>(y));
        const qsizetype rowBase = static_cast<qsizetype>(y) * static_cast<qsizetype>(width) * 3;
        for (quint32 x = 0; x < width; ++x)
        {
            const qsizetype si = rowBase + static_cast<qsizetype>(x) * 3;
            const quint16 r10 = qFromLittleEndian(src[si + 0]);
            const quint16 g10 = qFromLittleEndian(src[si + 1]);
            const quint16 b10 = qFromLittleEndian(src[si + 2]);
            dst[x * 3 + 0] = static_cast<uchar>((static_cast<unsigned>(r10) * 255u + 511u) / 1023u);
            dst[x * 3 + 1] = static_cast<uchar>((static_cast<unsigned>(g10) * 255u + 511u) / 1023u);
            dst[x * 3 + 2] = static_cast<uchar>((static_cast<unsigned>(b10) * 255u + 511u) / 1023u);
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
        *rawPath = basePath + ".raw";
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
        pngOk = savePngFromRg10Raw(rawPath, pngPath);
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
                             QStringLiteral("Snapshot / RGB10 export failed.\nBase path: %1").arg(basePath));
        return;
    }

    QStringList saved;
    if (pngOk)
        saved << pngPath;
    if (rgb10Ok)
    {
        saved << rawPath << tiffPath << statsPath;
        MainWindow::postLog(QStringLiteral("[RGB10Export] PNG=%1 RAW=%2 TIFF=%3 STATS=%4")
                                .arg(pngPath, rawPath, tiffPath, statsPath));
    }

    if (ui->statusbar)
    {
        ui->statusbar->showMessage(
            rgb10Ok
                ? QStringLiteral("Snapshot + RGB10 RAW/TIFF saved: %1").arg(QFileInfo(tiffPath).fileName())
                : QStringLiteral("Snapshot saved: %1").arg(QFileInfo(pngPath).fileName()),
            6000);
    }

    QMessageBox::information(this, "Snapshot",
                             QStringLiteral("Saved files:\n%1").arg(saved.join("\n")));
}
