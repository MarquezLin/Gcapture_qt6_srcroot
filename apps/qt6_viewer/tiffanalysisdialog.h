#ifndef TIFFANALYSISDIALOG_H
#define TIFFANALYSISDIALOG_H

#include <QDialog>
#include "tiff_analyzer.h"

class QPlainTextEdit;
class QLabel;
class d3dpreviewwidget;

class TiffAnalysisDialog : public QDialog
{
    Q_OBJECT
public:
    explicit TiffAnalysisDialog(QWidget *parent = nullptr);
    void setReport(const TiffBitDepthReport &report);

private slots:
    void refreshViewerDiagnostics();

private:
    QPlainTextEdit *text_ = nullptr;
    QLabel *viewerDiagLabel_ = nullptr;
    d3dpreviewwidget *viewer_ = nullptr;
};

#endif // TIFFANALYSISDIALOG_H
