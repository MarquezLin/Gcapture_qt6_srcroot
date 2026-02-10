#ifndef INPUTINFODIALOG_H
#define INPUTINFODIALOG_H

#include <QDialog>
#include <QString>
#include <vector>

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

signals:
    // Selected WASAPI endpoint id (UTF-8). Empty string means "system default".
    void audioDeviceSelected(const QString &deviceIdUtf8);
    void refreshRequested();

private slots:
    void onAudioDeviceChanged(int index);
    void onRefreshClicked();

private:
    Ui::inputinfodialog *ui;
    std::vector<gcap_audio_device_t> audioDevices_;
    QString currentAudioDeviceId_;
    bool initializing_ = false;
    QString currentAudioDeviceIdUtf8_;
    void refreshAudioDevices(bool keepSelection);
};

#endif // INPUTINFODIALOG_H
