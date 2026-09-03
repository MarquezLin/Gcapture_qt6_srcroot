#ifndef RAWPREVIEWWIDGET_H
#define RAWPREVIEWWIDGET_H

#include "rawframe.h"
#include <QWidget>

class RawPreviewWidget : public QWidget
{
    Q_OBJECT
public:
    explicit RawPreviewWidget(QWidget *parent = nullptr);
    void setFrame(const RawFrame *frame);
    void setHexadecimal(bool enabled);
    void resetView();

signals:
    void pixelTextChanged(const QString &text);
    void zoomChanged(double zoom);

protected:
    void paintEvent(QPaintEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;

private:
    QPoint imagePixelAt(const QPointF &widgetPos) const;
    QPointF imageTopLeft() const;

    const RawFrame *frame_ = nullptr;
    QImage preview_;
    double zoom_ = 1.0;
    QPointF pan_;
    bool panning_ = false;
    QPoint lastMouse_;
    bool hexadecimal_ = false;
};

#endif
