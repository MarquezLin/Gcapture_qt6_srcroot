#include "inputinfodialog.h"
#include "ui_inputinfodialog.h"
#include <QGridLayout>
#include <QGroupBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QListWidget>
#include <QPushButton>
#include <QScrollBar>



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

    connect(ui->btnRefresh, &QPushButton::clicked,
            this, &inputinfodialog::onRefreshClicked);

    if (QListWidget *list = ensurePropertyPageList(ui))
    {
        if (QPushButton *btn = list->parentWidget()->findChild<QPushButton*>(QStringLiteral("btnOpenPropertyPage")))
            connect(btn, &QPushButton::clicked, this, &inputinfodialog::onOpenSelectedPropertyPage);
        connect(list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *) { onOpenSelectedPropertyPage(); });
    }
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

    QStringList existing;
    existing.reserve(list->count());
    for (int i = 0; i < list->count(); ++i)
        existing << list->item(i)->text();

    if (existing == pages)
    {
        if (QPushButton *btn = list->parentWidget()->findChild<QPushButton*>(QStringLiteral("btnOpenPropertyPage")))
            btn->setEnabled(list->count() > 0);
        return;
    }

    const QString currentName = list->currentItem() ? list->currentItem()->data(Qt::UserRole).toString() : QString();
    list->clear();

    int selectRow = -1;
    for (int i = 0; i < pages.size(); ++i)
    {
        const QString &display = pages.at(i);
        auto *item = new QListWidgetItem(display, list);
        const QString pageName = propertyPageNameFromDisplay(display);
        item->setData(Qt::UserRole, pageName);
        item->setData(Qt::UserRole + 1, propertyPageIsCapturePin(display));
        if (!currentName.isEmpty() && pageName == currentName)
            selectRow = i;
    }
    if (selectRow >= 0)
        list->setCurrentRow(selectRow);
    else if (list->count() > 0)
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
    if (!ui || !ui->plainTextInfo)
        return;

    if (lastInfoText_ == text)
        return;

    QPlainTextEdit *edit = ui->plainTextInfo;
    QScrollBar *vbar = edit->verticalScrollBar();
    QScrollBar *hbar = edit->horizontalScrollBar();
    const int v = vbar ? vbar->value() : 0;
    const int h = hbar ? hbar->value() : 0;

    edit->setPlainText(text);

    if (vbar)
        vbar->setValue(v);
    if (hbar)
        hbar->setValue(h);

    lastInfoText_ = text;
}

void inputinfodialog::onRefreshClicked()
{
    emit refreshRequested();
}
