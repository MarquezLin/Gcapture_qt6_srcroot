#include "rawinspectordialog.h"
#include "rawpreviewwidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

RawInspectorDialog::RawInspectorDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("RAW Pixel Inspector"));
    resize(1200, 780);

    auto *layout = new QVBoxLayout(this);
    fileLabel_ = new QLabel(this);
    fileLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(fileLabel_);

    auto *settings = new QHBoxLayout;
    formatCombo_ = new QComboBox(this);
    formatCombo_->addItem(QStringLiteral("YUY2 (8-bit 4:2:2, Y0 U Y1 V)"), int(RawPixelFormat::Yuy2));
    formatCombo_->addItem(QStringLiteral("Y210 (10-bit 4:2:2)"), int(RawPixelFormat::Y210));
    formatCombo_->addItem(QStringLiteral("BGRA8"), int(RawPixelFormat::Bgra8));
    formatCombo_->addItem(QStringLiteral("RGBA8"), int(RawPixelFormat::Rgba8));
    formatCombo_->addItem(QStringLiteral("ABGR2101010 / R10G10B10A2"), int(RawPixelFormat::Abgr2101010));
    widthSpin_ = new QSpinBox(this);
    heightSpin_ = new QSpinBox(this);
    strideSpin_ = new QSpinBox(this);
    for (QSpinBox *spin : {widthSpin_, heightSpin_, strideSpin_})
        spin->setRange(1, 1000000);
    widthSpin_->setValue(1920);
    heightSpin_->setValue(1080);
    strideSpin_->setValue(3840);
    hexCheck_ = new QCheckBox(QStringLiteral("Hex values"), this);
    auto *reload = new QPushButton(QStringLiteral("Load / Apply"), this);
    settings->addWidget(new QLabel(QStringLiteral("Format:"), this));
    settings->addWidget(formatCombo_);
    settings->addWidget(new QLabel(QStringLiteral("Width:"), this));
    settings->addWidget(widthSpin_);
    settings->addWidget(new QLabel(QStringLiteral("Height:"), this));
    settings->addWidget(heightSpin_);
    settings->addWidget(new QLabel(QStringLiteral("Stride:"), this));
    settings->addWidget(strideSpin_);
    settings->addWidget(hexCheck_);
    settings->addWidget(reload);
    layout->addLayout(settings);

    viewer_ = new RawPreviewWidget(this);
    layout->addWidget(viewer_, 1);
    pixelLabel_ = new QLabel(QStringLiteral("Pixel: move cursor over image"), this);
    pixelLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pixelLabel_->setWordWrap(true);
    layout->addWidget(pixelLabel_);

    auto *bottom = new QHBoxLayout;
    zoomLabel_ = new QLabel(QStringLiteral("Zoom: 1.00x"), this);
    auto *reset = new QPushButton(QStringLiteral("Reset View"), this);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    bottom->addWidget(zoomLabel_);
    bottom->addWidget(reset);
    bottom->addStretch();
    bottom->addWidget(buttons);
    layout->addLayout(bottom);

    connect(formatCombo_, &QComboBox::currentIndexChanged, this, &RawInspectorDialog::applyFormatDefaults);
    connect(widthSpin_, qOverload<int>(&QSpinBox::valueChanged), this, &RawInspectorDialog::applyFormatDefaults);
    connect(reload, &QPushButton::clicked, this, &RawInspectorDialog::loadCurrent);
    connect(hexCheck_, &QCheckBox::toggled, viewer_, &RawPreviewWidget::setHexadecimal);
    connect(viewer_, &RawPreviewWidget::pixelTextChanged, pixelLabel_, &QLabel::setText);
    connect(viewer_, &RawPreviewWidget::zoomChanged, this, [this](double zoom) {
        zoomLabel_->setText(QStringLiteral("Zoom: %1x").arg(zoom, 0, 'f', 2));
    });
    connect(reset, &QPushButton::clicked, viewer_, &RawPreviewWidget::resetView);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

