#ifndef PREVIEWWINDOW_H
#define PREVIEWWINDOW_H

#include <QWidget>
#include <QCloseEvent>
#include <QImage>
#include <QSize>
#include "d3dpreviewwidget.h"

namespace Ui
{
    class previewwindow;
}

class d3dpreviewwidget;

class previewwindow : public QWidget
{
    Q_OBJECT

public:
    explicit previewwindow(QWidget *parent = nullptr);
    ~previewwindow();
    void setFrame(const QImage &img);
    void clearFrame();

    // Resize the preview window once so the native preview host roughly matches
    // the actual source frame size. After that, users can freely resize the
    // window by themselves; MainWindow controls the one-shot behavior.
    QSize resizeToSourceContent(int sourceWidth, int sourceHeight);

    void *previewHwnd() const;

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    Ui::previewwindow *ui;
    d3dpreviewwidget *previewWidget_ = nullptr;
};

#endif // PREVIEWWINDOW_H
