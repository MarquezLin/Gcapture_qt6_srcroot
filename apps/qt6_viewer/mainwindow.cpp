#include "mainwindow.h"
#include "gcapture.h"

#include "./ui_mainwindow.h"
#include <QVBoxLayout>
#include <QPushButton>
#include <QComboBox>
#include <QMessageBox>
#include <QPixmap>
#include <QWindow>
#include <QScreen>
#include <QGuiApplication>
#include <QCoreApplication>
#include <QDir>
#include "display_info.h"
#include <QDialog>
#include <QPlainTextEdit>
#include <QLabel>
#include <QDesktopServices>
#include <QUrl>
#include <vector>
#include <set>
#include <QSignalBlocker>
#include "edid_reader.h"
#include "tiffanalysisdialog.h"
#include "tiff_analyzer.h"
#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QTimer>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_2.h>
#include <wrl.h>
using Microsoft::WRL::ComPtr;
#endif

static MainWindow *g_mainWindow = nullptr;

#ifndef QT6_VIEWER_VERSION
#define QT6_VIEWER_VERSION "1.0.0"
#endif

static void sdkLogCallback(gcap_log_level_t level, const char *message_utf8, void *user)
{
    Q_UNUSED(user);
    const QString prefix = [level]() -> QString {
        switch (level)
        {
        case GCAP_LOG_TRACE: return QStringLiteral("[SDK][TRACE]");
        case GCAP_LOG_DEBUG: return QStringLiteral("[SDK][DEBUG]");
        case GCAP_LOG_INFO:  return QStringLiteral("[SDK][INFO]");
        case GCAP_LOG_WARN:  return QStringLiteral("[SDK][WARN]");
        case GCAP_LOG_ERROR: return QStringLiteral("[SDK][ERROR]");
        default:             return QStringLiteral("[SDK]");
        }
    }();
    QString msg = QString::fromUtf8(message_utf8 ? message_utf8 : "").trimmed();
    if (msg.isEmpty())
        return;
    const QString line = prefix + QLatin1Char(' ') + msg;
    if (level >= GCAP_LOG_WARN)
        qWarning().noquote() << line;
    else
        qInfo().noquote() << line;
    if (g_mainWindow)
    {
        QMetaObject::invokeMethod(
            g_mainWindow,
            "appendDebugLog",
            Qt::QueuedConnection,
            Q_ARG(QString, line));
    }
}

namespace
{
    static const char *packetFmtName(int fmt)
    {
        return gcap_pixfmt_name(static_cast<gcap_pixfmt_t>(fmt));
    }

    static QString formatVideoCapDisplay(const gcap_video_cap_t &cap)
    {
        const double fps = (cap.fps_den > 0) ? (double(cap.fps_num) / double(cap.fps_den)) : 0.0;
        const QString nativeName = cap.subtype_name[0]
                                       ? QString::fromUtf8(cap.subtype_name)
                                       : QString::fromLatin1(packetFmtName(cap.pixfmt));
        QString text = QStringLiteral("%1 %2x%3")
                           .arg(nativeName)
                           .arg(cap.width)
                           .arg(cap.height);
        if (fps > 0.0)
            text += QStringLiteral(" %1 fps").arg(fps, 0, 'f', 2);
        if (cap.bit_depth > 0)
            text += QStringLiteral(" (%1-bit)").arg(cap.bit_depth);
        return text;
    }

    static QString pixelFormatComboLabel(gcap_pixfmt_t fmt)
    {
        if (gcap_pixfmt_bit_depth(fmt) <= 0)
            return QString();
        if (fmt == GCAP_FMT_YUY2)
            return QStringLiteral("Format: YUY2/HDYC/UYVY");
        return QStringLiteral("Format: %1").arg(QString::fromUtf8(gcap_pixfmt_name(fmt)));
    }

#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
    static QString hex32(uint32_t value)
    {
        return QStringLiteral("0x") + QString::number(value, 16).rightJustified(8, QLatin1Char('0')).toUpper();
    }

    static QString formatGvfgFpgaRawLogLine(const gvfg_runtime_info_t &rt)
    {
        const auto &fpga = rt.input_signal.fpga;
        return QStringLiteral("[GVFG][fpga_raw]\n"
                              "  valid_mask=%1\n"
                              "  resolution:   width_valid=%2 width_raw=%3 height_valid=%4 height_raw=%5\n"
                              "  video_format: valid=%6 raw=%7 code=%8 name=%9\n"
                              "  frame_rate:   valid=%10 raw=%11 code=%12 bits=%13 name=%14\n"
                              "  bit_depth:    valid=%15 raw=%16 value=%17\n"
                              "  status:       valid=%18 raw=%19 sdi_lock=%20 sdi_ddr=%21 hdmi_lock=%22 hdmi_ddr=%23")
            .arg(hex32(fpga.valid_mask))
            .arg(fpga.width_valid)
            .arg(fpga.width_raw)
            .arg(fpga.height_valid)
            .arg(fpga.height_raw)
            .arg(fpga.video_format_valid)
            .arg(hex32(fpga.video_format_raw))
            .arg(fpga.video_format_code)
            .arg(QString::fromLatin1(fpga.video_format))
            .arg(fpga.frame_rate_valid)
            .arg(hex32(fpga.frame_rate_raw))
            .arg(fpga.frame_rate_code)
            .arg(QString::fromLatin1(fpga.frame_rate_bits))
            .arg(QString::fromLatin1(fpga.frame_rate_name))
            .arg(fpga.bit_depth_valid)
            .arg(hex32(fpga.bit_depth_raw))
            .arg(fpga.bit_depth)
            .arg(fpga.status_valid)
            .arg(hex32(fpga.status_raw))
            .arg(fpga.sdi_locked)
            .arg(fpga.sdi_ddr_ok)
            .arg(fpga.hdmi_locked)
            .arg(fpga.hdmi_ddr_ok);
    }

    static QString formatGvfgFpgaRawStateKey(const gvfg_runtime_info_t &rt)
    {
        const auto &fpga = rt.input_signal.fpga;
        return QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8|%9|%10|%11|%12|%13|%14|%15|%16|%17|%18|%19|%20|%21|%22|%23")
            .arg(fpga.valid_mask)
            .arg(fpga.width_valid)
            .arg(fpga.width_raw)
            .arg(fpga.height_valid)
            .arg(fpga.height_raw)
            .arg(fpga.video_format_valid)
            .arg(fpga.video_format_raw)
            .arg(fpga.video_format_code)
            .arg(QString::fromLatin1(fpga.video_format))
            .arg(fpga.frame_rate_valid)
            .arg(fpga.frame_rate_raw)
            .arg(fpga.frame_rate_code)
            .arg(QString::fromLatin1(fpga.frame_rate_bits))
            .arg(QString::fromLatin1(fpga.frame_rate_name))
            .arg(fpga.bit_depth_valid)
            .arg(fpga.bit_depth_raw)
            .arg(fpga.bit_depth)
            .arg(fpga.status_valid)
            .arg(fpga.status_raw)
            .arg(fpga.sdi_locked)
            .arg(fpga.sdi_ddr_ok)
            .arg(fpga.hdmi_locked)
            .arg(fpga.hdmi_ddr_ok);
    }
#endif

