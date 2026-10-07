#include "dicomviewerdialog.h"
#include "rawpreviewwidget.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <utility>

DicomViewerDialog::DicomViewerDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("DICOM Viewer"));
    resize(1200, 800);

    auto *layout = new QVBoxLayout(this);
    auto *fileRow = new QHBoxLayout;
    pathLabel_ = new QLabel(this);
    pathLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pathLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto *openFileButton = new QPushButton(QStringLiteral("Open File..."), this);
    auto *openFolderButton = new QPushButton(QStringLiteral("Open Folder..."), this);
    fileRow->addWidget(pathLabel_, 1);
    fileRow->addWidget(openFileButton);
    fileRow->addWidget(openFolderButton);
    layout->addLayout(fileRow);

    auto *seriesRow = new QHBoxLayout;
    seriesCombo_ = new QComboBox(this);
    sliceSpin_ = new QSpinBox(this);
    frameSpin_ = new QSpinBox(this);
    sliceSpin_->setRange(1, 1);
    frameSpin_->setRange(1, 1);
    seriesRow->addWidget(new QLabel(QStringLiteral("Series:"), this));
    seriesRow->addWidget(seriesCombo_, 1);
    seriesRow->addWidget(new QLabel(QStringLiteral("Slice:"), this));
    seriesRow->addWidget(sliceSpin_);
    frameLabel_ = new QLabel(QStringLiteral("Frame:"), this);
    seriesRow->addWidget(frameLabel_);
    seriesRow->addWidget(frameSpin_);
    layout->addLayout(seriesRow);

    auto *windowRow = new QHBoxLayout;
    windowCenterSpin_ = new QDoubleSpinBox(this);
    windowWidthSpin_ = new QDoubleSpinBox(this);
    windowCenterSpin_->setRange(-1000000.0, 1000000.0);
    windowWidthSpin_->setRange(1.0, 2000000.0);
    windowCenterSpin_->setDecimals(3);
    windowWidthSpin_->setDecimals(3);
    applyWindowButton_ = new QPushButton(QStringLiteral("Apply Window"), this);
    windowRow->addWidget(new QLabel(QStringLiteral("Window Center:"), this));
    windowRow->addWidget(windowCenterSpin_);
    windowRow->addWidget(new QLabel(QStringLiteral("Window Width:"), this));
    windowRow->addWidget(windowWidthSpin_);
    windowRow->addWidget(applyWindowButton_);
    windowRow->addStretch();
    layout->addLayout(windowRow);

    viewer_ = new RawPreviewWidget(this);
    layout->addWidget(viewer_, 1);
    pixelLabel_ = new QLabel(QStringLiteral("Pixel: move cursor over image"), this);
    pixelLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pixelLabel_->setWordWrap(true);
    layout->addWidget(pixelLabel_);

    auto *bottom = new QHBoxLayout;
    zoomLabel_ = new QLabel(QStringLiteral("Zoom: 1.00x"), this);
    auto *resetButton = new QPushButton(QStringLiteral("Reset View"), this);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    bottom->addWidget(zoomLabel_);
    bottom->addWidget(resetButton);
    bottom->addStretch();
    bottom->addWidget(buttons);
    layout->addLayout(bottom);

    connect(openFileButton, &QPushButton::clicked, this, &DicomViewerDialog::chooseFile);
    connect(openFolderButton, &QPushButton::clicked, this, &DicomViewerDialog::chooseFolder);
    connect(seriesCombo_, &QComboBox::currentIndexChanged,
            this, &DicomViewerDialog::selectSeries);
    connect(sliceSpin_, qOverload<int>(&QSpinBox::valueChanged),
            this, &DicomViewerDialog::selectSlice);
    connect(frameSpin_, qOverload<int>(&QSpinBox::valueChanged),
            this, &DicomViewerDialog::selectFrame);
    connect(applyWindowButton_, &QPushButton::clicked,
            this, &DicomViewerDialog::applyWindow);
    connect(viewer_, &RawPreviewWidget::pixelTextChanged,
            pixelLabel_, &QLabel::setText);
    connect(viewer_, &RawPreviewWidget::zoomChanged, this, [this](double zoom) {
        zoomLabel_->setText(QStringLiteral("Zoom: %1x").arg(zoom, 0, 'f', 2));
    });
    connect(resetButton, &QPushButton::clicked, viewer_, &RawPreviewWidget::resetView);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    updateControls();
}

bool DicomViewerDialog::openFile(const QString &path)
{
    currentPath_ = QFileInfo(path).absoluteFilePath();
    series_.clear();
    currentSeriesIndex_ = -1;
    {
        const QSignalBlocker block(seriesCombo_);
        seriesCombo_->clear();
    }
    {
        const QSignalBlocker block(sliceSpin_);
        sliceSpin_->setRange(1, 1);
        sliceSpin_->setValue(1);
    }

    QString error;
    if (!frame_.load(currentPath_, &error))
    {
        viewer_->setDicomFrame(nullptr, QImage());
        QMessageBox::warning(this, QStringLiteral("DICOM Viewer"), error);
        updateControls();
        return false;
    }
    pathLabel_->setText(currentPath_);
    pathLabel_->setToolTip(currentPath_);
    renderCurrent(true);
    return true;
}

