#include "procamp.h"
#include "ui_procamp.h"

ProcAmp::ProcAmp(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::ProcAmp)
{
    ui->setupUi(this);
}

ProcAmp::~ProcAmp()
{
    delete ui;
}