    static std::vector<gcap_pixfmt_t> enumerateSupportedPixelFormats(int backend, int deviceIndex)
    {
        std::vector<gcap_pixfmt_t> result;
#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
        if (backend == kQtViewerGvfgBackend)
        {
            Q_UNUSED(deviceIndex);
            result.push_back(GCAP_FMT_YUY2);
            return result;
        }
#endif
        const int fmtCount = gcap_enum_supported_pixel_formats(backend, deviceIndex, nullptr, 0);
        if (fmtCount <= 0)
            return result;

        std::vector<gcap_pixfmt_t> formats(static_cast<size_t>(fmtCount));
        const int written = gcap_enum_supported_pixel_formats(backend, deviceIndex, formats.data(), static_cast<int>(formats.size()));
        std::set<int> seen;
        for (int i = 0; i < written; ++i)
        {
            const auto fmt = formats[static_cast<size_t>(i)];
            switch (fmt)
            {
            case GCAP_FMT_NV12:
            case GCAP_FMT_YUY2:
            case GCAP_FMT_Y210:
            case GCAP_FMT_V210:
            case GCAP_FMT_P010:
            case GCAP_FMT_ARGB:
                if (seen.insert(static_cast<int>(fmt)).second)
                    result.push_back(fmt);
                break;
            default:
                break;
            }
        }
        return result;
    }

    static QString formatPropertyPageDisplay(const gcap_property_page_t &page)
    {
        return QStringLiteral("%1 - %2")
            .arg(QString::fromUtf8(page.page_name))
            .arg(page.capture_pin ? QStringLiteral("Capture Pin") : QStringLiteral("Filter"));
    }

    static void fillDeviceCapabilitiesFromSdk(int deviceIndex, CaptureDeviceInfo &info)
    {
        info.supportedFormats.clear();
        info.propertyPages.clear();

        const int capCount = gcap_enum_video_caps(deviceIndex, nullptr, 0);
        if (capCount > 0)
        {
            std::vector<gcap_video_cap_t> caps(static_cast<size_t>(capCount));
            const int written = gcap_enum_video_caps(deviceIndex, caps.data(), static_cast<int>(caps.size()));
            for (int i = 0; i < written; ++i)
                info.supportedFormats << formatVideoCapDisplay(caps[static_cast<size_t>(i)]);
        }

        const int pageCount = gcap_enum_property_pages(deviceIndex, nullptr, 0);
        if (pageCount > 0)
        {
            std::vector<gcap_property_page_t> pages(static_cast<size_t>(pageCount));
            const int written = gcap_enum_property_pages(deviceIndex, pages.data(), static_cast<int>(pages.size()));
            for (int i = 0; i < written; ++i)
                info.propertyPages << formatPropertyPageDisplay(pages[static_cast<size_t>(i)]);
        }
    }

    static QImage framePacketToQImage(const gcap_frame_packet_t &pkt)
    {
        if (pkt.width <= 0 || pkt.height <= 0 || pkt.plane_count <= 0 || !pkt.data[0])
            return {};

        QImage img(pkt.width, pkt.height, QImage::Format_ARGB32);
        if (img.isNull())
            return {};

        const gcap_status_t st = gcap_frame_to_bgra8(&pkt, img.bits(), img.bytesPerLine());
        if (st != GCAP_OK)
            return {};
        return img;
    }

    struct GigabyteRawLoadResult
    {
        QImage image;
        QString format;
        int sourceBitDepth = 0;
        QString error;
    };

    static bool parseGigabyteRawHeader(const QByteArray &header,
                                       int &width,
                                       int &height,
                                       QString &format,
                                       int &sourceBitDepth,
                                       QString &error)
    {
        const int nul = header.indexOf('\0');
        const QByteArray textBytes = nul >= 0 ? header.left(nul) : header;
        const QStringList lines = QString::fromLatin1(textBytes)
                                      .replace(QLatin1Char('\r'), QLatin1Char('\n'))
                                      .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        if (lines.isEmpty() || lines.first().trimmed() != QStringLiteral("GIGABYTE_RAW"))
        {
            error = QStringLiteral("Not a GIGABYTE_RAW file.");
            return false;
        }

        int headerSize = 0;
        for (int i = 1; i < lines.size(); ++i)
        {
            const QString line = lines.at(i).trimmed();
            const int eq = line.indexOf(QLatin1Char('='));
            if (eq <= 0)
                continue;
            const QString key = line.left(eq).trimmed();
            const QString value = line.mid(eq + 1).trimmed();
            if (key == QStringLiteral("header_size"))
                headerSize = value.toInt();
            else if (key == QStringLiteral("Width"))
                width = value.toInt();
            else if (key == QStringLiteral("Height"))
                height = value.toInt();
            else if (key == QStringLiteral("Format"))
                format = value;
            else if (key == QStringLiteral("SourceBitDepth"))
                sourceBitDepth = value.toInt();
        }

        if (headerSize != GCAP_GIGABYTE_RAW_HEADER_SIZE)
        {
            error = QStringLiteral("Unsupported GIGABYTE RAW header size: %1").arg(headerSize);
            return false;
        }
        if (width <= 0 || height <= 0 || format.isEmpty())
        {
            error = QStringLiteral("Invalid GIGABYTE RAW metadata.");
            return false;
        }
        return true;
    }

