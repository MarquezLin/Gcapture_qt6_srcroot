#include "previewwindow.h"
#include "ui_previewwindow.h"

#include <QCloseEvent>

PreviewWindow::PreviewWindow(QWidget *parent)
    : QWidget(parent), ui_(new Ui::PreviewWindow)
{
    ui_->setupUi(this);
    ui_->previewHost->setAttribute(Qt::WA_NativeWindow, true);
    ui_->previewHost->setStyleSheet(QStringLiteral("background: black;"));
}

PreviewWindow::~PreviewWindow()
{
    delete ui_;
}

void *PreviewWindow::nativePreviewHandle() const
{
    return reinterpret_cast<void *>(ui_->previewHost->winId());
}

void PreviewWindow::closePreview()
{
    closeAllowed_ = true;
    close();
}

void PreviewWindow::showPreview()
{
    show();
    raise();
    activateWindow();
}

void PreviewWindow::closeEvent(QCloseEvent *event)
{
    if (closeAllowed_)
    {
        event->accept();
        return;
    }

    hide();
    event->ignore();
}
