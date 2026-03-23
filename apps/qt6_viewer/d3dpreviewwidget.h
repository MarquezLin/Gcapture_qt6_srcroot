#ifndef D3DPREVIEWWIDGET_H
#define D3DPREVIEWWIDGET_H

#include <QWidget>

class d3dpreviewwidget : public QWidget
{
    Q_OBJECT
public:
    explicit d3dpreviewwidget(QWidget *parent = nullptr);

    WId nativePreviewId() const { return winId(); }

protected:
    QPaintEngine *paintEngine() const override { return nullptr; }
    void paintEvent(QPaintEvent *event) override;

signals:
};

#endif // D3DPREVIEWWIDGET_H
