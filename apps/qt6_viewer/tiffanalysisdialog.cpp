#include "tiffanalysisdialog.h"

#include <QDialogButtonBox>
#include <QPlainTextEdit>
#include <QVBoxLayout>
#include <QLabel>
#include <QGroupBox>
#include <QSplitter>
#include "tiffpreviewwidget.h"

TiffAnalysisDialog::TiffAnalysisDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("TIFF Inspector"));
    resize(1100, 700);

    auto *layout = new QVBoxLayout(this);
    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);
    layout->addWidget(splitter, 1);

    auto *viewerBox = new QGroupBox(QStringLiteral("TIFF Viewer"), this);
    auto *viewerLayout = new QVBoxLayout(viewerBox);
    viewer_ = new TiffPreviewWidget(viewerBox);
    viewer_->setMinimumSize(420, 320);
    pixelLabel_ = new QLabel(QStringLiteral("Pixel: move cursor over image"), viewerBox);
    pixelLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pixelLabel_->setWordWrap(true);
    viewerDiagLabel_ = new QLabel(viewerBox);
    viewerDiagLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    viewerDiagLabel_->setWordWrap(true);
    viewerLayout->addWidget(viewer_, 1);
    viewerLayout->addWidget(pixelLabel_);
    viewerLayout->addWidget(viewerDiagLabel_);
    splitter->addWidget(viewerBox);

    auto *reportBox = new QGroupBox(QStringLiteral("TIFF Info"), this);
    auto *reportLayout = new QVBoxLayout(reportBox);
    text_ = new QPlainTextEdit(reportBox);
    text_->setReadOnly(true);
    text_->setLineWrapMode(QPlainTextEdit::NoWrap);
    reportLayout->addWidget(text_);
    splitter->addWidget(reportBox);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({760, 300});

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(viewer_, &TiffPreviewWidget::diagnosticsChanged, this, &TiffAnalysisDialog::refreshViewerDiagnostics);
    connect(viewer_, &TiffPreviewWidget::pixelHoverTextChanged, pixelLabel_, &QLabel::setText);
    layout->addWidget(buttons);

    refreshViewerDiagnostics();
}

void TiffAnalysisDialog::setReport(const TiffBitDepthReport &report)
{
    if (text_)
        text_->setPlainText(TiffAnalyzer::formatReportText(report));
    if (pixelLabel_)
        pixelLabel_->setText(QStringLiteral("Pixel: move cursor over image"));

    if (viewer_)
    {
        viewer_->setSourceFormatInfo(report.photometric, report.samplesPerPixel, report.bitsPerSample);
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
