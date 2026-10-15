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

OsdDialog::OsdDialog(GvfgSource *source, QWidget *parent)
    : QDialog(parent), source_(source)
{
    setObjectName(QStringLiteral("gvfgOsdTool"));
    setWindowTitle(tr("GVFG OSD Tool"));
    setAttribute(Qt::WA_DeleteOnClose, false);

    text_ = new QLineEdit(QStringLiteral("GVFG OSD"), this);
    text_->setMaxLength(static_cast<int>(GVFG_OSD_MAX_TEXT_CHARACTERS));
    text_->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("[ -~]{1,64}")), text_));

    x_ = new QSpinBox(this);
    y_ = new QSpinBox(this);
    for (QSpinBox *position : {x_, y_})
        position->setRange(-16384, 16384);
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
    auto *applyButton = buttons->addButton(tr("Apply OSD"), QDialogButtonBox::ApplyRole);
    auto *clearButton = buttons->addButton(tr("Clear OSD"), QDialogButtonBox::ResetRole);
    connect(applyButton, &QPushButton::clicked, this, &OsdDialog::apply);
    connect(clearButton, &QPushButton::clicked, this, &OsdDialog::clear);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::hide);

    status_ = new QLabel(tr("Apply OSD before or during capture. Settings are kept across Stop/Start."), this);
    status_->setWordWrap(true);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(status_);
    layout->addWidget(buttons);
    setMinimumWidth(420);
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
    if (!source_ || text_->text().isEmpty())
    {
        status_->setText(tr("Text cannot be empty."));
        return;
    }
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
