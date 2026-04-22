#ifndef TIFFANALYSISDIALOG_H
#define TIFFANALYSISDIALOG_H

#include <QDialog>
#include "tiff_analyzer.h"

class QPlainTextEdit;
class QLabel;
class QCheckBox;
class d3dpreviewwidget;

class TiffAnalysisDialog : public QDialog
{
    Q_OBJECT
public:
    explicit TiffAnalysisDialog(QWidget *parent = nullptr);
    void setReport(const TiffBitDepthReport &report);

private slots:
    void refreshViewerDiagnostics();
    void onDitheringToggled(bool checked);

private:
    QPlainTextEdit *text_ = nullptr;
    QLabel *viewerDiagLabel_ = nullptr;
    QCheckBox *ditherCheck_ = nullptr;
    d3dpreviewwidget *viewer_ = nullptr;
};

#endif // TIFFANALYSISDIALOG_H
