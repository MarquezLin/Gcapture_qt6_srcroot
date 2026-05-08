#include "previewwindow.h"
#include "ui_previewwindow.h"

#include <QGuiApplication>
#include <QScreen>
#include <QSize>
#include <QRect>
#include <QGridLayout>
#include <QLabel>
#include <QPixmap>
#include <QResizeEvent>

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
    if (importedImageLabel_)
    {
        importedImageLabel_->clear();
        importedImageLabel_->hide();
    }
    importedImage_ = QImage();
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
    Q_UNUSED(img);
}

void previewwindow::setImportedFrame(const QImage &img)
{
    if (img.isNull() || !ui || !ui->previewHost)
        return;

    if (!importedImageLabel_)
    {
        importedImageLabel_ = new QLabel(ui->previewHost);
        importedImageLabel_->setAlignment(Qt::AlignCenter);
        importedImageLabel_->setAutoFillBackground(true);

        auto *hostLayout = new QGridLayout(ui->previewHost);
        hostLayout->setContentsMargins(0, 0, 0, 0);
        hostLayout->setSpacing(0);
        hostLayout->addWidget(importedImageLabel_, 0, 0);
    }

    importedImage_ = img;
    updateImportedPixmap();
    importedImageLabel_->show();
    importedImageLabel_->raise();
}

void previewwindow::updateImportedPixmap()
{
    if (!importedImageLabel_ || importedImage_.isNull() || !ui || !ui->previewHost)
        return;

    const QSize targetSize = ui->previewHost->size();
    if (targetSize.width() <= 0 || targetSize.height() <= 0)
        return;

    const QImage scaled = importedImage_.scaled(targetSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    importedImageLabel_->setPixmap(QPixmap::fromImage(scaled));
}

void previewwindow::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateImportedPixmap();
}

void previewwindow::closeEvent(QCloseEvent *event)
{
    event->ignore();
    hide();
}
