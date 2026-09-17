#include "rawinspectordialog.h"
#include "rawpreviewwidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QDoubleSpinBox>

RawInspectorDialog::RawInspectorDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Pixel Inspector"));
    resize(1200, 780);

    auto *layout = new QVBoxLayout(this);
    auto *fileRow = new QHBoxLayout;
    fileLabel_ = new QLabel(this);
    fileLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    fileLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    previousFileButton_ = new QPushButton(QStringLiteral("Previous"), this);
    nextFileButton_ = new QPushButton(QStringLiteral("Next"), this);
    auto *openFileButton = new QPushButton(QStringLiteral("Open File..."), this);
    fileRow->addWidget(fileLabel_, 1);
    fileRow->addWidget(previousFileButton_);
    fileRow->addWidget(nextFileButton_);
    fileRow->addWidget(openFileButton);
    layout->addLayout(fileRow);

    auto *settings = new QHBoxLayout;
    formatCombo_ = new QComboBox(this);
    formatCombo_->addItem(QStringLiteral("YUY2 (8-bit 4:2:2, Y0 U Y1 V)"), int(RawPixelFormat::Yuy2));
    formatCombo_->addItem(QStringLiteral("Y210 (10-bit 4:2:2)"), int(RawPixelFormat::Y210));
    formatCombo_->addItem(QStringLiteral("NV12 (8-bit 4:2:0, Y + UV)"), int(RawPixelFormat::Nv12));
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
    windowCenterSpin_ =
        new QDoubleSpinBox(this);

    windowWidthSpin_ =
        new QDoubleSpinBox(this);

    windowCenterSpin_->setRange(
        -1000000.0,
        1000000.0);

    windowWidthSpin_->setRange(
        1.0,
        2000000.0);

    windowCenterSpin_->setDecimals(3);
    windowWidthSpin_->setDecimals(3);

    applyWindowButton_ =
        new QPushButton(
            QStringLiteral("Apply Window"),
            this);
    frameSpin_ = new QSpinBox(this);
    frameSpin_->setRange(1, 1);
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
    settings->addWidget(
        new QLabel(
            QStringLiteral("WC:"),
            this));

    settings->addWidget(
        windowCenterSpin_);

    settings->addWidget(
        new QLabel(
            QStringLiteral("WW:"),
            this));

    settings->addWidget(
        windowWidthSpin_);

    settings->addWidget(
        applyWindowButton_);
    settings->addWidget(new QLabel(QStringLiteral("Frame:"), this));
    settings->addWidget(frameSpin_);
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
    connect(openFileButton, &QPushButton::clicked, this, &RawInspectorDialog::chooseFile);
    connect(previousFileButton_, &QPushButton::clicked, this, [this]()
            { navigateFile(-1); });
    connect(nextFileButton_, &QPushButton::clicked, this, [this]()
            { navigateFile(1); });
    connect(hexCheck_, &QCheckBox::toggled, viewer_, &RawPreviewWidget::setHexadecimal);
    connect(viewer_, &RawPreviewWidget::pixelTextChanged, pixelLabel_, &QLabel::setText);
    connect(viewer_, &RawPreviewWidget::zoomChanged, this, [this](double zoom)
            { zoomLabel_->setText(QStringLiteral("Zoom: %1x").arg(zoom, 0, 'f', 2)); });
    connect(reset, &QPushButton::clicked, viewer_, &RawPreviewWidget::resetView);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(
        applyWindowButton_,
        &QPushButton::clicked,
        this,
        &RawInspectorDialog::applyDicomWindow);
    connect(frameSpin_, qOverload<int>(&QSpinBox::valueChanged),
            this, &RawInspectorDialog::selectDicomFrame);
}

bool RawInspectorDialog::isDicomFile(
    const QString &path) const
{
    const QString suffix =
        QFileInfo(path)
            .suffix()
            .toLower();

    return suffix == QStringLiteral("dcm") ||
           suffix == QStringLiteral("dicom");
}

bool RawInspectorDialog::openFile(
    const QString &path)
{
    path_ =
        QFileInfo(path)
            .absoluteFilePath();

    dicomMode_ =
        isDicomFile(path_);

    fileLabel_->setText(path_);
    fileLabel_->setToolTip(path_);

    refreshFileNavigation();
    updateModeControls();

    if (!dicomMode_)
    {
        inferFromFileName(path_);
    }

    loadCurrent();

    return dicomMode_
               ? dicomFrame_.isValid()
               : frame_.isValid();
}

