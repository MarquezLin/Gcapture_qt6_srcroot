#include "previewwindow.h"
#include "ui_previewwindow.h"

#include <QGuiApplication>
#include <QScreen>
#include <QSize>
#include <QRect>

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

QSize previewwindow::resizeToSourceContent(int sourceWidth, int sourceHeight)
{
    if (sourceWidth <= 0 || sourceHeight <= 0 || !ui || !ui->previewHost)
        return size();

    QSize targetContent(sourceWidth, sourceHeight);

    // Avoid creating a window larger than the usable desktop area.
    // 4K or larger sources are scaled down for the initial open only; users can
    // still resize the preview window manually afterwards.
    QScreen *s = screen();
    if (!s)
        s = QGuiApplication::primaryScreen();
    if (s)
    {
        const QRect avail = s->availableGeometry();
        const QSize maxContent(qMax(320, int(avail.width() * 0.85)),
                               qMax(240, int(avail.height() * 0.85)));
        if (targetContent.width() > maxContent.width() || targetContent.height() > maxContent.height())
            targetContent.scale(maxContent, Qt::KeepAspectRatio);
    }

    const QSize currentHost = ui->previewHost->size().isValid() ? ui->previewHost->size() : QSize(1, 1);
    QSize targetWindow = size() + (targetContent - currentHost);
    targetWindow.setWidth(qMax(320, targetWindow.width()));
    targetWindow.setHeight(qMax(240, targetWindow.height()));

    resize(targetWindow);
    return targetContent;
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
