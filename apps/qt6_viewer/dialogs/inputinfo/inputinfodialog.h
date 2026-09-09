#ifndef INPUTINFODIALOG_H
#define INPUTINFODIALOG_H

#include <QDialog>
#include <QString>
#include <QListWidget>

namespace Ui
{
    class inputinfodialog;
}

class inputinfodialog : public QDialog
{
    Q_OBJECT

public:
    explicit inputinfodialog(QWidget *parent = nullptr);
    ~inputinfodialog();

    void setInfoText(const QString &text);
    void setPropertyPages(const QStringList &pages);

signals:
    void refreshRequested();
    void openPropertyPageRequested(const QString &pageNameUtf8, bool capturePin);

private slots:
    void onRefreshClicked();
    void onOpenSelectedPropertyPage();

private:
    Ui::inputinfodialog *ui;
    QString lastInfoText_;
    static QString propertyPageNameFromDisplay(const QString &display);
    static bool propertyPageIsCapturePin(const QString &display);
};

#endif // INPUTINFODIALOG_H