    static GigabyteRawLoadResult loadGigabyteRawImage(const QString &path)
    {
        GigabyteRawLoadResult result;
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
        {
            result.error = QStringLiteral("Failed to open file.");
            return result;
        }

        const QByteArray header = f.read(GCAP_GIGABYTE_RAW_HEADER_SIZE);
        if (header.size() != GCAP_GIGABYTE_RAW_HEADER_SIZE)
        {
            result.error = QStringLiteral("File is too small for a GIGABYTE RAW header.");
            return result;
        }

        int width = 0;
        int height = 0;
        QString format;
        int sourceBitDepth = 0;
        QString error;
        if (!parseGigabyteRawHeader(header, width, height, format, sourceBitDepth, error))
        {
            result.error = error;
            return result;
        }

        const qint64 pixelCount = qint64(width) * qint64(height);
        const qint64 expected = pixelCount * 4;
        if (pixelCount <= 0 || expected <= 0)
        {
            result.error = QStringLiteral("Invalid image dimensions.");
            return result;
        }

        const QByteArray payload = f.read(expected);
        if (payload.size() != expected)
        {
            result.error = QStringLiteral("%1 payload is incomplete.").arg(format);
            return result;
        }

        if (format == QStringLiteral("BGRA8"))
        {
            const QImage wrapped(reinterpret_cast<const uchar *>(payload.constData()),
                                 width,
                                 height,
                                 width * 4,
                                 QImage::Format_ARGB32);
            result.image = wrapped.copy();
        }
        else if (format == QStringLiteral("ABGR2101010"))
        {
            QImage img(width, height, QImage::Format_ARGB32);
            if (img.isNull())
            {
                result.error = QStringLiteral("Failed to allocate image.");
                return result;
            }

            const uchar *src = reinterpret_cast<const uchar *>(payload.constData());
            for (int y = 0; y < height; ++y)
            {
                QRgb *dst = reinterpret_cast<QRgb *>(img.scanLine(y));
                for (int x = 0; x < width; ++x)
                {
                    const qint64 i = (qint64(y) * qint64(width) + qint64(x)) * 4;
                    const quint32 p = quint32(src[i + 0]) |
                                      (quint32(src[i + 1]) << 8) |
                                      (quint32(src[i + 2]) << 16) |
                                      (quint32(src[i + 3]) << 24);
                    const quint32 r10 = (p >> 0) & 0x3FFu;
                    const quint32 g10 = (p >> 10) & 0x3FFu;
                    const quint32 b10 = (p >> 20) & 0x3FFu;
                    dst[x] = qRgba(int((r10 * 255u + 511u) / 1023u),
                                   int((g10 * 255u + 511u) / 1023u),
                                   int((b10 * 255u + 511u) / 1023u),
                                   255);
                }
            }
            result.image = img;
        }
        else
        {
            result.error = QStringLiteral("Unsupported GIGABYTE RAW format: %1").arg(format);
            return result;
        }

        result.format = format;
        result.sourceBitDepth = sourceBitDepth;
        return result;
    }
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    setWindowTitle(QStringLiteral("Gigabyte v%1").arg(QString::fromLatin1(QT6_VIEWER_VERSION)));

#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
    gvfg_ = new GvfgSource(this);
    connect(gvfg_, &GvfgSource::frameReady, this, &MainWindow::sigFrame, Qt::QueuedConnection);
    connect(gvfg_, &GvfgSource::errorOccurred, this, [this](const QString &message)
            { MainWindow::postLog(QStringLiteral("[GVFG] %1").arg(message), true); },
            Qt::QueuedConnection);
    connect(gvfg_, &GvfgSource::preStartRuntimeInfoReady, this, [this](const gvfg_runtime_info_t &info)
            {
                const QString rawStateKey = formatGvfgFpgaRawStateKey(info);
                MainWindow::postLog(QStringLiteral("[GVFG] FPGA signal before stream start"));
                MainWindow::postLog(formatGvfgFpgaRawLogLine(info));
                lastGvfgRawStateKey_ = rawStateKey;
            });
#endif

    setupRuntimeStatusTimer();
    setupDebugDock();
    setupProcAmpAction();
    setupBackendControls();
    setupPreviewBitDepthControls();
    initializeDeviceList();
    initializeGpuList();
    setupConnections();

    g_mainWindow = this;
    gcap_set_log_callback(sdkLogCallback, this);
    logStartupInfo();
}

MainWindow::~MainWindow()
{
#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
    if (gvfg_)
        gvfg_->stop();
#endif
    if (g_mainWindow == this)
    {
        gcap_set_log_callback(nullptr, nullptr);
        g_mainWindow = nullptr;
    }
    delete tiffAnalysisDlg_;
    delete rawPreviewWindow_;
    delete previewWindow_;
    delete ui;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (rawPreviewWindow_)
    {
        rawPreviewWindow_->close();
    }

    if (previewWindow_)
    {
        previewWindow_->close();
    }

    QMainWindow::closeEvent(event);
}

void MainWindow::s_vcb(const gcap_frame_t *f, void *u)
{
    auto *self = static_cast<MainWindow *>(u);
    if (!self || !f || !f->data[0] || f->width <= 0 || f->height <= 0)
        return;
    if (self->usePacketCallback_)
        return;

    // Copy the frame before crossing from the callback thread to the UI thread.
    QImage img((const uchar *)f->data[0], f->width, f->height, f->stride[0], QImage::Format_ARGB32);
    const QImage safeImg = img.copy();
    const uint64_t ptsNs = f->pts_ns;
    const int width = f->width;
    const int height = f->height;

    QMetaObject::invokeMethod(
        self,
        [self, ptsNs, width, height, safeImg]()
        {
            self->updateFrameSourceState(ptsNs, width, height, self->lastVideoCallbackPtsNs_);
            self->dispatchFrameImage(safeImg);
        },
        Qt::QueuedConnection);
}

void MainWindow::s_pcb(const gcap_frame_packet_t *pkt, void *u)
{
    auto *self = static_cast<MainWindow *>(u);
    if (!self || !pkt)
        return;

    const gcap_frame_packet_t pktCopy = *pkt;
    QImage img;
    if (self->usePacketCallback_)
        img = framePacketToQImage(pktCopy);

    QMetaObject::invokeMethod(
        self,
        [self, pktCopy, img]()
        {
            self->updateFrameSourceState(pktCopy.pts_ns, pktCopy.width, pktCopy.height, self->lastPacketCallbackPtsNs_);
            self->logFramePacketIfNeeded(pktCopy);
            if (self->usePacketCallback_ && !img.isNull())
                self->dispatchFrameImage(img);
        },
        Qt::QueuedConnection);
}

void MainWindow::s_ecb(gcap_status_t c, const char *m, void *u)
{
    Q_UNUSED(u);

    const QByteArray ba = m ? QByteArray(m) : QByteArray();

    // SDK error text can be UTF-8 or Windows ACP depending on the source.
    QString msg = QString::fromUtf8(ba);
    if (msg.contains(QChar(0xFFFD)))
        msg = QString::fromLocal8Bit(ba);

    // Centralized log sink for UI and file logging.
    MainWindow::postLog(
        QStringLiteral("[gcapture][%1] %2").arg(int(c)).arg(msg),
        c != GCAP_OK);

    // Show a dialog only for errors.
    if (c != GCAP_OK && g_mainWindow)
    {
        QMetaObject::invokeMethod(
            g_mainWindow,
            [msg]
            {
                QMessageBox::warning(nullptr, "gcapture", msg);
            },
            Qt::QueuedConnection);
    }
}

