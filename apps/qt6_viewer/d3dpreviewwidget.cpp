#include "d3dpreviewwidget.h"

#include <QPainter>
#include <QMutexLocker>

d3dpreviewwidget::d3dpreviewwidget(QWidget *parent)
    : QWidget{parent}
{
    setAttribute(Qt::WA_NativeWindow, true);
    setAutoFillBackground(false);
}

void d3dpreviewwidget::setFrame(const QImage &img)
{
    {
        QMutexLocker lock(&frameMtx_);
        frame_ = img.copy();
    }
    update();
}

void d3dpreviewwidget::clearFrame()
{
    {
        QMutexLocker lock(&frameMtx_);
        frame_ = QImage();
    }
    update();
}

void d3dpreviewwidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter p(this);
    p.fillRect(rect(), Qt::black);

    QImage img;
    {
        QMutexLocker lock(&frameMtx_);
        img = frame_;
    }

    if (img.isNull())
        return;

    QImage draw = img.format() == QImage::Format_ARGB32 ||
                          img.format() == QImage::Format_RGB32 ||
                          img.format() == QImage::Format_RGB888
                      ? img
                      : img.convertToFormat(QImage::Format_ARGB32);

    QSize scaled = draw.size();
    scaled.scale(size(), Qt::KeepAspectRatio);
    QRect target(QPoint(0, 0), scaled);
    target.moveCenter(rect().center());
    p.drawImage(target, draw);
}
