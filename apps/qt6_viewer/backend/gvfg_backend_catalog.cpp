#include "capture_backend_catalog.h"

#include "gvfg/gvfg_source.h"

#if defined(_WIN32) && defined(QT6_VIEWER_ENABLE_GVFG_BACKEND)
namespace
{
class GvfgBackendCatalog final : public CaptureBackendCatalog
{
public:
    CaptureBackend backend() const override { return CaptureBackend::Gvfg; }
    QString displayName() const override { return QStringLiteral("GVFG Direct"); }

    QList<CaptureBackendDevice> devices() override
    {
        const QStringList found = GvfgSource::enumerateDevices();
        QList<CaptureBackendDevice> result;
        for (int i = 0; i < found.size(); ++i)
            result.push_back({found.at(i), i});
        return result;
    }

    CaptureBackendFormats formats(int deviceIndex) override
    {
        Q_UNUSED(deviceIndex);
        CaptureBackendFormats result;
        result.items.push_back({QStringLiteral("YUY2 (8-bit 4:2:2)"), static_cast<int>(GVFG_PIXFMT_YUY2)});
        result.items.push_back({QStringLiteral("Y210 (10-bit 4:2:2)"), static_cast<int>(GVFG_PIXFMT_Y210)});
        return result;
    }

    CaptureBackendCapabilities capabilities(int deviceIndex) override
    {
        Q_UNUSED(deviceIndex);
        CaptureBackendCapabilities result;
        result.formats << QStringLiteral("YUY2") << QStringLiteral("Y210");
        return result;
    }

    QString formatToolTip() const override
    {
        return QStringLiteral("GVFG output format controlled through the SDK API (register 0x80).");
    }
};
} // namespace

std::unique_ptr<CaptureBackendCatalog> createGvfgBackendCatalog()
{
    return std::make_unique<GvfgBackendCatalog>();
}
#endif
