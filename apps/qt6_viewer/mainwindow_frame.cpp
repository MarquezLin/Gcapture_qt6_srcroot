#include "mainwindow.h"

#include "./ui_mainwindow.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QImage>
#include <QMessageBox>
#include <QStringList>
#include <algorithm>
#include <cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#endif

namespace
{
#ifdef _WIN32
    static bool writeTiffWithWic(const QString &path, const QImage &source)
    {
        if (source.isNull())
            return false;

        const HRESULT initHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool uninitialize = SUCCEEDED(initHr);
        Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                      CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        Microsoft::WRL::ComPtr<IWICStream> stream;
        Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
        Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
        Microsoft::WRL::ComPtr<IPropertyBag2> properties;
        const bool highBitDepth = source.format() == QImage::Format_RGBA64 ||
                                  source.format() == QImage::Format_RGBX64 ||
                                  source.format() == QImage::Format_RGBA64_Premultiplied;
        const QImage image = highBitDepth
                                 ? source.convertToFormat(QImage::Format_RGBA64)
                                 : source.convertToFormat(QImage::Format_ARGB32);
        const std::wstring widePath = QDir::toNativeSeparators(path).toStdWString();

        if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
        if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(widePath.c_str(), GENERIC_WRITE);
        if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatTiff, nullptr, &encoder);
        if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, &properties);
        if (SUCCEEDED(hr)) hr = frame->Initialize(properties.Get());
        if (SUCCEEDED(hr)) hr = frame->SetSize(UINT(image.width()), UINT(image.height()));
        QByteArray rgb48;
        WICPixelFormatGUID pixelFormat = highBitDepth
                                             ? GUID_WICPixelFormat48bppRGB
                                             : GUID_WICPixelFormat32bppBGRA;
        if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&pixelFormat);
        const WICPixelFormatGUID expectedFormat = highBitDepth
                                                       ? GUID_WICPixelFormat48bppRGB
                                                       : GUID_WICPixelFormat32bppBGRA;
        if (SUCCEEDED(hr) && !IsEqualGUID(pixelFormat, expectedFormat))
            hr = E_FAIL;
        if (SUCCEEDED(hr) && highBitDepth)
        {
            const int stride = image.width() * 6;
            rgb48.resize(stride * image.height());
            for (int y = 0; y < image.height(); ++y)
            {
                const QRgba64 *src = reinterpret_cast<const QRgba64 *>(image.constScanLine(y));
                quint16 *dst = reinterpret_cast<quint16 *>(rgb48.data() + qsizetype(y) * stride);
                for (int x = 0; x < image.width(); ++x)
                {
                    dst[x * 3 + 0] = src[x].red();
                    dst[x * 3 + 1] = src[x].green();
                    dst[x * 3 + 2] = src[x].blue();
                }
            }
            hr = frame->WritePixels(UINT(image.height()), UINT(stride), UINT(rgb48.size()),
                                    reinterpret_cast<BYTE *>(rgb48.data()));
        }
        else if (SUCCEEDED(hr))
        {
            hr = frame->WritePixels(UINT(image.height()), UINT(image.bytesPerLine()),
                                    UINT(image.sizeInBytes()), const_cast<BYTE *>(image.constBits()));
        }
        if (SUCCEEDED(hr)) hr = frame->Commit();
        if (SUCCEEDED(hr)) hr = encoder->Commit();
        if (uninitialize)
            CoUninitialize();
        return SUCCEEDED(hr);
    }
#endif

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

