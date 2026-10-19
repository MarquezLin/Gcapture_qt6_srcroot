#include "osddialog.h"

#include "gvfg/gvfg_source.h"

#include <QColorDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSpinBox>
#include <QVBoxLayout>

#include <limits>

OsdDialog::OsdDialog(GvfgSource *source, QWidget *parent)
    : QDialog(parent), source_(source)
{
    setObjectName(QStringLiteral("gvfgOsdTool"));
    setWindowTitle(tr("GVFG OSD Tool"));
    setAttribute(Qt::WA_DeleteOnClose, false);

    text_ = new QLineEdit(QStringLiteral("GVFG OSD"), this);
    text_->setMaxLength(static_cast<int>(GVFG_OSD_MAX_TEXT_CHARACTERS));
    text_->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("[ -~]{1,%1}")
                               .arg(GVFG_OSD_MAX_TEXT_CHARACTERS)), text_));

    x_ = new QSpinBox(this);
    y_ = new QSpinBox(this);
    for (QSpinBox *position : {x_, y_})
        position->setRange(0, (std::numeric_limits<int>::max)());
    x_->setValue(32);
    y_->setValue(32);

    fontSize_ = new QSpinBox(this);
    fontSize_->setRange(1, 512);
    fontSize_->setValue(48);

    colorButton_ = new QPushButton(this);
    updateColorButton();
    connect(colorButton_, &QPushButton::clicked, this, &OsdDialog::chooseColor);

    auto *form = new QFormLayout;
    form->addRow(tr("English text"), text_);
    form->addRow(tr("X"), x_);
    form->addRow(tr("Y"), y_);
    form->addRow(tr("Font size (px)"), fontSize_);
    form->addRow(tr("Color"), colorButton_);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    applyButton_ = buttons->addButton(tr("Apply OSD"), QDialogButtonBox::ApplyRole);
    auto *clearButton = buttons->addButton(tr("Clear OSD"), QDialogButtonBox::ResetRole);
    connect(applyButton_, &QPushButton::clicked, this, &OsdDialog::apply);
    connect(clearButton, &QPushButton::clicked, this, &OsdDialog::clear);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::hide);

    boundsStatus_ = new QLabel(this);
    boundsStatus_->setWordWrap(true);
    status_ = new QLabel(tr("Apply OSD before or during capture. Settings are kept across Stop/Start."), this);
    status_->setWordWrap(true);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(boundsStatus_);
    layout->addWidget(status_);
    layout->addWidget(buttons);
    setMinimumWidth(420);

    connect(text_, &QLineEdit::textChanged, this, [this]() { updatePlacement(); });
    connect(fontSize_, &QSpinBox::valueChanged, this, [this]() { updatePlacement(); });
    if (source_)
    {
        connect(source_, &GvfgSource::osdFrameSizeChanged,
                this, [this]() { updatePlacement(); });
        connect(source_, &QObject::destroyed, this, [this]() {
            source_ = nullptr;
            updatePlacement();
        });
    }
    updatePlacement();
}

void OsdDialog::chooseColor()
{
    const QColor initial((colorRgb_ >> 16) & 0xffu,
                         (colorRgb_ >> 8) & 0xffu,
                         colorRgb_ & 0xffu);
    const QColor selected = QColorDialog::getColor(initial, this, tr("OSD Color"));
    if (!selected.isValid())
        return;
    colorRgb_ = (static_cast<unsigned int>(selected.red()) << 16) |
                (static_cast<unsigned int>(selected.green()) << 8) |
                static_cast<unsigned int>(selected.blue());
    updateColorButton();
}

void OsdDialog::apply()
{
    if (!updatePlacement())
        return;
    QString error;
    if (!source_->setOsd(text_->text(), x_->value(), y_->value(),
                         static_cast<uint32_t>(fontSize_->value()), colorRgb_, &error))
    {
        status_->setText(error);
        return;
    }
    status_->setText(source_->isRunning()
                         ? tr("OSD is enabled. Settings will also be applied on the next Start.")
                         : tr("OSD settings are saved and will be applied on Start."));
}

void OsdDialog::clear()
{
    QString error;
    if (!source_ || !source_->clearOsd(&error))
    {
        status_->setText(error.isEmpty() ? tr("GVFG source is unavailable.") : error);
        return;
    }
    status_->setText(tr("OSD is disabled and will remain disabled on the next Start."));
}

void OsdDialog::updateColorButton()
{
    colorButton_->setText(QStringLiteral("#%1").arg(colorRgb_, 6, 16, QLatin1Char('0')).toUpper());
    colorButton_->setStyleSheet(QStringLiteral("background-color: #%1;")
                                    .arg(colorRgb_, 6, 16, QLatin1Char('0')));
}

bool OsdDialog::updatePlacement()
{
    if (!source_)
    {
        boundsStatus_->setText(tr("GVFG source is unavailable."));
        applyButton_->setEnabled(false);
        return false;
    }

    GvfgSource::OsdPlacement placement;
    QString error;
    if (!source_->osdPlacement(text_->text(), static_cast<uint32_t>(fontSize_->value()),
                               &placement, &error))
    {
        boundsStatus_->setText(error.isEmpty() ? tr("Unable to measure OSD text.") : error);
        applyButton_->setEnabled(false);
        return false;
    }

    const QString textSize = tr("Text: %1 x %2 px.")
                                 .arg(placement.textSize.width())
                                 .arg(placement.textSize.height());
    if (!placement.frameKnown())
    {
        x_->setEnabled(true);
        y_->setEnabled(true);
        x_->setRange(0, (std::numeric_limits<int>::max)());
        y_->setRange(0, (std::numeric_limits<int>::max)());
        boundsStatus_->setText(
            textSize + QLatin1Char('\n') +
            tr("Frame size is not yet known. Apply saves X/Y; they will be adjusted when Start determines the frame size."));
        applyButton_->setEnabled(true);
        return true;
    }

    if (!placement.fits())
    {
        // There is no valid placement. Preserve the draft coordinates instead
        // of clamping them to zero while the source keeps its pending values.
        x_->setEnabled(false);
        y_->setEnabled(false);
        boundsStatus_->setText(
            tr("Frame: %1 x %2 px. %3\nText does not fit the frame. Reduce the font size or shorten the text. X/Y are retained until the text fits.")
                .arg(placement.frameSize.width())
                .arg(placement.frameSize.height())
                .arg(textSize));
        applyButton_->setEnabled(false);
        return false;
    }

    x_->setEnabled(true);
    y_->setEnabled(true);
    const int maximumX = placement.frameSize.width() - placement.textSize.width();
    const int maximumY = placement.frameSize.height() - placement.textSize.height();
    x_->setRange(0, maximumX);
    y_->setRange(0, maximumY);
    const QString bounds = tr("Frame: %1 x %2 px. %3\nX: 0..%4; Y: 0..%5.")
                         .arg(placement.frameSize.width())
                         .arg(placement.frameSize.height())
                         .arg(textSize)
                         .arg(maximumX)
                         .arg(maximumY);
    boundsStatus_->setText(bounds);
    applyButton_->setEnabled(true);
    return true;
}
