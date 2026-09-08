#ifndef PREVIEWWINDOW_H
#define PREVIEWWINDOW_H

#include <QWidget>
#include <QCloseEvent>
#include <QImage>
#include <QSize>

class QLabel;

namespace Ui
{
    class previewwindow;
}

class previewwindow : public QWidget
{
    Q_OBJECT

public:
    explicit previewwindow(QWidget *parent = nullptr);
    ~previewwindow();
    void setFrame(const QImage &img);
    void setImportedFrame(const QImage &img);
    void clearFrame();

    // Resize the preview window once so the native preview host roughly matches
    // the actual source frame size. After that, users can freely resize the
    // window by themselves; MainWindow controls the one-shot behavior.
    QSize resizeToSourceContent(int sourceWidth, int sourceHeight);

    void *previewHwnd() const;

signals:
    void doubleClicked();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void updateImportedPixmap();

    Ui::previewwindow *ui;
    QLabel *importedImageLabel_ = nullptr;
    QImage importedImage_;
};

#endif // PREVIEWWINDOW_H
