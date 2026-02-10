#include "displayinfodialog.h"
#include "ui_displayinfodialog.h"

DisplayInfoDialog::DisplayInfoDialog(QWidget *parent)
    : QDialog(parent), ui(new Ui::DisplayInfoDialog)
{
    ui->setupUi(this);
    setModal(false);
}

DisplayInfoDialog::~DisplayInfoDialog()
{
    delete ui;
}

void DisplayInfoDialog::setInfoText(const QString &text)
{
    // 如果你用的是 QPlainTextEdit
    ui->plainTextInfo->setPlainText(text);

    // 若改用 QLabel，就改成：
    // ui->labelInfo->setText(text);
}