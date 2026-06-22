#include "mainwindow.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QDateTime>
#include <QEvent>
#include <QPalette>
#include <QWidget>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#endif

// ---- App-wide log file (works in release .exe on other machines) ----
static QFile g_logFile;
static QMutex g_logMutex;

class DarkWindowTitleBarFilter final : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
#ifdef _WIN32
        if (event->type() == QEvent::Show)
        {
            if (auto *widget = qobject_cast<QWidget *>(watched); widget && widget->isWindow())
            {
                const BOOL dark = TRUE;
                const HWND hwnd = reinterpret_cast<HWND>(widget->winId());
                constexpr DWORD kImmersiveDarkMode = 20;
                constexpr DWORD kImmersiveDarkModeLegacy = 19;
                if (FAILED(DwmSetWindowAttribute(hwnd, kImmersiveDarkMode, &dark, sizeof(dark))))
                    DwmSetWindowAttribute(hwnd, kImmersiveDarkModeLegacy, &dark, sizeof(dark));
            }
        }
#else
        Q_UNUSED(watched);
        Q_UNUSED(event);
#endif
        return false;
    }
};

static void messageHandler(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    Q_UNUSED(ctx);

    const char *lvl = "INFO";
    switch (type)
    {
    case QtDebugMsg:
        lvl = "DEBUG";
        break;
    case QtInfoMsg:
        lvl = "INFO";
        break;
    case QtWarningMsg:
        lvl = "WARN";
        break;
    case QtCriticalMsg:
        lvl = "CRIT";
        break;
    case QtFatalMsg:
        lvl = "FATAL";
        break;
    }

    const QString ts = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");
    const QString line = QStringLiteral("[%1][%2] %3\n").arg(ts, QString::fromLatin1(lvl), msg);

    QMutexLocker locker(&g_logMutex);
    if (g_logFile.isOpen())
    {
        g_logFile.write(line.toUtf8());
        g_logFile.flush();
    }

    fprintf((type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg) ? stderr : stdout,
            "%s", line.toLocal8Bit().constData());
    fflush((type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg) ? stderr : stdout);
}

static QString initAppLogFile(QApplication &app)
{
    const QString baseDir = QCoreApplication::applicationDirPath() + "/logs";
    QDir().mkpath(baseDir);

    const QString ts = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    const QString path = baseDir + QString("/qt6_viewer_%1.log").arg(ts);

    g_logFile.setFileName(path);
    if (g_logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
    {
        const QString head = QStringLiteral("[%1][INFO] === qt6_viewer start | Qt %2 ===\n")
                                 .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz"))
                                 .arg(QString::fromLatin1(qVersion()));
        g_logFile.write(head.toUtf8());
        g_logFile.flush();
    }

    // Expose path to UI if needed
    app.setProperty("logPath", path);
    return path;
}

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(QStringLiteral("#0b1119")));
    palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#d8e6f3")));
    palette.setColor(QPalette::Base, QColor(QStringLiteral("#080e15")));
    palette.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#0e1822")));
    palette.setColor(QPalette::Text, QColor(QStringLiteral("#d8e6f3")));
    palette.setColor(QPalette::Button, QColor(QStringLiteral("#162536")));
    palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#ecf6ff")));
    palette.setColor(QPalette::Highlight, QColor(QStringLiteral("#176fa7")));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(QStringLiteral("#607487")));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(QStringLiteral("#607487")));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(QStringLiteral("#607487")));
    a.setPalette(palette);

    DarkWindowTitleBarFilter titleBarFilter;
    a.installEventFilter(&titleBarFilter);

    QFile theme(QStringLiteral(":/new/prefix1/styles/gigabyte_dark.qss"));
    if (theme.open(QIODevice::ReadOnly | QIODevice::Text))
        a.setStyleSheet(QString::fromUtf8(theme.readAll()));

    // Create an always-on log file so the packaged .exe can be debugged on other PCs.
    initAppLogFile(a);
    qInstallMessageHandler(messageHandler);

    MainWindow w;
    w.show();
    return a.exec();
}
