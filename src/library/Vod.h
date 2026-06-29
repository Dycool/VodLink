#pragma once

#include <QDateTime>
#include <QString>

struct AccountProfile
{
    QString email;
    QString displayName;
    QString pictureUrl;
};

struct Vod
{
    qint64 id = 0;
    QString game;
    QString youtubeId;
    QString streamStatus;
    QDateTime startedAt;
    qint64 durationMs = 0;
    // Google account that owns this local YouTube VOD cache entry. Empty for
    // legacy rows and friend VODs. Used to avoid deleting another signed-in
    // account's cached VOD from YouTube.
    QString accountEmail;
    // Empty for local account VODs; the friend's email for VODs received from the Worker.
    QString ownerEmail;
    QString ownerName;
    QString ownerPictureUrl;
    QString title;

    [[nodiscard]] bool isMine() const { return ownerEmail.isEmpty(); }
};

struct VodClip
{
    qint64 id = 0;
    // Parent YouTube VOD/video id.
    QString youtubeId;
    // Real YouTube Clip id and public URL. Empty only for legacy local caches.
    QString clipId;
    QString clipUrl;
    QString title;
    int startSeconds = 0;
    int endSeconds = 0;
    QDateTime createdAt;
};

Q_DECLARE_METATYPE(Vod)
Q_DECLARE_METATYPE(VodClip)
