#include "PlayerBridge.h"

#include "app/DebugLog.h"

#include <QLocale>

namespace {
QString secondsText(double seconds)
{
    return QLocale::c().toString(seconds, 'f', 3);
}
}

PlayerBridge::PlayerBridge(QObject *parent)
    : QObject(parent)
{
}

void PlayerBridge::loadVideo(const QString &videoId, double seekSeconds)
{
    DebugLog::writeCategory(QStringLiteral("LiteYouTube"),
                            QStringLiteral("C++ loadVideo videoId=%1 seek=%2")
                                .arg(videoId, secondsText(seekSeconds)));
    emit loadRequested(videoId, seekSeconds);
}

void PlayerBridge::seek(double seconds)
{
    DebugLog::writeCategory(QStringLiteral("LiteYouTube"),
                            QStringLiteral("C++ seek seconds=%1").arg(secondsText(seconds)));
    emit seekRequested(seconds);
}

void PlayerBridge::play()
{
    DebugLog::writeCategory(QStringLiteral("LiteYouTube"), QStringLiteral("C++ play"));
    emit playRequested();
}

void PlayerBridge::pause()
{
    DebugLog::writeCategory(QStringLiteral("LiteYouTube"), QStringLiteral("C++ pause"));
    emit pauseRequested();
}

void PlayerBridge::showMessage(const QString &text)
{
    DebugLog::writeCategory(QStringLiteral("LiteYouTube"),
                            QStringLiteral("C++ overlay message=%1").arg(text));
    emit messageRequested(text);
}

void PlayerBridge::cacheVideos(const QStringList &videoIds)
{
    DebugLog::writeCategory(QStringLiteral("LiteYouTube"),
                            QStringLiteral("C++ cacheVideos count=%1 ids=%2")
                                .arg(videoIds.size())
                                .arg(videoIds.join(QLatin1Char(','))));
    emit cacheRequested(videoIds);
}

void PlayerBridge::ready()
{
    DebugLog::writeCategory(QStringLiteral("LiteYouTube"), QStringLiteral("JS bridge ready"));
    m_pageReady = true;
    emit pageReady();
}

void PlayerBridge::onTimeUpdate(double seconds)
{
    m_currentTime = seconds;
    emit timeUpdated(seconds);
}

void PlayerBridge::debugLog(const QString &category, const QString &message)
{
    const QString cleanCategory = category.trimmed().isEmpty()
        ? QStringLiteral("LiteYouTube/JS")
        : QStringLiteral("LiteYouTube/%1").arg(category.trimmed());
    DebugLog::writeCategory(cleanCategory, message);
}
