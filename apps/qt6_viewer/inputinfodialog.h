#ifndef INPUTINFODIALOG_H
#define INPUTINFODIALOG_H

#include <QDialog>
#include <QString>
#include <vector>
#include <QListWidget>

struct gcap_audio_device_t;

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

    void setCurrentAudioDevice(const QString &deviceIdUtf8);

    void setInfoText(const QString &text);
    void setPropertyPages(const QStringList &pages);

signals:
    // Selected WASAPI endpoint id (UTF-8). Empty string means "system default".
    void audioDeviceSelected(const QString &deviceIdUtf8);
    void refreshRequested();
    void openPropertyPageRequested(const QString &pageNameUtf8, bool capturePin);

private slots:
    void onAudioDeviceChanged(int index);
    void onRefreshClicked();
    void onOpenSelectedPropertyPage();

private:
    Ui::inputinfodialog *ui;
    std::vector<gcap_audio_device_t> audioDevices_;
    QString currentAudioDeviceId_;
    bool initializing_ = false;
    QString currentAudioDeviceIdUtf8_;
    QString lastInfoText_;
    void refreshAudioDevices(bool keepSelection);
    static QString propertyPageNameFromDisplay(const QString &display);
    static bool propertyPageIsCapturePin(const QString &display);
};

#endif // INPUTINFODIALOG_H
