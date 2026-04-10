#include "previewwindow.h"
#include "ui_previewwindow.h"

previewwindow::previewwindow(QWidget *parent)
    : QWidget(parent), ui(new Ui::previewwindow)
{
    ui->setupUi(this);

    setAttribute(Qt::WA_DeleteOnClose, false);
}

previewwindow::~previewwindow()
{
    delete ui;
}

void *previewwindow::previewHwnd() const
{
    return reinterpret_cast<void *>(ui->previewHost->winId());
}

void previewwindow::clearFrame()
{
    if (previewWidget_)
        previewWidget_->clearFrame();
}

void previewwindow::setFrame(const QImage &img)
{
    if (previewWidget_)
        previewWidget_->setFrame(img);
}

void previewwindow::closeEvent(QCloseEvent *event)
{
    event->ignore();
    hide();
}