#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
    static bool writeRawAndMetadata(const QString &rawPath,
                                    const QByteArray &data,
                                    int width,
                                    int height,
                                    int strideBytes,
                                    const QString &pixelFormat,
                                    uint64_t frameId)
    {
        QFile raw(rawPath);
        if (!raw.open(QIODevice::WriteOnly) || raw.write(data) != data.size())
            return false;
        raw.close();

        QJsonObject metadata;
        metadata.insert(QStringLiteral("width"), width);
        metadata.insert(QStringLiteral("height"), height);
        metadata.insert(QStringLiteral("strideBytes"), strideBytes);
        metadata.insert(QStringLiteral("pixelFormat"), pixelFormat);
        metadata.insert(QStringLiteral("endianness"), QStringLiteral("little"));
        metadata.insert(QStringLiteral("dataOffset"), 0);
        metadata.insert(QStringLiteral("frameCount"), 1);
        metadata.insert(QStringLiteral("frameId"), QString::number(frameId));
        QFile json(rawPath + QStringLiteral(".json"));
        return json.open(QIODevice::WriteOnly) &&
               json.write(QJsonDocument(metadata).toJson(QJsonDocument::Indented)) > 0;
    }

    static QByteArray normalizeGvfgY210(const QByteArray &nativeData,
                                        int width,
                                        int height,
                                        int strideBytes)
    {
        QByteArray standard = nativeData;
        const int rowBytes = width * 4;
        for (int y = 0; y < height; ++y)
        {
            char *row = standard.data() + qsizetype(y) * strideBytes;
            for (int offset = 0; offset + 7 < rowBytes; offset += 8)
            {
                std::swap(row[offset + 2], row[offset + 6]);
                std::swap(row[offset + 3], row[offset + 7]);
            }
        }
        return standard;
    }
#endif
} // namespace

