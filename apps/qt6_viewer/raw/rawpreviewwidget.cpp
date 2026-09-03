#include "rawpreviewwidget.h"

#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <cmath>

RawPreviewWidget::RawPreviewWidget(QWidget *parent) : QWidget(parent)
{
    setMouseTracking(true);
    setMinimumSize(480, 360);
    setFocusPolicy(Qt::StrongFocus);
}

void RawPreviewWidget::setFrame(const RawFrame *frame)
{
    frame_ = frame;
    preview_ = frame_ ? frame_->makePreview() : QImage();
    resetView();
}

void RawPreviewWidget::setHexadecimal(bool enabled)
{
    hexadecimal_ = enabled;
    update();
}

void RawPreviewWidget::resetView()
{
    zoom_ = 1.0;
    pan_ = {};
    emit zoomChanged(zoom_);
    update();
}

QPointF RawPreviewWidget::imageTopLeft() const
{
    if (preview_.isNull())
        return {};
    const QSizeF scaled(preview_.width() * zoom_, preview_.height() * zoom_);
    return QPointF((width() - scaled.width()) / 2.0, (height() - scaled.height()) / 2.0) + pan_;
}

QPoint RawPreviewWidget::imagePixelAt(const QPointF &pos) const
{
    if (!frame_ || preview_.isNull())
        return {-1, -1};
    const QPointF local = (pos - imageTopLeft()) / zoom_;
    const int x = int(std::floor(local.x()));
    const int y = int(std::floor(local.y()));
    if (x < 0 || y < 0 || x >= frame_->width || y >= frame_->height)
        return {-1, -1};
    return {x, y};
}

void RawPreviewWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(20, 20, 20));
    if (!frame_ || preview_.isNull())
        return;

    const QPointF topLeft = imageTopLeft();
    p.setRenderHint(QPainter::SmoothPixmapTransform, zoom_ < 1.0);
    p.drawImage(QRectF(topLeft, QSizeF(preview_.width() * zoom_, preview_.height() * zoom_)), preview_);

    if (zoom_ < 10.0)
        return;

    const int x0 = qBound(0, int(std::floor((-topLeft.x()) / zoom_)), frame_->width - 1);
    const int y0 = qBound(0, int(std::floor((-topLeft.y()) / zoom_)), frame_->height - 1);
    const int x1 = qBound(0, int(std::ceil((width() - topLeft.x()) / zoom_)), frame_->width);
    const int y1 = qBound(0, int(std::ceil((height() - topLeft.y()) / zoom_)), frame_->height);

    p.setPen(QPen(QColor(255, 255, 255, 90), 0));
    for (int x = x0; x <= x1; ++x)
        p.drawLine(QPointF(topLeft.x() + x * zoom_, topLeft.y() + y0 * zoom_),
                   QPointF(topLeft.x() + x * zoom_, topLeft.y() + y1 * zoom_));
    for (int y = y0; y <= y1; ++y)
        p.drawLine(QPointF(topLeft.x() + x0 * zoom_, topLeft.y() + y * zoom_),
                   QPointF(topLeft.x() + x1 * zoom_, topLeft.y() + y * zoom_));

    if (zoom_ < 42.0)
        return;

    QFont font = p.font();
    font.setPixelSize(qBound(8, int(zoom_ / 7), 15));
    p.setFont(font);
    for (int y = y0; y < y1; ++y)
    {
        for (int x = x0; x < x1; ++x)
        {
            const QRectF cell(topLeft.x() + x * zoom_, topLeft.y() + y * zoom_, zoom_, zoom_);
            const QColor color = preview_.pixelColor(x, y);
            p.setPen(color.lightness() < 128 ? Qt::white : Qt::black);
            p.drawText(cell.adjusted(2, 2, -2, -2), Qt::AlignCenter,
                       frame_->cellText(x, y, hexadecimal_));
        }
    }
}

void RawPreviewWidget::wheelEvent(QWheelEvent *event)
{
    if (event->angleDelta().y() == 0)
        return;
    const QPointF anchor = event->position();
    const QPointF oldTopLeft = imageTopLeft();
    const QPointF imagePoint = (anchor - oldTopLeft) / zoom_;
    zoom_ = qBound(0.05, zoom_ * std::pow(1.25, event->angleDelta().y() / 120.0), 256.0);
    const QSizeF scaled(preview_.width() * zoom_, preview_.height() * zoom_);
    const QPointF centered((width() - scaled.width()) / 2.0, (height() - scaled.height()) / 2.0);
    pan_ = anchor - imagePoint * zoom_ - centered;
    emit zoomChanged(zoom_);
    update();
    event->accept();
}

void RawPreviewWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::MiddleButton || event->button() == Qt::LeftButton)
    {
        panning_ = true;
        lastMouse_ = event->position().toPoint();
        setCursor(Qt::ClosedHandCursor);
    }
}

void RawPreviewWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (panning_)
    {
        const QPoint now = event->position().toPoint();
        pan_ += now - lastMouse_;
        lastMouse_ = now;
        update();
    }
    const QPoint pixel = imagePixelAt(event->position());
    emit pixelTextChanged(pixel.x() >= 0 ? frame_->pixelText(pixel.x(), pixel.y(), hexadecimal_)
                                         : QStringLiteral("Pixel: move cursor over image"));
}

void RawPreviewWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if ((event->button() == Qt::MiddleButton || event->button() == Qt::LeftButton) && panning_)
    {
        panning_ = false;
        unsetCursor();
    }
}
