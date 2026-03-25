#ifndef D3DPREVIEWWIDGET_H
#define D3DPREVIEWWIDGET_H

#include <QWidget>
#include <QImage>
#include <QMutex>

class d3dpreviewwidget : public QWidget
{
    Q_OBJECT
public:
    explicit d3dpreviewwidget(QWidget *parent = nullptr);

    WId nativePreviewId() const { return winId(); }
    void setFrame(const QImage &img);
    void clearFrame();

protected:
    QPaintEngine *paintEngine() const override { return nullptr; }
    void paintEvent(QPaintEvent *event) override;

private:
    mutable QMutex frameMtx_;
    QImage frame_;
};

#endif // D3DPREVIEWWIDGET_H