void MainWindow::updateRuntimeStatusUi()
{
    if (!ui->statusbar)
        return;

    if (!h_)
    {
#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
        if (usingGvfg_ && gvfg_)
        {
            const gvfg_runtime_info_t rt = gvfg_->runtimeInfo();
            const gvfg_preview_info_t pv = gvfg_->previewInfo();
            const QString rawStateKey = formatGvfgFpgaRawStateKey(rt);
            if (lastGvfgRawStateKey_ != rawStateKey)
            {
                MainWindow::postLog(formatGvfgFpgaRawLogLine(rt));
                lastGvfgRawStateKey_ = rawStateKey;
            }
            const auto &fpga = rt.input_signal.fpga;
            const auto &delivered = rt.delivered_frame;
            const double runtimeFps = (rt.capture_fps > 0.0) ? rt.capture_fps : avgFps_;
            const QString renderPath = pv.active ? QString::fromUtf8(pv.render_path) : QStringLiteral("App-owned render");
            const QString fpgaResolution = (fpga.width_valid && fpga.height_valid)
                                               ? QStringLiteral("%1x%2").arg(fpga.width_raw).arg(fpga.height_raw)
                                               : QStringLiteral("--");
            const QString fpgaFps = fpga.frame_rate_valid
                                        ? QStringLiteral("%1fps").arg(QString::fromLatin1(fpga.frame_rate_name))
                                        : QStringLiteral("--fps");
            const QString deliveredText = delivered.valid
                                              ? QStringLiteral("%1x%2 %3 %4bit")
                                                    .arg(delivered.width)
                                                    .arg(delivered.height)
                                                    .arg(QString::fromUtf8(delivered.pixel_format))
                                                    .arg(delivered.bit_depth)
                                              : QStringLiteral("--");
            const QString sb = QStringLiteral("Backend: %1 | FPGA reported %2 %3 | Delivered frame %4 | %5 | App runtime %6fps frames=%7")
                                   .arg(QStringLiteral("GVFG"))
                                   .arg(fpgaResolution)
                                   .arg(fpgaFps)
                                   .arg(deliveredText)
                                   .arg(renderPath)
                                   .arg(runtimeFps > 0.0 ? QString::number(runtimeFps, 'f', 2) : QStringLiteral("--"))
                                   .arg(QString::number(static_cast<qulonglong>(rt.delivered_frames)));
            if (lastRuntimeStatusText_ != sb)
            {
                ui->statusbar->showMessage(sb);
                lastRuntimeStatusText_ = sb;
            }
            return;
        }
#endif
        const QString sb = QStringLiteral("Idle");
        if (lastRuntimeStatusText_ != sb)
        {
            ui->statusbar->showMessage(sb);
            lastRuntimeStatusText_ = sb;
        }
        return;
    }

    gcap_runtime_info_t rt{};
    if (gcap_get_runtime_info(h_, &rt) != GCAP_OK)
        return;

    const double negotiatedFps = (rt.negotiated.fps_den > 0) ? (double(rt.negotiated.fps_num) / double(rt.negotiated.fps_den)) : 0.0;
    const double runtimeFps = (rt.runtime_fps > 0.0) ? rt.runtime_fps : avgFps_;
    const QString backend = QString::fromUtf8(rt.backend_name);
    const QString source = QString::fromUtf8(rt.frame_source);

    QString negotiatedFmt;
    const QString backendLower = QString::fromUtf8(rt.backend_name).toLower();
    if (backendLower.contains(QStringLiteral("dshow")) && rt.source_format[0])
        negotiatedFmt = QString::fromUtf8(rt.source_format);
    else if (rt.negotiated_desc[0])
        negotiatedFmt = QString::fromUtf8(rt.negotiated_desc);
    else if (rt.source_format[0])
        negotiatedFmt = QString::fromUtf8(rt.source_format);
    else
        negotiatedFmt = QString::fromUtf8(packetFmtName(rt.negotiated.pixfmt));
    const QString renderFmt = QString::fromUtf8(rt.render_format);
    const auto statusBlock = [](const char *label, const gcap_signal_status_t &s, double fps, const QString &fmt)
    {
        if (s.width <= 0 || s.height <= 0)
            return QStringLiteral("%1 --").arg(QString::fromUtf8(label));
        return QStringLiteral("%1 %2x%3 %4fps %5")
            .arg(QString::fromUtf8(label))
            .arg(s.width)
            .arg(s.height)
            .arg(QString::number(fps, 'f', 2))
            .arg(fmt);
    };
    QString probeFmt = QString::fromUtf8(packetFmtName(rt.signal_probe.pixfmt));
    const double probeFps = (rt.signal_probe.fps_den > 0) ? (double(rt.signal_probe.fps_num) / double(rt.signal_probe.fps_den)) : 0.0;

    const QString sb = QStringLiteral("Backend: %1 | Source: %2 | %3 | %4 | AppInternal %5 | Runtime %6fps")
                           .arg(backend)
                           .arg(source)
                           .arg(statusBlock("InputProbe", rt.signal_probe, probeFps, probeFmt))
                           .arg(statusBlock("BackendFmt", rt.negotiated, negotiatedFps, negotiatedFmt))
                           .arg(renderFmt.isEmpty() ? QStringLiteral("--") : renderFmt)
                           .arg(runtimeFps > 0.0 ? QString::number(runtimeFps, 'f', 2) : QStringLiteral("--"));
    if (lastRuntimeStatusText_ != sb)
    {
        ui->statusbar->showMessage(sb);
        lastRuntimeStatusText_ = sb;
    }
}

void MainWindow::onToggleDebugLog(bool checked)
{
    if (!debugDock_)
        return;

    debugDock_->setVisible(checked);
}

