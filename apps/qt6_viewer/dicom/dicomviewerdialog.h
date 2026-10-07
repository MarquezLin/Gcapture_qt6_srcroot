#ifndef DICOMVIEWERDIALOG_H
#define DICOMVIEWERDIALOG_H

#include "dicomframe.h"
#include "dicomseries.h"

#include <QDialog>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;
class RawPreviewWidget;
template <typename T> class QFutureWatcher;

struct DicomFolderScanResult
{
    QString folderPath;
    QVector<DicomSeries> series;
    QStringList errors;
};

class DicomViewerDialog : public QDialog
{
    Q_OBJECT
public:
    explicit DicomViewerDialog(QWidget *parent = nullptr);
    bool openFile(const QString &path);
    bool openFolder(const QString &path);

protected:
    void wheelEvent(QWheelEvent *event) override;

private:
    void chooseFile();
    void chooseFolder();
    void selectSeries(int index);
    void selectSlice(int oneBasedSlice);
    void selectFrame(int oneBasedFrame);
    void applyWindow();
    void finishFolderScan();
    void renderCurrent();
    void updateControls();

    QString currentPath_;
    QVector<DicomSeries> series_;
    int currentSeriesIndex_ = -1;
    DicomFrame frame_;

    RawPreviewWidget *viewer_ = nullptr;
    QLabel *pathLabel_ = nullptr;
    QLabel *pixelLabel_ = nullptr;
    QLabel *zoomLabel_ = nullptr;
    QComboBox *seriesCombo_ = nullptr;
    QSpinBox *sliceSpin_ = nullptr;
    QLabel *frameLabel_ = nullptr;
    QSpinBox *frameSpin_ = nullptr;
    QDoubleSpinBox *windowCenterSpin_ = nullptr;
    QDoubleSpinBox *windowWidthSpin_ = nullptr;
    QPushButton *applyWindowButton_ = nullptr;
    QPushButton *openFileButton_ = nullptr;
    QPushButton *openFolderButton_ = nullptr;
    QFutureWatcher<DicomFolderScanResult> *folderScanWatcher_ = nullptr;
};

#endif
