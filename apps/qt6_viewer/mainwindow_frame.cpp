#include "mainwindow.h"

#include "./ui_mainwindow.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QMessageBox>

void MainWindow::onFrameArrived(const QImage &img)
{
    lastFrameImage_ = img;

    // 這條路徑是 callback/QImage 顯示入口：
    // - CaptureSDK frameReady
    // - MainWindow::sigFrame（例如 packet callback / CPU fallback）
    // 真正的 native GPU preview 不一定會經過這裡。
    if (previewWindow_)
        previewWindow_->setFrame(img);

    updateRuntimeStatusUi();
    refreshCaptureInfoFromSdkAndRuntime(true);
    refreshDisplayInfoFromFrame(img);

    if (infoDlg_)
        infoDlg_->setInfoText(lastInfoText_);

    if (DpinfoDlg_)
        refreshDisplayInfoDialog();
}

void MainWindow::onSnapshot()
{
    // 1) 檢查目前有沒有任何一張 frame
    if (lastFrameImage_.isNull())
    {
        QMessageBox::information(this, "Snapshot",
                                 "There is currently no screenshot available (please start capturing first).");
        return;
    }

    // 2) 固定輸出資料夾：<exe>/snapshots
    QString baseDir = QCoreApplication::applicationDirPath() + "/snapshots";
    QDir().mkpath(baseDir);

    // 3) 自動檔名：snapshot_YYYYMMDD_HHMMSS_zzz.png
    QDateTime now = QDateTime::currentDateTime();
    QString ts = now.toString("yyyyMMdd_HHmmss_zzz");
    QString fileName = QString("snapshot_%1.png").arg(ts);
    QString fullPath = baseDir + "/" + fileName;

    // 4) 存成 PNG（無損，適合擷取畫面）
    bool ok = lastFrameImage_.save(fullPath, "PNG");
    if (!ok)
    {
        QMessageBox::warning(this, "Snapshot",
                             QString("Snapshot storage failed:%1").arg(fullPath));
        return;
    }

    // 5) UI 提示（狀態列＋訊息）
    if (ui->statusbar)
    {
        ui->statusbar->showMessage(
            QStringLiteral("Snapshot saved: %1").arg(fileName),
            5000); // 顯示 5 秒
    }

    // 如果你不想跳視窗，可以把這段拿掉
    QMessageBox::information(this, "Snapshot",
                             QString("Snapshot already saved:\n%1").arg(fullPath));
}
