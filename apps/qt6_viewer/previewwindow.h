#ifndef PREVIEWWINDOW_H
#define PREVIEWWINDOW_H

#include <QWidget>
#include "d3dpreviewwidget.h"

namespace Ui {
class previewwindow;
}

class d3dpreviewwidget;

class previewwindow : public QWidget
{
    Q_OBJECT

public:
    explicit previewwindow(QWidget *parent = nullptr);
    ~previewwindow();

    void* previewHwnd() const;

private:
    Ui::previewwindow *ui;
    d3dpreviewwidget *previewWidget_ = nullptr;
};

#endif // PREVIEWWINDOW_H
