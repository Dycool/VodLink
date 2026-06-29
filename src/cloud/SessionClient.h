#pragma once

#include "library/Vod.h"

#include <QDateTime>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

// Talks to the VodLink Worker. This is the ONLY networked friend interaction and
// it costs exactly two requests per shared session:
//   announceStart()       -> POST /start   (on game start)
//   requestStopMatches()  -> GET  /stop    (on game stop; returns friends' VODs)
// Friend lists are passed in from the app; the Worker stores nothing permanent.
class SessionClient final : public QObject
{
    Q_OBJECT

public:
    explicit SessionClient(QObject *parent = nullptr);

    void setWorkerUrl(const QString &url);
    void setIdTokenProvider(std::function<QString()> provider);
    [[nodiscard]] bool isReady() const;

    void announceStart(const QString &game, const QString &youtubeId,
                       const QDateTime &startedAt, const QStringList &friendEmails);
    void requestStopMatches();

signals:
    void matchesReceived(const QVector<Vod> &friendVods);
    void requestFailed(const QString &message);

private:
    [[nodiscard]] QString idToken() const;

    QNetworkAccessManager m_network;
    QString m_workerUrl;
    std::function<QString()> m_tokenProvider;
};