void MainWindow::onShowEdid()
{
#ifdef _WIN32
    if (!windowHandle())
    {
        QMessageBox::warning(this, tr("EDID"), tr("No window handle available"));
        return;
    }

    HWND hwnd = reinterpret_cast<HWND>(windowHandle()->winId());
    EdidResult res = readEdidForWindow(hwnd);

    if (!res.ok)
    {
        QMessageBox::warning(this, tr("EDID"),
                             tr("Failed to read/parse EDID:\n%1").arg(res.error));
        return;
    }

    // Convert raw EDID to a hex dump.
    QString hex;
    const QByteArray &raw = res.raw;
    for (int i = 0; i < raw.size(); ++i)
    {
        if (i % 16 == 0)
            hex += QString("\n%1: ").arg(i, 4, 16, QLatin1Char('0')).toUpper();

        hex += QString("%1 ").arg(static_cast<quint8>(raw[i]), 2, 16, QLatin1Char('0')).toUpper();
    }

    // Build the EDID summary.
    EdidSummary sum = summarizeEdid(raw, res.decoded);

    // Compose the dialog HTML.
    QString html;

    // 1) High-level summary, already formatted as HTML.
    if (!sum.highLevelText.isEmpty())
    {
        html += sum.highLevelText;
        html += "<br><br>";
    }

    // 2) Basic summary, already formatted as HTML.
    if (!sum.basicText.isEmpty())
    {
        html += sum.basicText;
        html += "<br><br>";
    }

    // 3) Source and size.
    html += tr("Source: %1<br>Size: %2 bytes<br>")
                .arg(res.sourceName.toHtmlEscaped())
                .arg(raw.size());
    html += "<br><br>";

    // 4) Raw EDID hex dump.
    html += "<b>Raw EDID</b>";
    html += hex.toHtmlEscaped().replace("\n", "<br>");
    html += "<br><br>";

    // 5) Raw edid-decode output.
    html += "<b>Decoded by edid-decode</b><br>";
    if (res.decoded.isEmpty())
    {
        html += tr("(No output from edid-decode)");
    }
    else
    {
        html += res.decoded.toHtmlEscaped().replace("\n", "<br>");
    }

    // Show the dialog with a QTextEdit using HTML content.
    QDialog dlg(this);
    dlg.setWindowTitle(tr("EDID Viewer"));
    QVBoxLayout *layout = new QVBoxLayout(&dlg);

    auto *edit = new QTextEdit(&dlg);
    edit->setReadOnly(true);
    edit->setHtml(html);

    layout->addWidget(edit);
    dlg.resize(800, 600);
    dlg.exec();
#else
    QMessageBox::information(this, tr("EDID"),
                             tr("EDID viewer is only implemented on Windows."));
#endif
}

void MainWindow::setupPreviewWindow()
{
    if (previewWindow_)
        return;

    previewWindow_ = new previewwindow();
    previewWindow_->setWindowTitle(QStringLiteral("Preview"));
    previewWindow_->resize(1280, 720);
}

void MainWindow::setupRuntimeStatusTimer()
{
    runtimeStatusTimer_ = new QTimer(this);
    runtimeStatusTimer_->setInterval(500);
    connect(runtimeStatusTimer_, &QTimer::timeout, this, [this]()
            {
                updateRuntimeStatusUi();
                refreshCaptureInfoFromSdkAndRuntime(true);
                refreshDisplayInfoFromCurrentState();

                if (!suppressAuxDialogRefresh_)
                {
                    if (infoDlg_ && infoDlg_->isVisible())
                    {
                        infoDlg_->setInfoText(lastInfoText_);
                        infoDlg_->setPropertyPages(captureInfo_.propertyPages);
                        infoDlg_->setCurrentAudioDevice(recordAudioDeviceIdUtf8_);
                    }

                    if (DpinfoDlg_ && DpinfoDlg_->isVisible())
                        DpinfoDlg_->setInfoText(formatDisplayOutputInfo(displayInfo_));
                } });
    runtimeStatusTimer_->start();
}

void MainWindow::setupDebugDock()
{
    debugDock_ = new QDockWidget(tr("Debug Log"), this);
    debugDock_->setObjectName("DebugLogDock");
    debugDock_->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::RightDockWidgetArea);

    debugText_ = new QPlainTextEdit(debugDock_);
    debugText_->setReadOnly(true);
    debugText_->document()->setMaximumBlockCount(2000);
    debugDock_->setWidget(debugText_);

    addDockWidget(Qt::BottomDockWidgetArea, debugDock_);
    debugDock_->hide();

    if (ui->actionDebugLog)
    {
        ui->actionDebugLog->setCheckable(true);
        ui->actionDebugLog->setChecked(false);
    }
}

void MainWindow::setupProcAmpAction()
{
    m_currentProcAmp = {128, 128, 128, 128, 128};
    if (!ui->actionProcAmp)
        return;

    connect(ui->actionProcAmp, &QAction::triggered, this, [this]()
            {
                if (!procampDlg_)
                {
                    procampDlg_ = new ProcAmp(this);
                    procampDlg_->setWindowTitle(tr("ProcAmp"));
                    procampDlg_->setValues(m_currentProcAmp);
                    connect(procampDlg_, &ProcAmp::valuesChanged,
                            this,
                            [this](const gcap_procamp_t &p)
                            {
                                m_currentProcAmp = p;
                                if (h_)
                                    gcap_set_procamp(h_, &p);
                            });
                }

                usePacketCallback_ = false;

                bool supported = false;
                if (h_)
                {
                    gcap_procamp_caps_t caps{};
                    if (gcap_get_procamp_caps(h_, &caps) == GCAP_OK)
                    {
                        procampDlg_->setCaps(caps);
                        supported = true;

                        gcap_procamp_t current{};
                        if (gcap_get_procamp(h_, &current) == GCAP_OK)
                            m_currentProcAmp = current;
                    }
                }
                else
                {
                    // No active SDK handle yet. Keep the dialog usable for pre-start values;
                    // the values are applied in startCapture() after gcap_open().
                    const int backend = ui->comboBackend ? ui->comboBackend->currentData().toInt() : 1;
                    supported = (backend == 0 || backend == 1 || backend == 3);
                }

                procampDlg_->setControlsEnabled(supported);
                procampDlg_->setValues(m_currentProcAmp);
                procampDlg_->show();
                procampDlg_->raise();
                procampDlg_->activateWindow(); });
}

void MainWindow::setupBackendControls()
{
    if (!ui->comboBackend)
        return;

    const QSignalBlocker blocker(ui->comboBackend);
    ui->comboBackend->clear();
    ui->comboBackend->addItem("DirectShow", 2);
    ui->comboBackend->addItem("WinMF GPU", 1);
    ui->comboBackend->addItem("WinMF CPU", 0);

#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
    ui->comboBackend->addItem("GVFG Direct", kQtViewerGvfgBackend);
#endif

    const int dsIndex = ui->comboBackend->findData(2);
    ui->comboBackend->setCurrentIndex(dsIndex >= 0 ? dsIndex : 0);

    refreshPixelFormatOptions(false);
}

void MainWindow::setupPreviewBitDepthControls()
{
    if (!ui->comboPreviewBitDepth)
        return;

    const QSignalBlocker blocker(ui->comboPreviewBitDepth);
    ui->comboPreviewBitDepth->clear();
    ui->comboPreviewBitDepth->addItem(QStringLiteral("Preview: Auto"), GCAP_PREVIEW_BITDEPTH_AUTO);
    ui->comboPreviewBitDepth->addItem(QStringLiteral("Preview: 10-bit"), GCAP_PREVIEW_BITDEPTH_10BIT);
    ui->comboPreviewBitDepth->addItem(QStringLiteral("Preview: 8-bit"), GCAP_PREVIEW_BITDEPTH_8BIT);
    ui->comboPreviewBitDepth->setCurrentIndex(0);
}

