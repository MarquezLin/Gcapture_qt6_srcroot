#ifndef RAWINSPECTORDIALOG_H
#define RAWINSPECTORDIALOG_H

#include "rawframe.h"
#include "dicomframe.h"
#include <QDialog>
#include <QStringList>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;
class RawPreviewWidget;
class QDoubleSpinBox;

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
    void chooseFile();
    void navigateFile(int offset);
    void refreshFileNavigation();
    void applyDicomWindow();
    bool isDicomFile(const QString &path) const;
    void updateModeControls();

    QString path_;
    QStringList siblingRawFiles_;
    int currentFileIndex_ = -1;
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
    QPushButton *previousFileButton_ = nullptr;
    QPushButton *nextFileButton_ = nullptr;
    DicomFrame dicomFrame_;
    bool dicomMode_ = false;

    QDoubleSpinBox *windowCenterSpin_ = nullptr;
    QDoubleSpinBox *windowWidthSpin_ = nullptr;
    QPushButton *applyWindowButton_ = nullptr;
};

#endif
