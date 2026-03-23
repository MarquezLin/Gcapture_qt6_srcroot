#include "previewwindow.h"
#include <QVBoxLayout>
#include "ui_previewwindow.h"

previewwindow::previewwindow(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::previewwindow)
{
    ui->setupUi(this);
    auto *layout = new QVBoxLayout(ui->previewHost);
    layout->setContentsMargins(0,0,0,0);

    auto *w = new d3dpreviewwidget(ui->previewHost);
    layout->addWidget(w);

    previewWidget_ = w;

}

previewwindow::~previewwindow()
{
    delete ui;
}

void* previewwindow::previewHwnd() const
{
    return reinterpret_cast<void*>(previewWidget_->winId());
}
