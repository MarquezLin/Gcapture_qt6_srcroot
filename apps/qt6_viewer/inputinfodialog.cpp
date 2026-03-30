#include "inputinfodialog.h"
#include "ui_inputinfodialog.h"
#include <gcapture.h>
#include <gcap_audio.h>
#include <QGridLayout>
#include <QGroupBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QDebug>



static QListWidget *ensurePropertyPageList(Ui::inputinfodialog *ui)
{
    if (!ui || !ui->plainTextInfo)
        return nullptr;
    QWidget *parent = ui->plainTextInfo->parentWidget();
    if (!parent)
        return nullptr;
    if (auto existing = parent->findChild<QListWidget*>(QStringLiteral("listPropertyPages")))
        return existing;

    auto *layout = qobject_cast<QGridLayout*>(parent->layout());
    if (!layout)
        return nullptr;

    auto *group = new QGroupBox(QObject::tr("Available Property Pages"), parent);
    group->setObjectName(QStringLiteral("groupPropertyPages"));
    auto *vbox = new QVBoxLayout(group);
    vbox->setContentsMargins(8, 8, 8, 8);
    vbox->setSpacing(6);

    auto *list = new QListWidget(group);
    list->setObjectName(QStringLiteral("listPropertyPages"));
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    vbox->addWidget(list);

    auto *btn = new QPushButton(QObject::tr("Open Selected Page"), group);
    btn->setObjectName(QStringLiteral("btnOpenPropertyPage"));
    vbox->addWidget(btn);

    layout->addWidget(group, 4, 0);
    return list;
}

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

    if (QListWidget *list = ensurePropertyPageList(ui))
    {
        if (QPushButton *btn = list->parentWidget()->findChild<QPushButton*>(QStringLiteral("btnOpenPropertyPage")))
            connect(btn, &QPushButton::clicked, this, &inputinfodialog::onOpenSelectedPropertyPage);
        connect(list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *) { onOpenSelectedPropertyPage(); });
    }

    refreshAudioDevices(true);
}

inputinfodialog::~inputinfodialog()
{
    delete ui;
}

QString inputinfodialog::propertyPageNameFromDisplay(const QString &display)
{
    const QString sep = QStringLiteral(" — ");
    const int pos = display.lastIndexOf(sep);
    if (pos > 0)
        return display.left(pos).trimmed();
    return display.trimmed();
}

bool inputinfodialog::propertyPageIsCapturePin(const QString &display)
{
    return display.contains(QStringLiteral("Capture Pin"), Qt::CaseInsensitive);
}

void inputinfodialog::setPropertyPages(const QStringList &pages)
{
    QListWidget *list = ensurePropertyPageList(ui);
    if (!list)
        return;
    list->clear();
    for (const QString &display : pages)
    {
        auto *item = new QListWidgetItem(display, list);
        item->setData(Qt::UserRole, propertyPageNameFromDisplay(display));
        item->setData(Qt::UserRole + 1, propertyPageIsCapturePin(display));
    }
    if (list->count() > 0)
        list->setCurrentRow(0);
    if (QPushButton *btn = list->parentWidget()->findChild<QPushButton*>(QStringLiteral("btnOpenPropertyPage")))
        btn->setEnabled(list->count() > 0);
}

void inputinfodialog::onOpenSelectedPropertyPage()
{
    QListWidget *list = ensurePropertyPageList(ui);
    if (!list)
        return;
    QListWidgetItem *item = list->currentItem();
    if (!item)
        return;
    emit openPropertyPageRequested(item->data(Qt::UserRole).toString(),
                                   item->data(Qt::UserRole + 1).toBool());
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