void RawInspectorDialog::chooseFile()
{
    QFileDialog dialog(this,
                       QStringLiteral("Open Pixel Data"),
                       QFileInfo(path_).absolutePath(),
                       QStringLiteral(
                           "Pixel Data (*.raw *.dcm *.dicom);;"
                           "RAW Frame Files (*.raw);;"
                           "DICOM Files (*.dcm *.dicom);;"
                           "All Files (*.*)"));
#ifdef _WIN32
    dialog.setOption(QFileDialog::DontUseNativeDialog, true);
#endif
    dialog.setFileMode(QFileDialog::ExistingFile);
    if (dialog.exec() == QDialog::Accepted)
        openFile(dialog.selectedFiles().value(0));
}

void RawInspectorDialog::navigateFile(int offset)
{
    const int target = currentFileIndex_ + offset;
    if (target >= 0 && target < siblingRawFiles_.size())
        openFile(siblingRawFiles_.at(target));
}

void RawInspectorDialog::refreshFileNavigation()
{
    siblingRawFiles_.clear();
    currentFileIndex_ = -1;

    const QFileInfo current(path_);
    const QFileInfoList entries = QDir(current.absolutePath()).entryInfoList(QDir::Files | QDir::Readable, QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo &entry : entries)
    {
        if (entry.suffix().compare(QStringLiteral("raw"), Qt::CaseInsensitive) != 0)
            continue;
        siblingRawFiles_.push_back(entry.absoluteFilePath());
        if (entry.absoluteFilePath().compare(current.absoluteFilePath(), Qt::CaseInsensitive) == 0)
            currentFileIndex_ = siblingRawFiles_.size() - 1;
    }

    previousFileButton_->setEnabled(currentFileIndex_ > 0);
    nextFileButton_->setEnabled(currentFileIndex_ >= 0 &&
                                currentFileIndex_ + 1 < siblingRawFiles_.size());
}

