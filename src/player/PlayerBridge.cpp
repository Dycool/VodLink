#include "PlayerBridge.h"

PlayerBridge::PlayerBridge(QObject *parent)
    : QObject(parent)
{
}

void PlayerBridge::loadVideo(const QString &videoId, double seekSeconds)
{
    emit loadRequested(videoId, seekSeconds);
}

void PlayerBridge::seek(double seconds)
{
    emit seekRequested(seconds);
}

void PlayerBridge::play()
{
    emit playRequested();
}

void PlayerBridge::pause()
{
    emit pauseRequested();
}

void PlayerBridge::showMessage(const QString &text)
{
    emit messageRequested(text);
}

void PlayerBridge::cacheVideos(const QStringList &videoIds)
{
    emit cacheRequested(videoIds);
}

void PlayerBridge::ready()
{
    m_pageReady = true;
    emit pageReady();
}

void PlayerBridge::onTimeUpdate(double seconds)
{
    m_currentTime = seconds;
    emit timeUpdated(seconds);
}
