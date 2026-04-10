#include "tiffanalysisdialog.h"

#include <QDialogButtonBox>
#include <QPlainTextEdit>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QGroupBox>
#include "d3dpreviewwidget.h"

TiffAnalysisDialog::TiffAnalysisDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("TIFF 10-bit Analyzer"));
    resize(1100, 700);

    auto *layout = new QVBoxLayout(this);
    auto *top = new QHBoxLayout();
    layout->addLayout(top, 1);

    auto *viewerBox = new QGroupBox(QStringLiteral("10-bit Capable Viewer"), this);
    auto *viewerLayout = new QVBoxLayout(viewerBox);
    viewer_ = new d3dpreviewwidget(viewerBox);
    viewer_->setMinimumSize(420, 320);
    viewerDiagLabel_ = new QLabel(viewerBox);
    viewerDiagLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    viewerDiagLabel_->setWordWrap(true);
    viewerLayout->addWidget(viewer_, 1);
    viewerLayout->addWidget(viewerDiagLabel_);
    top->addWidget(viewerBox, 1);

    auto *reportBox = new QGroupBox(QStringLiteral("Analysis Report"), this);
    auto *reportLayout = new QVBoxLayout(reportBox);
    text_ = new QPlainTextEdit(reportBox);
    text_->setReadOnly(true);
    text_->setLineWrapMode(QPlainTextEdit::NoWrap);
    reportLayout->addWidget(text_);
    top->addWidget(reportBox, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(viewer_, &d3dpreviewwidget::diagnosticsChanged, this, &TiffAnalysisDialog::refreshViewerDiagnostics);
    layout->addWidget(buttons);

    refreshViewerDiagnostics();
}

void TiffAnalysisDialog::setReport(const TiffBitDepthReport &report)
{
    if (text_)
        text_->setPlainText(TiffAnalyzer::formatReportText(report));

    if (viewer_)
    {
        if (report.ok && report.width > 0 && report.height > 0 && !report.previewRgba64.isEmpty())
            viewer_->setFrameRgba64(report.width, report.height, report.previewRgba64, report.previewStrideBytes);
        else
            viewer_->clearFrame();
    }
    refreshViewerDiagnostics();
}

void TiffAnalysisDialog::refreshViewerDiagnostics()
{
    if (viewerDiagLabel_ && viewer_)
        viewerDiagLabel_->setText(viewer_->diagnosticsText());
}
