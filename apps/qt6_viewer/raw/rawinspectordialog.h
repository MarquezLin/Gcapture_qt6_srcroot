#ifndef RAWINSPECTORDIALOG_H
#define RAWINSPECTORDIALOG_H

#include "rawframe.h"
#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QSpinBox;
class RawPreviewWidget;

class RawInspectorDialog : public QDialog
{
    Q_OBJECT
public:
    explicit RawInspectorDialog(QWidget *parent = nullptr);
    bool openFile(const QString &path);

private:
    void applyFormatDefaults();
    void loadCurrent();
    void inferFromFileName(const QString &path);

    QString path_;
    RawFrame frame_;
    RawPreviewWidget *viewer_ = nullptr;
    QLabel *fileLabel_ = nullptr;
    QLabel *pixelLabel_ = nullptr;
    QLabel *zoomLabel_ = nullptr;
    QComboBox *formatCombo_ = nullptr;
    QSpinBox *widthSpin_ = nullptr;
    QSpinBox *heightSpin_ = nullptr;
    QSpinBox *strideSpin_ = nullptr;
    QCheckBox *hexCheck_ = nullptr;
};

#endif