int MainWindow::selectedPreviewBitDepthMode() const
{
    if (!ui->comboPreviewBitDepth)
        return GCAP_PREVIEW_BITDEPTH_AUTO;

    const int mode = ui->comboPreviewBitDepth->currentData().toInt();
    if (mode == GCAP_PREVIEW_BITDEPTH_8BIT ||
        mode == GCAP_PREVIEW_BITDEPTH_10BIT ||
        mode == GCAP_PREVIEW_BITDEPTH_AUTO)
    {
        return mode;
    }
    return GCAP_PREVIEW_BITDEPTH_AUTO;
}

QString MainWindow::selectedPreviewBitDepthText() const
{
    if (!ui->comboPreviewBitDepth)
        return QStringLiteral("Preview: Auto");
    return ui->comboPreviewBitDepth->currentText();
}

void MainWindow::applyPreviewSettingsToActiveSession()
{
#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
    if (usingGvfg_ && gvfg_)
    {
        const bool ok = gvfg_->setPreview(previewWindow_ ? previewWindow_->previewHwnd() : nullptr,
                                           selectedPreviewBitDepthMode());
        MainWindow::postLog(QStringLiteral("[Preview] GVFG requested %1%2")
                                .arg(selectedPreviewBitDepthText())
                                .arg(ok ? QString() : QStringLiteral(" failed")),
                            !ok);
        updateRuntimeStatusUi();
        return;
    }
#endif
    if (!h_)
        return;

    void *hwnd = previewWindow_ ? previewWindow_->previewHwnd() : nullptr;

    gcap_preview_desc_t pv{};
    pv.hwnd = hwnd;
    pv.enable_preview = (hwnd != nullptr) ? 1 : 0;
    pv.use_fp16_pipeline = 1;
    pv.swapchain_10bit = selectedPreviewBitDepthMode();

    const gcap_status_t st = gcap_set_preview(h_, &pv);
    if (st != GCAP_OK)
    {
        MainWindow::postLog(QStringLiteral("[Preview] apply bit depth failed: mode=%1 status=%2")
                                .arg(pv.swapchain_10bit)
                                .arg(int(st)),
                            true);
        return;
    }

    MainWindow::postLog(QStringLiteral("[Preview] requested %1 mode=%2; swapchain will be recreated on next frame")
                            .arg(selectedPreviewBitDepthText())
                            .arg(pv.swapchain_10bit));
    updateRuntimeStatusUi();
}

void MainWindow::notifyPixelFormatEnumerationFailure(int backend)
{
    if (!ui->comboBackend)
        return;

    QString title = QStringLiteral("Pixel Format");
    QString message;
    bool showDialog = true;
    switch (backend)
    {
    case GCAP_BACKEND_DSHOW:
        message = QStringLiteral("DirectShow format enumeration failed; keeping Format: Auto.");
        showDialog = false;
        break;
    case GCAP_BACKEND_WINMF_CPU:
    case GCAP_BACKEND_WINMF_GPU:
        message = QStringLiteral("This device may not support Media Foundation. Please use DirectShow instead.");
        break;
    default:
        return;
    }

    const QString warningKey = QStringLiteral("%1:%2").arg(deviceIndex_).arg(backend);
    if (lastPixelFormatWarningKey_ == warningKey)
        return;

    lastPixelFormatWarningKey_ = warningKey;
    if (showDialog)
    {
        QMessageBox::warning(this, title, message);
    }
    else
    {
        MainWindow::postLog(QStringLiteral("[PixelFormat] %1").arg(message), true);
        if (ui->statusbar)
            ui->statusbar->showMessage(message, 6000);
    }
}

void MainWindow::refreshPixelFormatOptions(bool showFailurePrompt)
{
    if (!ui->comboPixelFormat)
        return;

    const QSignalBlocker blocker(ui->comboPixelFormat);
    const int previousData = ui->comboPixelFormat->currentData().toInt();

    ui->comboPixelFormat->setToolTip(tr("Requested capture format for the next Start. The actual negotiated format is shown in BackendFmt."));
    ui->comboPixelFormat->clear();
    ui->comboPixelFormat->addItem(QStringLiteral("Format: Auto"), -1);

    const int backend = ui->comboBackend ? ui->comboBackend->currentData().toInt() : GCAP_BACKEND_DSHOW;
    const auto supported = (deviceIndex_ >= 0)
                               ? enumerateSupportedPixelFormats(backend, deviceIndex_)
                               : std::vector<gcap_pixfmt_t>{};
    for (const auto fmt : supported)
    {
        const QString label = pixelFormatComboLabel(fmt);
        if (!label.isEmpty())
            ui->comboPixelFormat->addItem(label, static_cast<int>(fmt));
    }

    if (!supported.empty())
        lastPixelFormatWarningKey_.clear();
    else if (showFailurePrompt && deviceIndex_ >= 0)
        notifyPixelFormatEnumerationFailure(backend);

    int restoreIndex = ui->comboPixelFormat->findData(previousData);
    if (restoreIndex < 0)
        restoreIndex = 0;
    ui->comboPixelFormat->setCurrentIndex(restoreIndex);
}

void MainWindow::initializeDeviceList()
{
    if (!ui->comboDevice)
        return;

    const int backend = ui->comboBackend ? ui->comboBackend->currentData().toInt() : GCAP_BACKEND_DSHOW;
    const QString previousDeviceName = ui->comboDevice->currentText();

    // gcap_enumerate() uses CaptureManager's currently selected backend.
    // Keep the SDK backend in sync with the UI before rebuilding the device list,
    // otherwise WinMF and DirectShow indexes can be mixed up.
#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
    if (backend != kQtViewerGvfgBackend)
#endif
        gcap_set_backend(backend);

    const QSignalBlocker blocker(ui->comboDevice);
    ui->comboDevice->clear();

#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
    if (backend == kQtViewerGvfgBackend)
    {
        const QStringList devices = GvfgSource::enumerateDevices();
        for (int i = 0; i < devices.size(); ++i)
            ui->comboDevice->addItem(devices.at(i), i);
    }
    else
#endif
    {
        gcap_device_info_t list[16];
        int n = 0;
        if (gcap_enumerate(list, 16, &n) == GCAP_OK)
        {
            for (int i = 0; i < n; ++i)
                ui->comboDevice->addItem(QString::fromUtf8(list[i].name), i);
        }
    }

    int restoreIndex = -1;
    if (!previousDeviceName.isEmpty())
        restoreIndex = ui->comboDevice->findText(previousDeviceName);
    if (restoreIndex < 0 && ui->comboDevice->count() > 0)
        restoreIndex = 0;

    if (restoreIndex >= 0)
    {
        ui->comboDevice->setCurrentIndex(restoreIndex);
        deviceIndex_ = ui->comboDevice->itemData(restoreIndex).toInt();
    }
    else
    {
        deviceIndex_ = -1;
    }

    invalidateDeviceCapabilityCache();
    lastPixelFormatWarningKey_.clear();

    MainWindow::postLog(QStringLiteral("[DeviceList] backend=%1 devices=%2 selectedIndex=%3 selectedName=%4")
                            .arg(backend)
                            .arg(ui->comboDevice->count())
                            .arg(deviceIndex_)
                            .arg(ui->comboDevice->currentText()));

    refreshPixelFormatOptions(true);
}