void MainWindow::applyInitialPreviewSizeFromSource(int width, int height)
{
    if (initialPreviewSizeApplied_ || width <= 0 || height <= 0 || !previewWindow_)
        return;

    initialPreviewSizeApplied_ = true;

    MainWindow::postLog(QStringLiteral("[Preview] embedded source=%1x%2 host=%3x%4")
                            .arg(width)
                            .arg(height)
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

    const QByteArray baseUtf8 = QDir::toNativeSeparators(basePath).toUtf8();
    gcap_snapshot_export_desc_t desc{};
    desc.base_path_utf8 = baseUtf8.constData();
    desc.flags = GCAP_EXPORT_RAW_NATIVE | GCAP_EXPORT_RAW_RGB10_U16 | GCAP_EXPORT_RAW_RGBA8 |
                 GCAP_EXPORT_TIFF | GCAP_EXPORT_STATS | GCAP_EXPORT_PNG;

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

QString MainWindow::saveLatestSourceRaw(const QString &basePath)
{
    QByteArray data;
    int width = 0, height = 0, stride = 0, format = -1;
    {
        QMutexLocker lock(&rawSnapshotMutex_);
        data = latestRawFrame_;
        width = latestRawWidth_;
        height = latestRawHeight_;
        stride = latestRawStride_;
        format = latestRawFormat_;
    }
    if (data.isEmpty() || width <= 0 || height <= 0 || stride <= 0)
        return {};

    QString formatName;
    if (format == GCAP_FMT_YUY2)
        formatName = QStringLiteral("yuy2");
    else if (format == GCAP_FMT_Y210)
        formatName = QStringLiteral("y210");
    else if (format == GCAP_FMT_ARGB)
        formatName = QStringLiteral("bgra8");
    else
        return {};

    const QString rawPath = basePath + QStringLiteral("_source_%1.raw").arg(formatName);
    QFile raw(rawPath);
    if (!raw.open(QIODevice::WriteOnly) || raw.write(data) != data.size())
        return {};
    raw.close();

    QJsonObject metadata;
    metadata.insert(QStringLiteral("width"), width);
    metadata.insert(QStringLiteral("height"), height);
    metadata.insert(QStringLiteral("strideBytes"), stride);
    metadata.insert(QStringLiteral("pixelFormat"), formatName.toUpper());
    metadata.insert(QStringLiteral("endianness"), QStringLiteral("little"));
    metadata.insert(QStringLiteral("dataOffset"), 0);
    metadata.insert(QStringLiteral("frameCount"), 1);
    QFile json(rawPath + QStringLiteral(".json"));
    if (json.open(QIODevice::WriteOnly))
        json.write(QJsonDocument(metadata).toJson(QJsonDocument::Indented));
    return rawPath;
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
        const GvfgSource::Snapshot snapshot = gvfg_->captureSnapshotData(2000, &error);
        if (snapshot.image.isNull() || snapshot.rawData.isEmpty())
        {
            QMessageBox::warning(this, "Snapshot",
                                 QStringLiteral("GVFG snapshot failed.\n%1").arg(error));
            return;
        }

        const QString basePath = buildSnapshotBasePath();
        const QString pngPath = basePath + QStringLiteral(".png");
        if (!snapshot.image.save(pngPath, "PNG"))
        {
            QMessageBox::warning(this, "Snapshot",
                                 QStringLiteral("Snapshot save failed.\nPath: %1").arg(pngPath));
            return;
        }

        QStringList saved{pngPath};
        const QString tiffPath = basePath + QStringLiteral(".tiff");
#ifdef _WIN32
        if (writeTiffWithWic(tiffPath, snapshot.image))
            saved << tiffPath;
        else
            MainWindow::postLog(QStringLiteral("[GVFG Snapshot] TIFF export failed: %1").arg(tiffPath), true);
#endif
        if (snapshot.pixelFormat == GVFG_PIXFMT_Y210)
        {
            const QByteArray standardY210 = normalizeGvfgY210(
                snapshot.rawData, snapshot.width, snapshot.height, snapshot.strideBytes);
            const QString standardPath = basePath + QStringLiteral("_source_y210.raw");
            if (writeRawAndMetadata(standardPath, standardY210, snapshot.width,
                                    snapshot.height, snapshot.strideBytes,
                                    QStringLiteral("Y210"), snapshot.frameId))
                saved << standardPath << standardPath + QStringLiteral(".json");
        }
        else if (snapshot.pixelFormat == GVFG_PIXFMT_YVYU)
        {
            const QString yvyuPath = basePath + QStringLiteral("_source_yvyu.raw");
            if (writeRawAndMetadata(yvyuPath, snapshot.rawData, snapshot.width,
                                    snapshot.height, snapshot.strideBytes,
                                    QStringLiteral("YVYU"), snapshot.frameId))
                saved << yvyuPath << yvyuPath + QStringLiteral(".json");
        }

        lastFrameImage_ = snapshot.image;
        if (ui->statusbar)
            ui->statusbar->showMessage(QStringLiteral("GVFG RAW snapshot saved: %1")
                                           .arg(QFileInfo(pngPath).fileName()), 6000);
        QMessageBox::information(this, "Snapshot",
                                 QStringLiteral("Saved files:\n%1").arg(saved.join("\n")));
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
    const QString sourceRawPath = saveLatestSourceRaw(basePath);
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
        appendPath(result.rgb10_u16_path);
        appendPath(result.rgba8_path);
        appendPath(result.tiff_path);
        appendPath(result.stats_path);

        const QString source = result.source_bit_depth >= 10 ? QStringLiteral("10-bit") : QStringLiteral("8-bit");
        QStringList logParts;
        logParts << QStringLiteral("source=%1").arg(source)
                 << QStringLiteral("PNG=%1").arg(pngPath)
                 << QStringLiteral("nativeRAW=%1").arg(QString::fromUtf8(result.native_raw_path))
                 << QStringLiteral("RGB10=%1").arg(QString::fromUtf8(result.rgb10_u16_path))
                 << QStringLiteral("RGBA8=%1").arg(QString::fromUtf8(result.rgba8_path))
                 << QStringLiteral("TIFF=%1").arg(result.tiff_path[0] ? QString::fromUtf8(result.tiff_path) : QStringLiteral("disabled"))
                 << QStringLiteral("STATS=%1").arg(QString::fromUtf8(result.stats_path))
                 << QStringLiteral("flags=0x%1").arg(QString::number(result.generated_flags, 16));
        MainWindow::postLog(QStringLiteral("[SceneExport] %1").arg(logParts.join(QStringLiteral(" "))));
    }
    if (!sourceRawPath.isEmpty())
    {
        saved << sourceRawPath << sourceRawPath + QStringLiteral(".json");
        MainWindow::postLog(QStringLiteral("[SourceRAW] %1").arg(sourceRawPath));
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