bool DicomViewerDialog::openFolder(const QString &path)
{
    QStringList errors;
    QVector<DicomSeries> found = DicomSeriesScanner::scanFolder(path, &errors);
    if (found.isEmpty())
    {
        QMessageBox::warning(this, QStringLiteral("DICOM Folder"),
                             QStringLiteral("No DICOM image series were found in:\n%1").arg(path));
        return false;
    }

    currentPath_ = QFileInfo(path).absoluteFilePath();
    series_ = std::move(found);
    pathLabel_->setText(currentPath_);
    pathLabel_->setToolTip(currentPath_);
    {
        const QSignalBlocker block(seriesCombo_);
        seriesCombo_->clear();
        for (const DicomSeries &series : series_)
            seriesCombo_->addItem(series.displayName());
        seriesCombo_->setCurrentIndex(0);
    }
    selectSeries(0);
    return true;
}

void DicomViewerDialog::chooseFile()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open DICOM File"), currentPath_,
        QStringLiteral("DICOM Files (*.dcm *.dicom);;All Files (*.*)"));
    if (!path.isEmpty())
        openFile(path);
}

void DicomViewerDialog::chooseFolder()
{
    const QString path = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Open DICOM Folder"), currentPath_);
    if (!path.isEmpty())
        openFolder(path);
}

void DicomViewerDialog::selectSeries(int index)
{
    if (index < 0 || index >= series_.size())
        return;
    currentSeriesIndex_ = index;
    const int count = series_.at(index).instances.size();
    {
        const QSignalBlocker block(sliceSpin_);
        sliceSpin_->setRange(1, qMax(1, count));
        sliceSpin_->setValue(1);
    }
    selectSlice(1);
}

void DicomViewerDialog::selectSlice(int oneBasedSlice)
{
    if (currentSeriesIndex_ < 0 || currentSeriesIndex_ >= series_.size())
        return;
    const auto &instances = series_.at(currentSeriesIndex_).instances;
    const int index = oneBasedSlice - 1;
    if (index < 0 || index >= instances.size())
        return;

    QString error;
    if (!frame_.load(instances.at(index).path, &error))
    {
        QMessageBox::warning(this, QStringLiteral("DICOM Slice"), error);
        return;
    }
    renderCurrent(true);
}

void DicomViewerDialog::selectFrame(int oneBasedFrame)
{
    if (!frame_.setFrameIndex(oneBasedFrame - 1))
        return;
    renderCurrent(true);
}

void DicomViewerDialog::applyWindow()
{
    if (!frame_.isValid() || !frame_.isMonochrome())
        return;
    const QImage image = frame_.makeDisplayImage(
        windowCenterSpin_->value(), windowWidthSpin_->value());
    if (image.isNull())
    {
        QMessageBox::warning(this, QStringLiteral("DICOM Window"),
                             QStringLiteral("Failed to apply Window Center / Width."));
        return;
    }
    viewer_->updateDicomDisplay(image);
}

void DicomViewerDialog::renderCurrent(bool resetView)
{
    Q_UNUSED(resetView);
    if (!frame_.isValid())
        return;

    {
        const QSignalBlocker block(frameSpin_);
        frameSpin_->setRange(1, qMax(1, frame_.frameCount));
        frameSpin_->setValue(frame_.frameIndex() + 1);
    }
    if (frame_.hasWindow)
    {
        const QSignalBlocker centerBlock(windowCenterSpin_);
        const QSignalBlocker widthBlock(windowWidthSpin_);
        windowCenterSpin_->setValue(frame_.windowCenter);
        windowWidthSpin_->setValue(frame_.windowWidth);
    }

    QImage display;
    if (frame_.hasNativeYbrFull422Frame())
    {
        display = QImage(frame_.width, frame_.height, QImage::Format_RGBA64);
        display.fill(Qt::black);
    }
    else
    {
        display = frame_.makeDisplayImage();
    }
    if (display.isNull())
    {
        QMessageBox::warning(this, QStringLiteral("DICOM Viewer"),
                             QStringLiteral("Failed to render the selected image."));
        return;
    }

    viewer_->setDicomFrame(&frame_, display);
    pixelLabel_->setText(
        QStringLiteral("%1 × %2 | %3 | %4-bit stored | Frame %5/%6")
            .arg(frame_.width).arg(frame_.height)
            .arg(frame_.photometricInterpretation)
            .arg(frame_.bitsStored)
            .arg(frame_.frameIndex() + 1).arg(frame_.frameCount));
    updateControls();
}

void DicomViewerDialog::updateControls()
{
    const bool valid = frame_.isValid();
    seriesCombo_->setEnabled(!series_.isEmpty());
    sliceSpin_->setEnabled(currentSeriesIndex_ >= 0 &&
                           series_.at(currentSeriesIndex_).instances.size() > 1);
    const bool multipleFrames = valid && frame_.frameCount > 1;
    frameLabel_->setVisible(multipleFrames);
    frameSpin_->setVisible(multipleFrames);
    frameSpin_->setEnabled(multipleFrames);
    const bool windowing = !valid || frame_.isMonochrome();
    windowCenterSpin_->setEnabled(windowing);
    windowWidthSpin_->setEnabled(windowing);
    applyWindowButton_->setEnabled(valid && frame_.isMonochrome());
}

void DicomViewerDialog::wheelEvent(QWheelEvent *event)
{
    if (sliceSpin_->isEnabled() && event->modifiers() == Qt::NoModifier)
    {
        const int step = event->angleDelta().y() > 0 ? -1 : 1;
        sliceSpin_->setValue(sliceSpin_->value() + step);
        event->accept();
        return;
    }
    QDialog::wheelEvent(event);
}
