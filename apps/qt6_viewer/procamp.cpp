#include "procamp.h"
#include "ui_procamp.h"
#include <QSignalBlocker>
#include <QSlider>

ProcAmp::ProcAmp(QWidget *parent)
    : QDialog(parent), ui(new Ui::ProcAmp)
{
    ui->setupUi(this);

    // Range: 0..255, neutral=128
    ui->sldBrightness->setRange(0, 255);
    ui->sldContrast->setRange(0, 255);
    ui->sldHue->setRange(0, 255);
    ui->sldSaturation->setRange(0, 255);
    ui->sldSharpness->setRange(0, 255);

    setValues(gcap_procamp_t{128, 128, 128, 128, 128});

    auto hook = [this](int)
    { updateValueLabels(); emit valuesChanged(currentParams()); };

    connect(ui->sldBrightness, &QSlider::valueChanged, this, hook);
    connect(ui->sldContrast, &QSlider::valueChanged, this, hook);
    connect(ui->sldHue, &QSlider::valueChanged, this, hook);
    connect(ui->sldSaturation, &QSlider::valueChanged, this, hook);
    connect(ui->sldSharpness, &QSlider::valueChanged, this, hook);

    // OK/Cancel: keep default dialog behavior.
    connect(ui->buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(ui->buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

ProcAmp::~ProcAmp()
{
    delete ui;
}

void ProcAmp::setValues(const gcap_procamp_t &p)
{
    const QSignalBlocker b1(ui->sldBrightness);
    const QSignalBlocker b2(ui->sldContrast);
    const QSignalBlocker b3(ui->sldHue);
    const QSignalBlocker b4(ui->sldSaturation);
    const QSignalBlocker b5(ui->sldSharpness);
    ui->sldBrightness->setValue(p.brightness);
    ui->sldContrast->setValue(p.contrast);
    ui->sldHue->setValue(p.hue);
    ui->sldSaturation->setValue(p.saturation);
    ui->sldSharpness->setValue(p.sharpness);
    updateValueLabels();
}

gcap_procamp_t ProcAmp::values() const
{
    gcap_procamp_t p;
    p.brightness = ui->sldBrightness->value();
    p.contrast = ui->sldContrast->value();
    p.hue = ui->sldHue->value();
    p.saturation = ui->sldSaturation->value();
    p.sharpness = ui->sldSharpness->value();
    return p;
}

void ProcAmp::setControlsEnabled(bool en)
{
    ui->sldBrightness->setEnabled(en);
    ui->sldContrast->setEnabled(en);
    ui->sldHue->setEnabled(en);
    ui->sldSaturation->setEnabled(en);
    ui->sldSharpness->setEnabled(en);
    ui->buttonBox->setEnabled(en);
}


static void applyRange(QSlider *slider, const gcap_procamp_range_t &r)
{
    slider->setEnabled(r.supported != 0);
    if (r.supported)
    {
        slider->setRange(r.min_value, r.max_value);
        slider->setSingleStep(r.step > 0 ? r.step : 1);
        slider->setPageStep((r.step > 0 ? r.step : 1) * 8);
        slider->setValue(r.current_value);
    }
}

void ProcAmp::setCaps(const gcap_procamp_caps_t &caps)
{
    const QSignalBlocker b1(ui->sldBrightness);
    const QSignalBlocker b2(ui->sldContrast);
    const QSignalBlocker b3(ui->sldHue);
    const QSignalBlocker b4(ui->sldSaturation);
    const QSignalBlocker b5(ui->sldSharpness);
    applyRange(ui->sldBrightness, caps.brightness);
    applyRange(ui->sldContrast, caps.contrast);
    applyRange(ui->sldHue, caps.hue);
    applyRange(ui->sldSaturation, caps.saturation);
    applyRange(ui->sldSharpness, caps.sharpness);
    updateValueLabels();
}

void ProcAmp::updateValueLabels()
{
    ui->lblBrightnessVal->setText(QString::number(ui->sldBrightness->value()));
    ui->lblContrastVal->setText(QString::number(ui->sldContrast->value()));
    ui->lblHueVal->setText(QString::number(ui->sldHue->value()));
    ui->lblSaturationVal->setText(QString::number(ui->sldSaturation->value()));
    ui->lblSharpnessVal->setText(QString::number(ui->sldSharpness->value()));
}

gcap_procamp_t ProcAmp::currentParams() const
{
    gcap_procamp_t p{};
    p.brightness = ui->sldBrightness->value();
    p.contrast = ui->sldContrast->value();
    p.hue = ui->sldHue->value();
    p.saturation = ui->sldSaturation->value();
    p.sharpness = ui->sldSharpness->value();
    return p;
}

void ProcAmp::setParams(const gcap_procamp_t &p)
{
    setValues(p);
}
