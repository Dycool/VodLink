#include "SessionClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimeZone>
#include <QUrl>

#include <algorithm>

SessionClient::SessionClient(QObject *parent)
    : QObject(parent)
{
}

void SessionClient::setWorkerUrl(const QString &url)
{
    m_workerUrl = url;
    while (m_workerUrl.endsWith(u'/')) {
        m_workerUrl.chop(1);
    }
}

void SessionClient::setIdTokenProvider(std::function<QString()> provider)
{
    m_tokenProvider = std::move(provider);
}

QString SessionClient::idToken() const
{
    return m_tokenProvider ? m_tokenProvider() : QString();
}

bool SessionClient::isReady() const
{
    return !m_workerUrl.isEmpty() && !idToken().isEmpty();
}

void SessionClient::announceStart(const QString &game, const QString &youtubeId,
                                  const QDateTime &startedAt, const QStringList &friendEmails)
{
    if (!isReady()) {
        return; // sharing disabled or not signed in: stay silent, no requests.
    }

    QJsonArray friends;
    for (const QString &email : friendEmails) {
        friends.append(email.trimmed().toLower());
    }
    const QJsonObject body{
        {QStringLiteral("game"), game},
        {QStringLiteral("youtubeId"), youtubeId},
        {QStringLiteral("startedAt"), static_cast<double>(startedAt.toMSecsSinceEpoch())},
        {QStringLiteral("friends"), friends},
    };

    QNetworkRequest request(QUrl(m_workerUrl + QStringLiteral("/start")));
    request.setTransferTimeout(10000);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization", QByteArray("Bearer ") + idToken().toUtf8());

    QNetworkReply *reply = m_network.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300) {
            emit requestFailed(QStringLiteral("Could not announce session start: %1")
                                   .arg(reply->errorString()));
        }
        reply->deleteLater();
    });
}

void SessionClient::requestStopMatches()
{
    if (!isReady()) {
        return;
    }

    QNetworkRequest request(QUrl(m_workerUrl + QStringLiteral("/stop")));
    request.setTransferTimeout(10000);
    request.setRawHeader("Authorization", QByteArray("Bearer ") + idToken().toUtf8());

    QNetworkReply *reply = m_network.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const QByteArray payload = reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300) {
            emit requestFailed(QStringLiteral("Could not fetch friends' VODs: %1")
                                   .arg(QString::fromUtf8(payload)));
            return;
        }

        const QJsonArray items = QJsonDocument::fromJson(payload)
                                     .object()
                                     .value(QStringLiteral("vods"))
                                     .toArray();
        QVector<Vod> friendVods;
        for (const QJsonValue &value : items) {
            const QJsonObject object = value.toObject();
            const qint64 startedMs = static_cast<qint64>(object.value(QStringLiteral("startedAt")).toDouble());
            const QJsonValue stoppedValue = object.value(QStringLiteral("stoppedAt"));
            const QJsonValue durationValue = object.value(QStringLiteral("durationMs"));

            Vod vod;
            vod.ownerEmail = object.value(QStringLiteral("email")).toString();
            vod.ownerName = object.value(QStringLiteral("name")).toString();
            vod.ownerPictureUrl = object.value(QStringLiteral("picture")).toString();
            vod.youtubeId = object.value(QStringLiteral("youtubeId")).toString();
            vod.game = object.value(QStringLiteral("game")).toString();
            vod.streamStatus = QStringLiteral("shared");
            vod.startedAt = QDateTime::fromMSecsSinceEpoch(startedMs, QTimeZone(QTimeZone::UTC));
            if (durationValue.isDouble()) {
                vod.durationMs = std::max<qint64>(0, static_cast<qint64>(durationValue.toDouble()));
            } else if (stoppedValue.isDouble()) {
                vod.durationMs = std::max<qint64>(0, static_cast<qint64>(stoppedValue.toDouble()) - startedMs);
            }
            if (!vod.youtubeId.isEmpty() && !vod.ownerEmail.isEmpty()) {
                friendVods.push_back(vod);
            }
        }
        emit matchesReceived(friendVods);
    });
}
