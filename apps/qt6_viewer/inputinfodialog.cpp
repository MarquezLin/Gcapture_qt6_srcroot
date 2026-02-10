#include "inputinfodialog.h"
#include "ui_inputinfodialog.h"
#include <gcapture.h>
#include <gcap_audio.h>

inputinfodialog::inputinfodialog(QWidget *parent)
    : QDialog(parent), ui(new Ui::inputinfodialog)
{
    ui->setupUi(this);
    setModal(false);

    initializing_ = true;

    // // ---- Audio devices (SDK) ----
    // int count = gcap_get_audio_device_count();
    // if (count > 0)
    // {
    //     audioDevices_.resize(count);
    //     int n = gcap_enum_audio_devices(audioDevices_.data(), count);

    //     for (int i = 0; i < n; ++i)
    //     {
    //         ui->comboAudioDevice->addItem(audioDevices_[i].name);
    //     }

    //     connect(ui->comboAudioDevice,
    //             QOverload<int>::of(&QComboBox::currentIndexChanged),
    //             this,
    //             &inputinfodialog::onAudioDeviceChanged);

    //     if (n > 0)
    //         onAudioDeviceChanged(0);
    // }
    // else
    // {
    //     ui->labelAudioInfo->setText(tr("No audio device"));
    // }

    connect(ui->btnRefresh, &QPushButton::clicked,
            this, &inputinfodialog::onRefreshClicked);

    connect(ui->comboAudioDevice,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            &inputinfodialog::onAudioDeviceChanged);

    refreshAudioDevices(true);
}

inputinfodialog::~inputinfodialog()
{
    delete ui;
}

void inputinfodialog::setInfoText(const QString &text)
{
    // 如果你用的是 QPlainTextEdit
    ui->plainTextInfo->setPlainText(text);

    // 若改用 QLabel，就改成：
    // ui->labelInfo->setText(text);
}

void inputinfodialog::onAudioDeviceChanged(int index)
{
    if (initializing_)
        return;
    if (index < 0 || index >= (int)audioDevices_.size())
        return;

    const auto &d = audioDevices_[index];

    QString ch =
        (d.channels == 1) ? "Mono" : (d.channels == 2) ? "Stereo"
                                                       : QString("%1 ch").arg(d.channels);

    QString depth =
        d.is_float ? "32-bit float"
                   : QString("%1-bit").arg(d.bits_per_sample);

    QString text = QString("%1, %2 Hz, %3")
                       .arg(ch)
                       .arg(d.sample_rate)
                       .arg(depth);

    QString id = QString::fromUtf8(d.id);
    if (!currentAudioDeviceIdUtf8_.isEmpty() && id == currentAudioDeviceIdUtf8_)
    {
        ui->labelAudioInfo->setText(text);
        return;
    }

    emit audioDeviceSelected(id);

    qDebug() << "[AudioSelect] emit id=" << QString::fromUtf8(d.id)
             << " name=" << QString::fromUtf8(d.name);

    ui->labelAudioInfo->setText(text);

    initializing_ = false;
}

void inputinfodialog::setCurrentAudioDevice(const QString &deviceIdUtf8)
{
    currentAudioDeviceIdUtf8_ = deviceIdUtf8;
    // 更新顯示：重新掃描一次，但保持 selection 指向 currentAudioDeviceIdUtf8_
    refreshAudioDevices(/*keepSelection=*/true);
}

void inputinfodialog::onRefreshClicked()
{
    // 只刷新列表，不要自動切換
    refreshAudioDevices(/*keepSelection=*/true);
    emit refreshRequested();
}

void inputinfodialog::refreshAudioDevices(bool keepSelection)
{
    if (!ui->comboAudioDevice)
        return;

    initializing_ = true;
    QSignalBlocker blocker(ui->comboAudioDevice);

    QString keepId = keepSelection ? currentAudioDeviceIdUtf8_ : QString();

    audioDevices_.clear();
    ui->comboAudioDevice->clear();

    int count = gcap_get_audio_device_count();
    if (count <= 0)
    {
        ui->labelAudioInfo->setText(tr("No audio device"));
        initializing_ = false;
        return;
    }

    audioDevices_.resize(count);
    int n = gcap_enum_audio_devices(audioDevices_.data(), count);
    if (n <= 0)
    {
        audioDevices_.clear();
        ui->labelAudioInfo->setText(tr("No audio device"));
        initializing_ = false;
        return;
    }

    int selectIndex = 0;
    for (int i = 0; i < n; ++i)
    {
        ui->comboAudioDevice->addItem(QString::fromUtf8(audioDevices_[i].name));

        if (!keepId.isEmpty() && keepId == QString::fromUtf8(audioDevices_[i].id))
            selectIndex = i;
    }

    ui->comboAudioDevice->setCurrentIndex(selectIndex);

    // 注意：這裡只更新 UI 顯示資訊，不 emit
    if (selectIndex >= 0 && selectIndex < (int)audioDevices_.size())
    {
        const auto &d = audioDevices_[selectIndex];

        QString ch = (d.channels == 1) ? "Mono" : (d.channels == 2) ? "Stereo"
                                                                    : QString("%1 ch").arg(d.channels);
        QString depth = d.is_float ? "32-bit float" : QString("%1-bit").arg(d.bits_per_sample);
        QString text = QString("%1, %2 Hz, %3").arg(ch).arg(d.sample_rate).arg(depth);
        ui->labelAudioInfo->setText(text);

        currentAudioDeviceIdUtf8_ = QString::fromUtf8(d.id);
    }

    initializing_ = false;
}