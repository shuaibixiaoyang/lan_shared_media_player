#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QMutex>
#include <QStandardPaths>
#include <QSurfaceFormat>
#include <QTextStream>

#include "ui/MainWindow.h"

namespace
{
// 保护日志文件写入，避免多线程同时输出时发生内容交叉。
QMutex gLogMutex;

// 将 Qt 运行时日志统一写入应用数据目录，便于排查播放器启动和播放问题。
void fileMessageHandler(QtMsgType type, const QMessageLogContext &, const QString &message)
{
    const QString logDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(logDir);
    QFile logFile(logDir + "/runtime.log");
    if (logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        QTextStream stream(&logFile);
        QString level;
        switch (type) {
        case QtDebugMsg:
            level = "DEBUG";
            break;
        case QtInfoMsg:
            level = "INFO";
            break;
        case QtWarningMsg:
            level = "WARN";
            break;
        case QtCriticalMsg:
            level = "ERROR";
            break;
        case QtFatalMsg:
            level = "FATAL";
            break;
        }

        QMutexLocker locker(&gLogMutex);
        stream << QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz")
               << " [" << level << "] " << message << '\n';
    }
}
}

int main(int argc, char *argv[])
{
    // 程序入口：初始化 OpenGL 格式、Qt 应用、日志系统和主窗口。
    // 如果启动参数里带了媒体文件路径，会在窗口显示后自动打开该文件。
    // 请求兼容性更强的 OpenGL 上下文，尽量避免某些 Windows 环境下只能开窗但不出画面的情况。
    QSurfaceFormat surfaceFormat;
    surfaceFormat.setRenderableType(QSurfaceFormat::OpenGL);
    surfaceFormat.setVersion(2, 1);
    surfaceFormat.setProfile(QSurfaceFormat::NoProfile);
    surfaceFormat.setDepthBufferSize(24);
    surfaceFormat.setStencilBufferSize(8);
    QSurfaceFormat::setDefaultFormat(surfaceFormat);

    QApplication app(argc, argv);
    qInstallMessageHandler(fileMessageHandler);
    QApplication::setApplicationName("Qt FFmpeg OpenGL Player");
    QApplication::setOrganizationName("Codex");
    QApplication::setWindowIcon(QIcon(":/assets/app_icon.svg"));

    MainWindow window;
    window.setWindowIcon(QIcon(":/assets/app_icon.svg"));
    window.show();

    // 允许用户直接把媒体文件路径作为启动参数传给播放器。
    const QStringList arguments = app.arguments();
    if (arguments.size() > 1) {
        window.openMediaFile(arguments.at(1));
    }

    return app.exec();
}