void RawInspectorDialog::inferFromFileName(const QString &path)
{
    const QString name = QFileInfo(path).fileName().toLower();
    RawPixelFormat format = RawPixelFormat::Yuy2;
    if (name.contains(QStringLiteral("y210")))
        format = RawPixelFormat::Y210;
    else if (name.contains(QStringLiteral("nv12")))
        format = RawPixelFormat::Nv12;
    else if (name.contains(QStringLiteral("rgb10a2")) ||
             name.contains(QStringLiteral("abgr2101010")) ||
             name.contains(QStringLiteral("r10g10b10a2")))
        format = RawPixelFormat::Abgr2101010;
    else if (name.contains(QStringLiteral("rgba8")))
        format = RawPixelFormat::Rgba8;
    else if (name.contains(QStringLiteral("bgra8")))
        format = RawPixelFormat::Bgra8;
    QFile sidecar(path + QStringLiteral(".json"));
    if (sidecar.open(QIODevice::ReadOnly))
    {
        const QJsonObject json = QJsonDocument::fromJson(sidecar.readAll()).object();
        const QString metadataFormat = json.value(QStringLiteral("pixelFormat")).toString().toLower();
        if (metadataFormat == QStringLiteral("y210"))
            format = RawPixelFormat::Y210;
        else if (metadataFormat == QStringLiteral("yuy2"))
            format = RawPixelFormat::Yuy2;
        else if (metadataFormat == QStringLiteral("nv12"))
            format = RawPixelFormat::Nv12;
        else if (metadataFormat == QStringLiteral("bgra8"))
            format = RawPixelFormat::Bgra8;
        else if (metadataFormat == QStringLiteral("rgba8"))
            format = RawPixelFormat::Rgba8;
        else if (metadataFormat == QStringLiteral("rgb10a2") ||
                 metadataFormat.contains(QStringLiteral("2101010")))
            format = RawPixelFormat::Abgr2101010;

        const QSignalBlocker blockFormat(formatCombo_);
        const QSignalBlocker blockWidth(widthSpin_);
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

    const int idx = formatCombo_->findData(int(format));
    if (idx >= 0)
        formatCombo_->setCurrentIndex(idx);

    const qint64 size = QFileInfo(path).size();
    const QList<QSize> common = {{3840, 2160}, {2560, 1440}, {1920, 1080}, {1280, 720}, {720, 480}};
    for (const QSize &s : common)
    {
        qint64 expectedSize = 0;
        if (format == RawPixelFormat::Nv12)
            expectedSize = qint64(s.width()) * (s.height() + s.height() / 2);
        else
            expectedSize = qint64(s.width()) * s.height() *
                           (format == RawPixelFormat::Yuy2 ? 2 : 4);
        if (expectedSize == size)
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

    //
    // DICOM
    //
    if (dicomMode_)
    {
        if (!dicomFrame_.load(
                path_,
                &error))
        {
            viewer_->setDicomFrame(
                nullptr,
                QImage());

            QMessageBox::warning(
                this,
                QStringLiteral(
                    "DICOM Pixel Inspector"),
                error);

            return;
        }

        if (dicomFrame_.hasWindow)
        {
            const QSignalBlocker block1(
                windowCenterSpin_);

            const QSignalBlocker block2(
                windowWidthSpin_);

            windowCenterSpin_->setValue(
                dicomFrame_.windowCenter);

            windowWidthSpin_->setValue(
                dicomFrame_.windowWidth);
        }

        {
            const QSignalBlocker block(frameSpin_);
            frameSpin_->setRange(1, qMax(1, dicomFrame_.frameCount));
            frameSpin_->setValue(1);
        }

        const QImage display =
            dicomFrame_.makeDisplayImage();

        if (display.isNull())
        {
            QMessageBox::warning(
                this,
                QStringLiteral(
                    "DICOM Pixel Inspector"),
                QStringLiteral(
                    "Failed to render DICOM image."));

            return;
        }

        viewer_->setDicomFrame(
            &dicomFrame_,
            display);

        pixelLabel_->setText(
            QStringLiteral(
                "%1 × %2 | "
                "DICOM %3 | "
                "%4-bit stored | Frame %5/%6 | "
                "WC %7 | WW %8")
                .arg(dicomFrame_.width)
                .arg(dicomFrame_.height)
                .arg(dicomFrame_.photometricInterpretation)
                .arg(dicomFrame_.bitsStored)
                .arg(dicomFrame_.frameIndex() + 1)
                .arg(dicomFrame_.frameCount)
                .arg(
                    dicomFrame_.isMonochrome() && dicomFrame_.hasWindow
                        ? QString::number(
                              dicomFrame_
                                  .windowCenter)
                        : QStringLiteral("-"))
                .arg(
                    dicomFrame_.isMonochrome() && dicomFrame_.hasWindow
                        ? QString::number(
                              dicomFrame_
                                  .windowWidth)
                        : QStringLiteral("-")));

        updateModeControls();

        return;
    }

    //
    // 原本 RAW
    //
    const auto format =
        RawPixelFormat(
            formatCombo_
                ->currentData()
                .toInt());

    if (!frame_.load(
            path_,
            widthSpin_->value(),
            heightSpin_->value(),
            strideSpin_->value(),
            format,
            &error))
    {
        viewer_->setFrame(nullptr);

        QMessageBox::warning(
            this,
            QStringLiteral(
                "RAW Pixel Inspector"),
            error);

        return;
    }

    viewer_->setFrame(&frame_);

    pixelLabel_->setText(
        QStringLiteral(
            "%1 × %2, "
            "stride %3, "
            "%4 bytes")
            .arg(frame_.width)
            .arg(frame_.height)
            .arg(frame_.strideBytes)
            .arg(frame_.bytes.size()));
}

void RawInspectorDialog::selectDicomFrame(int oneBasedFrame)
{
    if (!dicomMode_ || !dicomFrame_.setFrameIndex(oneBasedFrame - 1))
        return;

    const QImage display = dicomFrame_.makeDisplayImage();
    if (display.isNull())
    {
        QMessageBox::warning(this, QStringLiteral("DICOM Frame"),
                             QStringLiteral("Failed to render frame %1.").arg(oneBasedFrame));
        return;
    }
    viewer_->setDicomFrame(&dicomFrame_, display);
    pixelLabel_->setText(QStringLiteral("%1 × %2 | DICOM %3 | %4-bit stored | Frame %5/%6")
                             .arg(dicomFrame_.width).arg(dicomFrame_.height)
                             .arg(dicomFrame_.photometricInterpretation)
                             .arg(dicomFrame_.bitsStored).arg(oneBasedFrame)
                             .arg(dicomFrame_.frameCount));
}

void RawInspectorDialog::applyDicomWindow()
{
    if (!dicomMode_ ||
        !dicomFrame_.isValid() ||
        !dicomFrame_.isMonochrome())
    {
        return;
    }

    const double wc =
        windowCenterSpin_->value();

    const double ww =
        windowWidthSpin_->value();

    const QImage image =
        dicomFrame_.makeDisplayImage(
            wc,
            ww);

    if (image.isNull())
    {
        QMessageBox::warning(
            this,
            QStringLiteral(
                "DICOM Window"),
            QStringLiteral(
                "Failed to apply "
                "Window Center / Width."));

        return;
    }

    //
    // 不 Reset Zoom
    //
    viewer_->updateDicomDisplay(
        image);
}

void RawInspectorDialog::updateModeControls()
{
    const bool raw =
        !dicomMode_;

    formatCombo_->setEnabled(raw);
    widthSpin_->setEnabled(raw);
    heightSpin_->setEnabled(raw);
    strideSpin_->setEnabled(raw);

    const bool windowing = dicomMode_ &&
        (!dicomFrame_.isValid() || dicomFrame_.isMonochrome());
    windowCenterSpin_->setEnabled(windowing);

    windowWidthSpin_->setEnabled(
        windowing);

    applyWindowButton_->setEnabled(
        windowing);
    frameSpin_->setEnabled(dicomMode_ && dicomFrame_.frameCount > 1);
}
