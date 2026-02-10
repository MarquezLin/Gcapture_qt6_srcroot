#include "mainwindow.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QDateTime>

// ---- App-wide log file (works in release .exe on other machines) ----
static QFile g_logFile;
static QMutex g_logMutex;

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

    // Create an always-on log file so the packaged .exe can be debugged on other PCs.
    initAppLogFile(a);
    qInstallMessageHandler(messageHandler);

    MainWindow w;
    w.show();
    return a.exec();
}
