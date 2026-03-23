#include "d3dpreviewwidget.h"

d3dpreviewwidget::d3dpreviewwidget(QWidget *parent)
    : QWidget{parent}
{
    setAttribute(Qt::WA_NativeWindow, true);
    setAttribute(Qt::WA_PaintOnScreen, true);
    setAutoFillBackground(false);
    setUpdatesEnabled(false);
}

void d3dpreviewwidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
}
