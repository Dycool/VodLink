#include "DebugLog.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QTextStream>

#include <atomic>
#include <cstdlib>

namespace {
QMutex &logMutex()
{
    static QMutex mutex;
    return mutex;
}

QFile &logFile()
{
    static QFile file;
    return file;
}

QString &logPathStorage()
{
    static QString path;
    return path;
}

std::atomic_bool &enabledStorage()
{
    static std::atomic_bool value(false);
    return value;
}

QtMessageHandler &previousHandlerStorage()
{
    static QtMessageHandler handler = nullptr;
    return handler;
}

QString messageTypeName(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:
        return QStringLiteral("debug");
    case QtInfoMsg:
        return QStringLiteral("info");
    case QtWarningMsg:
        return QStringLiteral("warning");
    case QtCriticalMsg:
        return QStringLiteral("critical");
    case QtFatalMsg:
        return QStringLiteral("fatal");
    }
    return QStringLiteral("unknown");
}

QString nowStamp()
{
    return QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
}

void writeLineLocked(const QString &line)
{
    QFile &file = logFile();
    if (!file.isOpen()) {
        return;
    }
    const QByteArray bytes = line.toUtf8();
    file.write(bytes);
    file.write("\n");
    file.flush();
}

void qtMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (DebugLog::enabled()) {
        QString line = QStringLiteral("[%1] [Qt/%2]").arg(nowStamp(), messageTypeName(type));
        if (context.category != nullptr && *context.category != '\0') {
            line += QStringLiteral(" [%1]").arg(QString::fromUtf8(context.category));
        }
        line += QStringLiteral(" %1").arg(message);
        if (context.file != nullptr && *context.file != '\0') {
            line += QStringLiteral(" (%1:%2)").arg(QString::fromUtf8(context.file)).arg(context.line);
        }
        QMutexLocker locker(&logMutex());
        writeLineLocked(line);
    }

    if (previousHandlerStorage() != nullptr) {
        previousHandlerStorage()(type, context, message);
    }

    if (type == QtFatalMsg) {
        std::abort();
    }
}
} // namespace

bool DebugLog::initializeFromArguments(const QStringList &arguments, QString *error)
{
    const bool requested = arguments.contains(QStringLiteral("-debug"), Qt::CaseInsensitive)
        || arguments.contains(QStringLiteral("--debug"), Qt::CaseInsensitive);
    if (!requested) {
        return true;
    }

    QMutexLocker locker(&logMutex());
    if (enabledStorage().load()) {
        return true;
    }

    const QString exeDir = QFileInfo(QCoreApplication::applicationFilePath()).absolutePath();
    const QString path = QDir(exeDir).filePath(QStringLiteral("VodLink-debug.txt"));
    QFile &file = logFile();
    file.setFileName(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (error != nullptr) {
            *error = QStringLiteral("Could not open debug log beside VodLink.exe: %1")
                         .arg(QDir::toNativeSeparators(path));
        }
        return false;
    }

    logPathStorage() = path;
    enabledStorage().store(true);
    qputenv("VODLINK_DEBUG", "1");
    previousHandlerStorage() = qInstallMessageHandler(qtMessageHandler);

    writeLineLocked(QStringLiteral("[%1] [VodLink] debug log started").arg(nowStamp()));
    writeLineLocked(QStringLiteral("[%1] [VodLink] executable=%2")
                        .arg(nowStamp(), QDir::toNativeSeparators(QCoreApplication::applicationFilePath())));
    writeLineLocked(QStringLiteral("[%1] [VodLink] log=%2")
                        .arg(nowStamp(), QDir::toNativeSeparators(path)));
    writeLineLocked(QStringLiteral("[%1] [VodLink] arguments=%2")
                        .arg(nowStamp(), arguments.join(QLatin1Char(' '))));
    return true;
}

void DebugLog::shutdown()
{
    QMutexLocker locker(&logMutex());
    if (!enabledStorage().load()) {
        return;
    }

    writeLineLocked(QStringLiteral("[%1] [VodLink] debug log stopped").arg(nowStamp()));
    qInstallMessageHandler(previousHandlerStorage());
    previousHandlerStorage() = nullptr;
    logFile().close();
    enabledStorage().store(false);
}

bool DebugLog::enabled()
{
    return enabledStorage().load();
}

QString DebugLog::filePath()
{
    QMutexLocker locker(&logMutex());
    return logPathStorage();
}

void DebugLog::write(const QString &message)
{
    if (!enabled()) {
        return;
    }
    QMutexLocker locker(&logMutex());
    writeLineLocked(QStringLiteral("[%1] [VodLink] %2").arg(nowStamp(), message));
}

void DebugLog::write(const char *message)
{
    write(QString::fromUtf8(message == nullptr ? "" : message));
}

void DebugLog::writeCategory(const QString &category, const QString &message)
{
    if (!enabled()) {
        return;
    }
    QMutexLocker locker(&logMutex());
    writeLineLocked(QStringLiteral("[%1] [%2] %3").arg(nowStamp(), category, message));
}
