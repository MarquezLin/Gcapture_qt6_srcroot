#ifndef TIFFANALYSISDIALOG_H
#define TIFFANALYSISDIALOG_H

#include <QDialog>
#include "tiff_analyzer.h"

class QPlainTextEdit;

class TiffAnalysisDialog : public QDialog
{
    Q_OBJECT
public:
    explicit TiffAnalysisDialog(QWidget *parent = nullptr);
    void setReport(const TiffBitDepthReport &report);

private:
    QPlainTextEdit *text_ = nullptr;
};

#endif // TIFFANALYSISDIALOG_H
