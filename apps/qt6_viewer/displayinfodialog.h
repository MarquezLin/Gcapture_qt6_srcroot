#ifndef DISPLAYINFODIALOG_H
#define DISPLAYINFODIALOG_H

#include <QDialog>

namespace Ui
{
    class DisplayInfoDialog;
}

class DisplayInfoDialog : public QDialog
{
    Q_OBJECT

public:
    explicit DisplayInfoDialog(QWidget *parent = nullptr);
    ~DisplayInfoDialog();

    void setInfoText(const QString &text);

private:
    Ui::DisplayInfoDialog *ui;
};

#endif // DISPLAYINFODIALOG_H