void MainWindow::initializeGpuList()
{
#ifdef _WIN32
    if (!ui->comboGpu)
        return;

    ComPtr<IDXGIFactory1> fac;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(fac.GetAddressOf()))) && fac)
    {
        UINT idx = 0;
        while (true)
        {
            ComPtr<IDXGIAdapter1> ad;
            HRESULT hr = fac->EnumAdapters1(idx, &ad);
            if (hr == DXGI_ERROR_NOT_FOUND)
                break;
            if (FAILED(hr) || !ad)
            {
                ++idx;
                continue;
            }

            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(ad->GetDesc1(&desc)))
            {
                if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
                {
                    ++idx;
                    continue;
                }
                QString name = QString::fromWCharArray(desc.Description);
                ui->comboGpu->addItem(name, static_cast<int>(idx));
            }
            ++idx;
        }
    }

    if (ui->comboGpu->count() == 0)
        ui->comboGpu->addItem(QStringLiteral("Default GPU (DXGI)"), -1);

    ui->comboGpu->setCurrentIndex(0);
    gpuIndex_ = ui->comboGpu->currentData().toInt();
    gpuName_ = ui->comboGpu->currentText();
    gcap_set_d3d_adapter(gpuIndex_);
#else
    Q_UNUSED(this);
#endif
}

void MainWindow::setupConnections()
{
    if (ui->comboDevice)
    {
        connect(ui->comboDevice, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int idx)
                {
                    if (idx < 0)
                    {
                        deviceIndex_ = -1;
                        invalidateDeviceCapabilityCache();
                        refreshPixelFormatOptions(false);
                        return;
                    }

                    bool ok = false;
                    const int selectedDeviceIndex = ui->comboDevice->itemData(idx).toInt(&ok);
                    deviceIndex_ = ok ? selectedDeviceIndex : -1;
                    invalidateDeviceCapabilityCache();
                    refreshPixelFormatOptions(true);
                    refreshCaptureInfoFromSdkAndRuntime(false);
                    if (infoDlg_ && infoDlg_->isVisible())
                    {
                        infoDlg_->setInfoText(lastInfoText_);
                        infoDlg_->setPropertyPages(captureInfo_.propertyPages);
                    } });
    }

    if (ui->comboBackend)
    {
        connect(ui->comboBackend, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int)
                {
                    const int backend = ui->comboBackend->currentData().toInt();
                    const bool isGvfgBackend =
#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
                        (backend == kQtViewerGvfgBackend);
#else
                        false;
#endif
                    if (ui->comboDevice)
                        ui->comboDevice->setEnabled(true);

                    // Re-enumerate devices when backend changes. Device index is only
                    // meaningful within the backend that produced the list.
                    if (!isGvfgBackend)
                    {
                        initializeDeviceList();
                        refreshCaptureInfoFromSdkAndRuntime(false);
                        if (infoDlg_ && infoDlg_->isVisible())
                        {
                            infoDlg_->setInfoText(lastInfoText_);
                            infoDlg_->setPropertyPages(captureInfo_.propertyPages);
                        }
                    }
                    else
                    {
                        initializeDeviceList();
                        refreshCaptureInfoFromSdkAndRuntime(false);
                        if (infoDlg_ && infoDlg_->isVisible())
                        {
                            infoDlg_->setInfoText(lastInfoText_);
                            infoDlg_->setPropertyPages(captureInfo_.propertyPages);
                        }
                    } });
    }

    if (ui->comboPreviewBitDepth)
    {
        connect(ui->comboPreviewBitDepth, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int)
                { applyPreviewSettingsToActiveSession(); });
    }

#ifdef _WIN32
    if (ui->comboGpu)
    {
        connect(ui->comboGpu,
                QOverload<int>::of(&QComboBox::currentIndexChanged),
                this,
                [this](int idx)
                {
                    gpuIndex_ = ui->comboGpu->itemData(idx).toInt();
                    gpuName_ = ui->comboGpu->itemText(idx);
                    gcap_set_d3d_adapter(gpuIndex_);
                });
    }
#endif

    connect(ui->btnStart, &QPushButton::clicked, this, &MainWindow::onStart);
    connect(ui->btnStop, &QPushButton::clicked, this, &MainWindow::onStop);
    connect(this, &MainWindow::sigFrame, this, &MainWindow::onFrameArrived, Qt::QueuedConnection);
    if (ui->actionDebugLog)
        connect(ui->actionDebugLog, &QAction::toggled, this, &MainWindow::onToggleDebugLog);
    if (debugDock_ && ui->actionDebugLog)
        connect(debugDock_, &QDockWidget::visibilityChanged, ui->actionDebugLog, &QAction::setChecked);
    if (ui->btnShowEdid)
        connect(ui->btnShowEdid, &QPushButton::clicked, this, &MainWindow::onShowEdid);
    if (ui->btnRecord)
        connect(ui->btnRecord, &QPushButton::clicked, this, &MainWindow::onRecord);
    if (ui->actionOpenRecordFolder)
        connect(ui->actionOpenRecordFolder, &QAction::triggered, this, &MainWindow::onOpenRecordFolder);
    if (ui->actionOpenLogFolder)
        connect(ui->actionOpenLogFolder, &QAction::triggered, this, &MainWindow::onOpenLogFolder);
    if (ui->actionOpenSnapshot)
        connect(ui->actionOpenSnapshot, &QAction::triggered, this, &MainWindow::onOpenSnapshot);
    if (ui->actionOpenGigabyteRaw)
        connect(ui->actionOpenGigabyteRaw, &QAction::triggered, this, &MainWindow::onOpenGigabyteRaw);
    if (ui->actionInputInfo)
        connect(ui->actionInputInfo, &QAction::triggered, this, &MainWindow::onShowInputInfo);
    if (ui->actionDisplayInfo)
        connect(ui->actionDisplayInfo, &QAction::triggered, this, &MainWindow::onShowDisplayInfo);
    if (ui->btnSnapshot)
        connect(ui->btnSnapshot, &QPushButton::clicked, this, &MainWindow::onSnapshot);
    if (ui->btnOpenTiff)
        connect(ui->btnOpenTiff, &QPushButton::clicked, this, &MainWindow::onOpenTiffAnalyze);
    if (ui->actionOpenTiffAnalyzer)
        connect(ui->actionOpenTiffAnalyzer, &QAction::triggered, this, &MainWindow::onOpenTiffAnalyze);
}

