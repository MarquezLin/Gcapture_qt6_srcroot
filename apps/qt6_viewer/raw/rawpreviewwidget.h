#ifndef RAWPREVIEWWIDGET_H
#define RAWPREVIEWWIDGET_H

#include "rawframe.h"
#include "dicomframe.h"

#include <QWidget>

class RawPreviewWidget : public QWidget
{
    Q_OBJECT

public:
    explicit RawPreviewWidget(QWidget *parent = nullptr);
    ~RawPreviewWidget() override;

    void setFrame(const RawFrame *frame);

    void setDicomFrame(
        const DicomFrame *frame,
        const QImage &displayImage);

    void updateDicomDisplay(
        const QImage &displayImage);

    void setHexadecimal(bool enabled);
    void resetView();

signals:
    void pixelTextChanged(const QString &text);
    void zoomChanged(double zoom);

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;

private:
    QPoint imagePixelAt(
        const QPointF &widgetPos) const;

    QPointF imageTopLeft() const;

    void updateD3dPreview();
    void updateD3dSurfaceGeometry();

    const RawFrame *frame_ = nullptr;

    const DicomFrame *dicomFrame_ = nullptr;

    QImage preview_;

    QWidget *d3dSurface_ = nullptr;
    void *previewHandle_ = nullptr;
    bool d3dReady_ = false;

    double zoom_ = 1.0;
    QPointF pan_;

    bool panning_ = false;
    QPoint lastMouse_;

    bool hexadecimal_ = false;
};

#endif
