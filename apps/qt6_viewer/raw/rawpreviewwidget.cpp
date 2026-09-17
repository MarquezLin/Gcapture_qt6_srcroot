#include "rawpreviewwidget.h"

#include "gvfg_preview.h"

#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QWheelEvent>
#include <cmath>

RawPreviewWidget::RawPreviewWidget(QWidget *parent) : QWidget(parent)
{
    setMouseTracking(true);
    setMinimumSize(480, 360);
    setFocusPolicy(Qt::StrongFocus);

    d3dSurface_ = new QWidget(this);
    d3dSurface_->setAttribute(Qt::WA_NativeWindow);
    d3dSurface_->setAttribute(Qt::WA_TransparentForMouseEvents);
    d3dSurface_->setAutoFillBackground(false);
    d3dSurface_->hide();

    gvfg_preview_handle handle = nullptr;
    if (gvfg_preview_create(&handle) == GVFG_PREVIEW_OK &&
        gvfg_preview_attach_window(handle, reinterpret_cast<void *>(d3dSurface_->winId())) == GVFG_PREVIEW_OK)
    {
        previewHandle_ = handle;
        d3dReady_ = true;
    }
    else if (handle)
    {
        gvfg_preview_destroy(handle);
    }
}

RawPreviewWidget::~RawPreviewWidget()
{
    if (previewHandle_)
    {
        auto handle = static_cast<gvfg_preview_handle>(previewHandle_);
        gvfg_preview_shutdown(handle);
        gvfg_preview_destroy(handle);
    }
}

void RawPreviewWidget::setFrame(
    const RawFrame *frame)
{
    frame_ = frame;
    dicomFrame_ = nullptr;

    preview_ =
        frame_
            ? frame_->makePreview()
            : QImage();

    resetView();
}

void RawPreviewWidget::setDicomFrame(
    const DicomFrame *frame,
    const QImage &displayImage)
{
    frame_ = nullptr;
    dicomFrame_ = frame;

    preview_ = displayImage;

    resetView();
    updateD3dPreview();
}

void RawPreviewWidget::updateDicomDisplay(
    const QImage &displayImage)
{
    if (!dicomFrame_)
        return;

    preview_ = displayImage;
    updateD3dPreview();
    update();
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
    updateD3dSurfaceGeometry();
    update();
}

void RawPreviewWidget::updateD3dPreview()
{
    const bool gray16 = preview_.format() == QImage::Format_Grayscale16;
    const bool rgba16 = preview_.format() == QImage::Format_RGBA64;
    if (!d3dReady_ || !previewHandle_ || !dicomFrame_ || preview_.isNull() ||
        (!gray16 && !rgba16))
    {
        if (d3dSurface_)
            d3dSurface_->hide();
        return;
    }

    // Size the HWND before the worker creates/resizes its swapchain for this
    // frame; otherwise the first present can use the child's default size.
    updateD3dSurfaceGeometry();
    auto handle = static_cast<gvfg_preview_handle>(previewHandle_);
    if (gvfg_preview_wait_idle(handle, 1000) != GVFG_PREVIEW_OK ||
        gvfg_preview_prepare(handle, preview_.width(), preview_.height(), 16) != GVFG_PREVIEW_OK)
    {
        d3dSurface_->hide();
        return;
    }

    gvfg_preview_frame_t frame{};
    frame.data = preview_.constBits();
    frame.data_size = static_cast<uint64_t>(preview_.bytesPerLine()) *
                      static_cast<uint64_t>(preview_.height());
    frame.width = preview_.width();
    frame.height = preview_.height();
    frame.pixel_format = gray16
                             ? GVFG_PREVIEW_PIXFMT_GRAY16
                             : GVFG_PREVIEW_PIXFMT_RGBA16;
    frame.bit_depth = 16;
    frame.row_bytes = preview_.bytesPerLine();

    if (gvfg_preview_render_frame(handle, &frame) != GVFG_PREVIEW_OK)
    {
        d3dSurface_->hide();
        return;
    }

}

void RawPreviewWidget::updateD3dSurfaceGeometry()
{
    if (!d3dSurface_)
        return;

    // A native child covers its parent's QPainter output. Keep the existing
    // high-zoom grid/value renderer available once pixel inspection begins.
    const bool useD3d = d3dReady_ && dicomFrame_ && !preview_.isNull() && zoom_ < 10.0;
    if (!useD3d)
    {
        d3dSurface_->hide();
        return;
    }

    const QPointF topLeft = imageTopLeft();
    const QSizeF scaled(preview_.width() * zoom_, preview_.height() * zoom_);
    d3dSurface_->setGeometry(QRectF(topLeft, scaled).toAlignedRect());
    d3dSurface_->show();
    d3dSurface_->raise();
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
    if ((!frame_ && !dicomFrame_) ||
        preview_.isNull())
    {
        return {-1, -1};
    }
    const QPointF local = (pos - imageTopLeft()) / zoom_;
    const int x = int(std::floor(local.x()));
    const int y = int(std::floor(local.y()));
    if (x < 0 ||
        y < 0 ||
        x >= preview_.width() ||
        y >= preview_.height())
    {
        return {-1, -1};
    }
    return {x, y};
}

void RawPreviewWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(20, 20, 20));
    if ((!frame_ && !dicomFrame_) ||
        preview_.isNull())
    {
        return;
    }

    const QPointF topLeft = imageTopLeft();
    p.setRenderHint(QPainter::SmoothPixmapTransform, zoom_ < 1.0);
    if (!d3dSurface_ || !d3dSurface_->isVisible())
        p.drawImage(QRectF(topLeft, QSizeF(preview_.width() * zoom_, preview_.height() * zoom_)), preview_);

    if (zoom_ < 10.0)
        return;

    const int x0 = qBound(0, int(std::floor((-topLeft.x()) / zoom_)), preview_.width() - 1);
    const int y0 = qBound(0, int(std::floor((-topLeft.y()) / zoom_)), preview_.height() - 1);
    const int x1 = qBound(0, int(std::ceil((width() - topLeft.x()) / zoom_)), preview_.width());
    const int y1 = qBound(0, int(std::ceil((height() - topLeft.y()) / zoom_)), preview_.height());

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
            QString text;

            if (frame_)
            {
                text =
                    frame_->cellText(
                        x,
                        y,
                        hexadecimal_);
            }
            else if (dicomFrame_)
            {
                text =
                    dicomFrame_->cellText(
                        x,
                        y,
                        hexadecimal_);
            }

            p.drawText(
                cell.adjusted(2, 2, -2, -2),
                Qt::AlignCenter,
                text);
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
    updateD3dSurfaceGeometry();
    if (zoom_ < 10.0 && dicomFrame_)
        updateD3dPreview();
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
        updateD3dSurfaceGeometry();
        update();
    }
    const QPoint pixel = imagePixelAt(event->position());
    QString text =
        QStringLiteral(
            "Pixel: move cursor over image");

    if (pixel.x() >= 0)
    {
        if (frame_)
        {
            text =
                frame_->pixelText(
                    pixel.x(),
                    pixel.y(),
                    hexadecimal_);
        }
        else if (dicomFrame_)
        {
            text =
                dicomFrame_->pixelText(
                    pixel.x(),
                    pixel.y(),
                    hexadecimal_);
        }
    }

    emit pixelTextChanged(text);
}

void RawPreviewWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateD3dSurfaceGeometry();
}

void RawPreviewWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if ((event->button() == Qt::MiddleButton || event->button() == Qt::LeftButton) && panning_)
    {
        panning_ = false;
        unsetCursor();
    }
}