bool RawInspectorDialog::openFile(const QString &path)
{
    path_ = path;
    fileLabel_->setText(QFileInfo(path).absoluteFilePath());
    inferFromFileName(path);
    loadCurrent();
    return frame_.isValid();
}

void RawInspectorDialog::inferFromFileName(const QString &path)
{
    const QString name = QFileInfo(path).fileName().toLower();
    RawPixelFormat format = RawPixelFormat::Yuy2;
    if (name.contains(QStringLiteral("y210")))
        format = RawPixelFormat::Y210;
    else if (name.contains(QStringLiteral("abgr2101010")) || name.contains(QStringLiteral("r10g10b10a2")))
        format = RawPixelFormat::Abgr2101010;
    else if (name.contains(QStringLiteral("rgba8")))
        format = RawPixelFormat::Rgba8;
    else if (name.contains(QStringLiteral("bgra8")))
        format = RawPixelFormat::Bgra8;
    const int idx = formatCombo_->findData(int(format));
    if (idx >= 0)
        formatCombo_->setCurrentIndex(idx);

    QFile sidecar(path + QStringLiteral(".json"));
    if (sidecar.open(QIODevice::ReadOnly))
    {
        const QJsonObject json = QJsonDocument::fromJson(sidecar.readAll()).object();
        const QString metadataFormat = json.value(QStringLiteral("pixelFormat")).toString().toLower();
        if (metadataFormat == QStringLiteral("y210")) format = RawPixelFormat::Y210;
        else if (metadataFormat == QStringLiteral("yuy2")) format = RawPixelFormat::Yuy2;
        else if (metadataFormat == QStringLiteral("bgra8")) format = RawPixelFormat::Bgra8;
        else if (metadataFormat == QStringLiteral("rgba8")) format = RawPixelFormat::Rgba8;
        else if (metadataFormat.contains(QStringLiteral("2101010"))) format = RawPixelFormat::Abgr2101010;
        const int metadataIndex = formatCombo_->findData(int(format));
        if (metadataIndex >= 0)
            formatCombo_->setCurrentIndex(metadataIndex);
        if (json.value(QStringLiteral("width")).toInt() > 0)
            widthSpin_->setValue(json.value(QStringLiteral("width")).toInt());
        if (json.value(QStringLiteral("height")).toInt() > 0)
            heightSpin_->setValue(json.value(QStringLiteral("height")).toInt());
        const int metadataStride = json.value(QStringLiteral("strideBytes")).toInt();
        if (metadataStride > 0)
            strideSpin_->setValue(metadataStride);
        return;
    }

    const qint64 size = QFileInfo(path).size();
    const int bpp = (format == RawPixelFormat::Yuy2) ? 2 : 4;
    const QList<QSize> common = {{3840, 2160}, {2560, 1440}, {1920, 1080}, {1280, 720}, {720, 480}};
    for (const QSize &s : common)
    {
        if (qint64(s.width()) * s.height() * bpp == size)
        {
            widthSpin_->setValue(s.width());
            heightSpin_->setValue(s.height());
            break;
        }
    }
    applyFormatDefaults();
}

void RawInspectorDialog::applyFormatDefaults()
{
    const auto format = RawPixelFormat(formatCombo_->currentData().toInt());
    strideSpin_->setValue(RawFrame::minimumStride(widthSpin_->value(), format));
}

void RawInspectorDialog::loadCurrent()
{
    QString error;
    const auto format = RawPixelFormat(formatCombo_->currentData().toInt());
    if (!frame_.load(path_, widthSpin_->value(), heightSpin_->value(), strideSpin_->value(), format, &error))
    {
        viewer_->setFrame(nullptr);
        QMessageBox::warning(this, QStringLiteral("RAW Pixel Inspector"), error);
        return;
    }
    viewer_->setFrame(&frame_);
    pixelLabel_->setText(QStringLiteral("%1 × %2, stride %3, %4 bytes")
                             .arg(frame_.width).arg(frame_.height).arg(frame_.strideBytes).arg(frame_.bytes.size()));
}
