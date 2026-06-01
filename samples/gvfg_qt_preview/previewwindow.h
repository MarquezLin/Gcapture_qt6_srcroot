#pragma once

#include <QWidget>

QT_BEGIN_NAMESPACE
namespace Ui
{
class PreviewWindow;
}
QT_END_NAMESPACE

class PreviewWindow final : public QWidget
{
    Q_OBJECT

public:
    explicit PreviewWindow(QWidget *parent = nullptr);
    ~PreviewWindow() override;

    void *nativePreviewHandle() const;
    void closePreview();

public slots:
    void showPreview();

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    Ui::PreviewWindow *ui_ = nullptr;
    bool closeAllowed_ = false;
};
