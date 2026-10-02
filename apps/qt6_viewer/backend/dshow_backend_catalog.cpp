#include "capture_backend_catalog.h"

#include <gcapture.h>
#include <set>
#include <vector>

namespace
{
QString pixelFormatLabel(gcap_pixfmt_t format)
{
    if (format == GCAP_FMT_YUY2)
        return QStringLiteral("Format: YUY2/HDYC/UYVY");
    return QStringLiteral("Format: %1").arg(QString::fromUtf8(gcap_pixfmt_name(format)));
}

QString videoCapabilityLabel(const gcap_video_cap_t &capability)
{
    const double fps = capability.fps_den > 0
                           ? double(capability.fps_num) / double(capability.fps_den)
                           : 0.0;
    const QString format = capability.subtype_name[0]
                               ? QString::fromUtf8(capability.subtype_name)
                               : QString::fromUtf8(gcap_pixfmt_name(capability.pixfmt));
    QString text = QStringLiteral("%1 %2x%3").arg(format).arg(capability.width).arg(capability.height);
    if (fps > 0.0)
        text += QStringLiteral(" %1 fps").arg(fps, 0, 'f', 2);
    if (capability.bit_depth > 0)
        text += QStringLiteral(" (%1-bit)").arg(capability.bit_depth);
    return text;
}

class DirectShowBackendCatalog final : public CaptureBackendCatalog
{
public:
    CaptureBackend backend() const override { return CaptureBackend::DirectShow; }
    QString displayName() const override { return QStringLiteral("DirectShow"); }

    QList<CaptureBackendDevice> devices() override
    {
        gcap_set_backend(GCAP_BACKEND_DSHOW);
        gcap_device_info_t found[16]{};
        int count = 0;
        QList<CaptureBackendDevice> result;
        if (gcap_enumerate(found, 16, &count) != GCAP_OK)
            return result;
        for (int i = 0; i < count; ++i)
            result.push_back({QString::fromUtf8(found[i].name), i});
        return result;
    }

    CaptureBackendFormats formats(int deviceIndex) override
    {
        CaptureBackendFormats result;
        result.items.push_back({QStringLiteral("Format: Auto"), -1});
        if (deviceIndex < 0)
            return result;

        const int count = gcap_enum_supported_pixel_formats(GCAP_BACKEND_DSHOW, deviceIndex, nullptr, 0);
        result.querySucceeded = count > 0;
        if (count <= 0)
            return result;

        std::vector<gcap_pixfmt_t> found(static_cast<size_t>(count));
        const int written = gcap_enum_supported_pixel_formats(
            GCAP_BACKEND_DSHOW, deviceIndex, found.data(), static_cast<int>(found.size()));
        std::set<int> seen;
        for (int i = 0; i < written; ++i)
        {
            const gcap_pixfmt_t format = found[static_cast<size_t>(i)];
            if (gcap_pixfmt_bit_depth(format) > 0 && seen.insert(static_cast<int>(format)).second)
                result.items.push_back({pixelFormatLabel(format), static_cast<int>(format)});
        }
        return result;
    }

    CaptureBackendCapabilities capabilities(int deviceIndex) override
    {
        CaptureBackendCapabilities result;
        if (deviceIndex < 0)
            return result;

        const int capCount = gcap_enum_video_caps(deviceIndex, nullptr, 0);
        if (capCount > 0)
        {
            std::vector<gcap_video_cap_t> found(static_cast<size_t>(capCount));
            const int written = gcap_enum_video_caps(deviceIndex, found.data(), static_cast<int>(found.size()));
            for (int i = 0; i < written; ++i)
                result.formats << videoCapabilityLabel(found[static_cast<size_t>(i)]);
        }

        const int pageCount = gcap_enum_property_pages(deviceIndex, nullptr, 0);
        if (pageCount > 0)
        {
            std::vector<gcap_property_page_t> found(static_cast<size_t>(pageCount));
            const int written = gcap_enum_property_pages(deviceIndex, found.data(), static_cast<int>(found.size()));
            for (int i = 0; i < written; ++i)
            {
                const auto &page = found[static_cast<size_t>(i)];
                result.propertyPages << QStringLiteral("%1 - %2")
                                            .arg(QString::fromUtf8(page.page_name))
                                            .arg(page.capture_pin ? QStringLiteral("Capture Pin")
                                                                  : QStringLiteral("Filter"));
            }
        }
        return result;
    }

    QString formatToolTip() const override
    {
        return QStringLiteral("Requested DirectShow capture format for the next Start. The negotiated format is shown in BackendFmt.");
    }
};
} // namespace

std::unique_ptr<CaptureBackendCatalog> createDirectShowBackendCatalog()
{
    return std::make_unique<DirectShowBackendCatalog>();
}
