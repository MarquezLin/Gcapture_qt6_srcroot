#include "previewwindow.h"
#include <QVBoxLayout>
#include "ui_previewwindow.h"

previewwindow::previewwindow(QWidget *parent)
    : QWidget(parent), ui(new Ui::previewwindow)
{
    ui->setupUi(this);

    setAttribute(Qt::WA_DeleteOnClose, false);

    auto *layout = new QVBoxLayout(ui->previewHost);
    layout->setContentsMargins(0, 0, 0, 0);

    auto *w = new d3dpreviewwidget(ui->previewHost);
    layout->addWidget(w);

    previewWidget_ = w;
}

previewwindow::~previewwindow()
{
    delete ui;
}

void *previewwindow::previewHwnd() const
{
    return reinterpret_cast<void *>(previewWidget_->winId());
}

void previewwindow::setFrame(const QImage &img)
{
    if (previewWidget_)
        previewWidget_->setFrame(img);
}

void previewwindow::clearFrame()
{
    if (previewWidget_)
        previewWidget_->clearFrame();
}

void previewwindow::closeEvent(QCloseEvent *event)
{
    event->ignore();
    hide();
}