void MainWindow::logStartupInfo()
{
    MainWindow::postLog(QStringLiteral("Viewer version: %1").arg(QString::fromLatin1(QT6_VIEWER_VERSION)));
    MainWindow::postLog(QStringLiteral("gcapture SDK version: %1").arg(QString::fromUtf8(gcap_version_string())));

    const QString logPath = qApp ? qApp->property("logPath").toString() : QString();
    if (!logPath.isEmpty())
        MainWindow::postLog(QStringLiteral("Log file: %1").arg(logPath));
}

void MainWindow::onOpenGigabyteRaw()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        tr("Open GIGABYTE RAW"),
        QCoreApplication::applicationDirPath() + "/snapshots",
        tr("GIGABYTE RAW (*.raw);;All Files (*.*)"));
    if (path.isEmpty())
        return;

    const GigabyteRawLoadResult loaded = loadGigabyteRawImage(path);
    if (loaded.image.isNull())
    {
        QMessageBox::warning(this,
                             tr("Open GIGABYTE RAW"),
                             tr("Failed to load GIGABYTE RAW:\n%1\n\n%2").arg(path, loaded.error));
        return;
    }

    if (!rawPreviewWindow_)
    {
        rawPreviewWindow_ = new previewwindow();
        rawPreviewWindow_->setWindowTitle(QStringLiteral("GIGABYTE RAW Preview"));
        rawPreviewWindow_->resize(1280, 720);
    }

    rawPreviewWindow_->show();
    rawPreviewWindow_->resizeToSourceContent(loaded.image.width(), loaded.image.height());
    rawPreviewWindow_->setImportedFrame(loaded.image);
    rawPreviewWindow_->raise();
    rawPreviewWindow_->activateWindow();

    const QString fileName = QFileInfo(path).fileName();
    MainWindow::postLog(QStringLiteral("[GIGABYTE_RAW] loaded %1 format=%2 sourceBitDepth=%3 size=%4x%5")
                            .arg(fileName,
                                 loaded.format,
                                 QString::number(loaded.sourceBitDepth),
                                 QString::number(loaded.image.width()),
                                 QString::number(loaded.image.height())));
    if (ui->statusbar)
        ui->statusbar->showMessage(QStringLiteral("Loaded GIGABYTE RAW: %1").arg(fileName), 6000);
}

void MainWindow::postLog(const QString &line, bool isError)
{
    // 1) Write through the Qt message handler.
    if (isError)
        qWarning().noquote() << line;
    else
        qInfo().noquote() << line;

    // 2) Mirror to the UI when available.
    if (g_mainWindow)
    {
        QMetaObject::invokeMethod(
            g_mainWindow,
            "appendDebugLog",
            Qt::QueuedConnection,
            Q_ARG(QString, line));
    }
}

void MainWindow::appendDebugLog(const QString &line)
{
    if (!debugText_)
        return;

    const QString ts = QDateTime::currentDateTime().toString("HH:mm:ss.zzz");
    debugText_->appendPlainText(QStringLiteral("[%1] %2").arg(ts, line));
}

void MainWindow::onOpenTiffAnalyze()
{
    if (openingTiffDialog_)
        return;

    openingTiffDialog_ = true;
    suppressAuxDialogRefresh_ = true;

    const bool wasTimerActive = runtimeStatusTimer_ && runtimeStatusTimer_->isActive();
    if (wasTimerActive)
        runtimeStatusTimer_->stop();

    QString path;
    {
        QFileDialog dlg(this,
                        tr("Open TIFF"),
                        QString(),
                        tr("TIFF Files (*.tif *.tiff)"));
#ifdef _WIN32
        dlg.setOption(QFileDialog::DontUseNativeDialog, true);
#endif
        dlg.setFileMode(QFileDialog::ExistingFile);
        if (dlg.exec() == QDialog::Accepted)
            path = dlg.selectedFiles().value(0);
    }

    if (wasTimerActive && runtimeStatusTimer_)
        runtimeStatusTimer_->start();
    suppressAuxDialogRefresh_ = false;
    openingTiffDialog_ = false;

    if (path.isEmpty())
        return;

    lastTiffReport_ = TiffAnalyzer::analyzeFile(path);

    if (!tiffAnalysisDlg_)
        tiffAnalysisDlg_ = new TiffAnalysisDialog(this);

    tiffAnalysisDlg_->setReport(lastTiffReport_);
    tiffAnalysisDlg_->show();
    tiffAnalysisDlg_->raise();
    tiffAnalysisDlg_->activateWindow();

    if (ui && ui->labelinfo1)
    {
        if (lastTiffReport_.ok)
        {
            ui->labelinfo1->setEnabled(true);
            ui->labelinfo1->setText(
                tr("TIFF: %1 | stored=%2-bit | effective=%3-bit | 10-bit evidence=%4 | ramp=%5")
                    .arg(QFileInfo(path).fileName())
                    .arg(lastTiffReport_.storedBitDepth)
                    .arg(lastTiffReport_.effectiveBitDepth)
                    .arg(lastTiffReport_.likelyTenBitContent ? tr("Yes") : tr("No"))
                    .arg(lastTiffReport_.rampStatusText));
        }
        else
        {
            ui->labelinfo1->setEnabled(true);
            ui->labelinfo1->setText(tr("TIFF analyze failed: %1").arg(lastTiffReport_.error));
        }
    }

    if (lastTiffReport_.ok)
    {
        MainWindow::postLog(
            QStringLiteral("[TIFF] %1 stored=%2 effective=%3 likely10=%4 rampStatus=%5 fmt=%6 range=%7..%8 unique=%9")
                .arg(path)
                .arg(lastTiffReport_.storedBitDepth)
                .arg(lastTiffReport_.effectiveBitDepth)
                .arg(lastTiffReport_.likelyTenBitContent ? QStringLiteral("Yes") : QStringLiteral("No"))
                .arg(lastTiffReport_.rampStatusText)
                .arg(lastTiffReport_.pixelFormatName)
                .arg(lastTiffReport_.minValue)
                .arg(lastTiffReport_.maxValue)
                .arg(lastTiffReport_.uniqueValueCount));
    }
    else
    {
        MainWindow::postLog(QStringLiteral("[TIFF] analyze failed: %1 (%2)").arg(path, lastTiffReport_.error), true);
    }
}

void MainWindow::on_btnPreview_clicked()
{
    setupPreviewWindow();
    previewWindow_->show();
    previewWindow_->raise();
    previewWindow_->activateWindow();
}
