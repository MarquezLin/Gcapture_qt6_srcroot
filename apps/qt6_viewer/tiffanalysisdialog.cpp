#include "tiffanalysisdialog.h"

#include <QDialogButtonBox>
#include <QPlainTextEdit>
#include <QVBoxLayout>

TiffAnalysisDialog::TiffAnalysisDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("TIFF 10-bit Analyzer"));
    resize(760, 520);

    auto *layout = new QVBoxLayout(this);
    text_ = new QPlainTextEdit(this);
    text_->setReadOnly(true);
    text_->setLineWrapMode(QPlainTextEdit::NoWrap);
    layout->addWidget(text_);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void TiffAnalysisDialog::setReport(const TiffBitDepthReport &report)
{
    if (text_)
        text_->setPlainText(TiffAnalyzer::formatReportText(report));
}
