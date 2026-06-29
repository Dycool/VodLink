#include "YouTubeLiveClient.h"

#include "auth/GoogleAuth.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QStringList>
#include <QUrlQuery>

#include <algorithm>
#include <memory>

namespace {
constexpr auto kVodLinkStart = "[VodLink]";
constexpr auto kVodLinkEnd = "[/VodLink]";
constexpr int kDefaultStatusPollMs = 3000;
constexpr int kRateLimitedStatusPollMs = 30000;
constexpr int kMaxUploadPagesToSync = 2;
constexpr int kMaxUploadVideosToSync = 100;

QString apiBase()
{
    return QStringLiteral("https://www.googleapis.com/youtube/v3/");
}

QString htmlDecode(QString text)
{
    return text.replace(QStringLiteral("&quot;"), QStringLiteral("\""))
        .replace(QStringLiteral("&#34;"), QStringLiteral("\""))
        .replace(QStringLiteral("&amp;"), QStringLiteral("&"))
        .replace(QStringLiteral("&#39;"), QStringLiteral("'"))
        .replace(QStringLiteral("&lt;"), QStringLiteral("<"))
        .replace(QStringLiteral("&gt;"), QStringLiteral(">"));
}

QString firstCapture(const QString &text, const QString &pattern)
{
    const QRegularExpression re(pattern, QRegularExpression::DotMatchesEverythingOption);
    const auto match = re.match(text);
    return match.hasMatch() ? htmlDecode(match.captured(1)) : QString();
}

int firstIntCapture(const QString &text, const QStringList &patterns, int fallback)
{
    for (const QString &pattern : patterns) {
        const QString value = firstCapture(text, pattern);
        if (value.isEmpty()) {
            continue;
        }
        bool ok = false;
        const int parsed = value.toInt(&ok);
        if (ok) {
            return parsed;
        }
    }
    return fallback;
}

QString clipIdFromUrl(const QUrl &url)
{
    const QString path = url.path();
    const QRegularExpression re(QStringLiteral("/clip/([A-Za-z0-9_-]+)"));
    const auto match = re.match(path);
    if (match.hasMatch()) {
        return match.captured(1);
    }
    return {};
}

QString canonicalClipUrl(const QString &clipId, const QString &fallbackUrl)
{
    if (!clipId.trimmed().isEmpty()) {
        return QStringLiteral("https://www.youtube.com/clip/%1").arg(clipId.trimmed());
    }
    return fallbackUrl.trimmed();
}

qint64 isoDurationMs(const QString &duration)
{
    // Handles the YouTube ISO-8601 subset used by contentDetails.duration, e.g.
    // PT1H02M03S, PT4M10S, PT42S.
    const QRegularExpression re(QStringLiteral("^P(?:\\d+D)?T(?:(\\d+)H)?(?:(\\d+)M)?(?:(\\d+)S)?$"));
    const auto match = re.match(duration.trimmed());
    if (!match.hasMatch()) {
        return 0;
    }
    const int hours = match.captured(1).isEmpty() ? 0 : match.captured(1).toInt();
    const int minutes = match.captured(2).isEmpty() ? 0 : match.captured(2).toInt();
    const int seconds = match.captured(3).isEmpty() ? 0 : match.captured(3).toInt();
    return qint64((hours * 3600) + (minutes * 60) + seconds) * 1000;
}

QJsonObject vodLinkMetadataFromDescription(const QString &description)
{
    const int start = description.indexOf(QString::fromLatin1(kVodLinkStart));
    const int end = description.indexOf(QString::fromLatin1(kVodLinkEnd));
    if (start < 0 || end <= start) {
        return {};
    }

    const int jsonStart = start + int(QString::fromLatin1(kVodLinkStart).size());
    const QString json = description.mid(jsonStart, end - jsonStart).trimmed();
    return QJsonDocument::fromJson(json.toUtf8()).object();
}

QJsonObject clipToJson(const VodClip &clip)
{
    return QJsonObject {
        {QStringLiteral("clipId"), clip.clipId},
        {QStringLiteral("clipUrl"), clip.clipUrl},
        {QStringLiteral("title"), clip.title},
        {QStringLiteral("startSeconds"), clip.startSeconds},
        {QStringLiteral("endSeconds"), clip.endSeconds},
        {QStringLiteral("createdAt"), clip.createdAt.isValid()
             ? clip.createdAt.toUTC().toString(Qt::ISODateWithMs)
             : QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}
    };
}

VodClip clipFromJson(const QJsonObject &object, const QString &youtubeId)
{
    VodClip clip;
    clip.youtubeId = youtubeId;
    clip.clipId = object.value(QStringLiteral("clipId")).toString();
    clip.clipUrl = object.value(QStringLiteral("clipUrl")).toString();
    if (clip.clipUrl.isEmpty()) {
        clip.clipUrl = canonicalClipUrl(clip.clipId, {});
    }
    clip.title = object.value(QStringLiteral("title")).toString();
    clip.startSeconds = object.value(QStringLiteral("startSeconds")).toInt();
    clip.endSeconds = std::max(clip.startSeconds + 1, object.value(QStringLiteral("endSeconds")).toInt());
    clip.createdAt = QDateTime::fromString(object.value(QStringLiteral("createdAt")).toString(), Qt::ISODateWithMs);
    if (!clip.createdAt.isValid()) {
        clip.createdAt = QDateTime::currentDateTimeUtc();
    }
    return clip;
}

QString descriptionWithVodLinkMetadata(QString existingDescription, const Vod &vod,
                                       const QVector<VodClip> &clips)
{
    const int start = existingDescription.indexOf(QString::fromLatin1(kVodLinkStart));
    const int end = existingDescription.indexOf(QString::fromLatin1(kVodLinkEnd));
    if (start >= 0 && end > start) {
        existingDescription.remove(start, end - start + int(QString::fromLatin1(kVodLinkEnd).size()));
        existingDescription = existingDescription.trimmed();
    }
    if (existingDescription.trimmed().isEmpty()) {
        existingDescription = QStringLiteral("Automatically captured by VodLink");
    }

    QJsonArray clipArray;
    for (const VodClip &clip : clips) {
        clipArray.append(clipToJson(clip));
    }

    const QJsonObject meta {
        {QStringLiteral("version"), 1},
        {QStringLiteral("app"), QStringLiteral("VodLink")},
        {QStringLiteral("game"), vod.game},
        {QStringLiteral("youtubeId"), vod.youtubeId},
        {QStringLiteral("startedAt"), vod.startedAt.isValid()
             ? vod.startedAt.toUTC().toString(Qt::ISODateWithMs)
             : QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("durationMs"), double(std::max<qint64>(0, vod.durationMs))},
        {QStringLiteral("clips"), clipArray}
    };

    const QString block = QStringLiteral("%1\n%2\n%3")
                              .arg(QString::fromLatin1(kVodLinkStart),
                                   QString::fromUtf8(QJsonDocument(meta).toJson(QJsonDocument::Compact)),
                                   QString::fromLatin1(kVodLinkEnd));
    return QStringLiteral("%1\n\n%2").arg(existingDescription.trimmed(), block).trimmed();
}

QString gameFromTitle(const QString &title)
{
    const int dash = title.indexOf(QStringLiteral(" — "));
    if (dash > 0) {
        return title.left(dash).trimmed();
    }
    QString clean = title;
    clean.remove(QStringLiteral(" VOD"), Qt::CaseInsensitive);
    return clean.trimmed().isEmpty() ? QStringLiteral("Game") : clean.trimmed();
}

Vod vodFromVideoResource(const QJsonObject &item)
{
    const QString id = item.value(QStringLiteral("id")).toString();
    const QJsonObject snippet = item.value(QStringLiteral("snippet")).toObject();
    const QJsonObject contentDetails = item.value(QStringLiteral("contentDetails")).toObject();
    const QJsonObject status = item.value(QStringLiteral("status")).toObject();
    const QString description = snippet.value(QStringLiteral("description")).toString();
    const QJsonObject meta = vodLinkMetadataFromDescription(description);
    const QString title = snippet.value(QStringLiteral("title")).toString();

    Vod vod;
    vod.youtubeId = id;
    vod.title = title;
    vod.game = meta.value(QStringLiteral("game")).toString();
    if (vod.game.trimmed().isEmpty()) {
        vod.game = gameFromTitle(title);
    }
    vod.streamStatus = status.value(QStringLiteral("uploadStatus")).toString(QStringLiteral("processed"));
    vod.startedAt = QDateTime::fromString(meta.value(QStringLiteral("startedAt")).toString(), Qt::ISODateWithMs);
    if (!vod.startedAt.isValid()) {
        vod.startedAt = QDateTime::fromString(snippet.value(QStringLiteral("publishedAt")).toString(), Qt::ISODate);
    }
    vod.durationMs = qint64(meta.value(QStringLiteral("durationMs")).toDouble());
    if (vod.durationMs <= 0) {
        vod.durationMs = isoDurationMs(contentDetails.value(QStringLiteral("duration")).toString());
    }
    return vod;
}

QVector<VodClip> clipsFromVideoResource(const QJsonObject &item)
{
    const QString id = item.value(QStringLiteral("id")).toString();
    const QString description = item.value(QStringLiteral("snippet")).toObject()
                                    .value(QStringLiteral("description")).toString();
    const QJsonObject meta = vodLinkMetadataFromDescription(description);
    QVector<VodClip> clips;
    const QJsonArray array = meta.value(QStringLiteral("clips")).toArray();
    for (const QJsonValue &value : array) {
        VodClip clip = clipFromJson(value.toObject(), id);
        if (!clip.clipUrl.trimmed().isEmpty() || !clip.clipId.trimmed().isEmpty()) {
            clips.push_back(clip);
        }
    }
    return clips;
}

bool isVodLinkVideo(const QJsonObject &item)
{
    const QJsonObject snippet = item.value(QStringLiteral("snippet")).toObject();
    const QString description = snippet.value(QStringLiteral("description")).toString();
    return description.contains(QString::fromLatin1(kVodLinkStart))
           || description.contains(QStringLiteral("Automatically captured by VodLink"), Qt::CaseInsensitive);
}

bool isRateLimitApiError(const QJsonObject &apiError)
{
    QStringList probes;
    probes << apiError.value(QStringLiteral("message")).toString();
    const QJsonArray errors = apiError.value(QStringLiteral("errors")).toArray();
    for (const QJsonValue &value : errors) {
        const QJsonObject item = value.toObject();
        probes << item.value(QStringLiteral("reason")).toString();
        probes << item.value(QStringLiteral("message")).toString();
    }
    const QString text = probes.join(QLatin1Char(' ')).toLower();
    return text.contains(QStringLiteral("ratelimit"))
        || text.contains(QStringLiteral("rate limit"))
        || text.contains(QStringLiteral("quota"))
        || text.contains(QStringLiteral("too many requests"));
}
} // namespace

static QJsonObject vodLinkSafeVideoStatus()
{
    // YouTube Data API v3 does not expose per-video switches for comments,
    // ratings, live-chat creation, or Clips availability. These are the closest
    // writable video-status defaults VodLink can enforce automatically:
    //   - unlisted: linkable/embeddable, but not searchable or channel-listed;
    //   - embeddable: required by the in-app player;
    //   - publicStatsViewable=false: hides extended public stats/ratings UI;
    //   - selfDeclaredMadeForKids=false: keeps Clips eligible when the channel's
    //     own Clips settings allow them. Marking as made-for-kids would disable
    //     comments, but it also disables Clips, so do not use that as a hack.
    return QJsonObject {
        {QStringLiteral("privacyStatus"), QStringLiteral("unlisted")},
        {QStringLiteral("selfDeclaredMadeForKids"), false},
        {QStringLiteral("embeddable"), true},
        {QStringLiteral("publicStatsViewable"), false}
    };
}

static bool videoStatusMatchesVodLinkDefaults(const QJsonObject &status)
{
    return status.value(QStringLiteral("privacyStatus")).toString() == QStringLiteral("unlisted")
           && status.value(QStringLiteral("embeddable")).toBool(true)
           && !status.value(QStringLiteral("publicStatsViewable")).toBool(true)
           && !status.value(QStringLiteral("selfDeclaredMadeForKids")).toBool(false);
}

YouTubeLiveClient::YouTubeLiveClient(QObject *parent)
    : QObject(parent)
{
    m_statusPollTimer.setInterval(kDefaultStatusPollMs);
    m_statusPollTimer.setTimerType(Qt::CoarseTimer);
    connect(&m_statusPollTimer, &QTimer::timeout, this, [this] {
        pollStreamStatus();
    });
}

void YouTubeLiveClient::setAuth(GoogleAuth *auth)
{
    m_auth = auth;
}

void YouTubeLiveClient::setStreamSettings(const QString &resolution, int fps)
{
    const QString normalized = resolution.trimmed().toLower();
    int width = 0;
    int height = 0;

    const QStringList parts = normalized.split(QLatin1Char('x'));
    if (parts.size() == 2) {
        bool widthOk = false;
        bool heightOk = false;
        width = parts.at(0).trimmed().toInt(&widthOk);
        height = parts.at(1).trimmed().toInt(&heightOk);
        if (!widthOk || !heightOk) {
            width = 0;
            height = 0;
        }
    } else if (normalized.endsWith(QStringLiteral("p"))) {
        bool ok = false;
        height = normalized.left(normalized.size() - 1).toInt(&ok);
        if (!ok) {
            height = 0;
        }
    }

    const bool hasExactSize = width > 0 && height > 0;
    const qint64 aspectDelta = hasExactSize
        ? (static_cast<qint64>(width) * 9) - (static_cast<qint64>(height) * 16)
        : 0;
    const bool standardAspect = hasExactSize && aspectDelta >= -16 && aspectDelta <= 16;

    // YouTube's liveStream.cdn.resolution is a height tier, not an arbitrary
    // WxH string.  For standard 16:9 sizes, advertise the matching tier.  For
    // ultrawide/custom sizes such as 3440x1440, use variable/variable so the
    // ingest is auto-detected from the actual OBS encoder output instead of
    // being squeezed into the nearest 16:9 tier (which can show up as e.g.
    // 2560x1072 in YouTube's player).
    if (hasExactSize && !standardAspect) {
        m_resolution = QStringLiteral("variable");
        m_frameRate = QStringLiteral("variable");
        return;
    }

    if (height >= 2160) {
        m_resolution = QStringLiteral("2160p");
    } else if (height >= 1440) {
        m_resolution = QStringLiteral("1440p");
    } else if (height >= 1080) {
        m_resolution = QStringLiteral("1080p");
    } else if (height >= 720) {
        m_resolution = QStringLiteral("720p");
    } else if (height >= 480) {
        m_resolution = QStringLiteral("480p");
    } else if (height >= 360) {
        m_resolution = QStringLiteral("360p");
    } else {
        m_resolution = QStringLiteral("1080p");
    }

    m_frameRate = fps == 30 ? QStringLiteral("30fps") : QStringLiteral("60fps");
}

bool YouTubeLiveClient::isAuthenticated() const
{
    return m_auth != nullptr && !m_auth->accessToken().isEmpty();
}

void YouTubeLiveClient::startStreamStatusPolling(const QString &broadcastId, const QString &streamId)
{
    Q_UNUSED(broadcastId);
    Q_UNUSED(streamId);

    // Intentionally disabled. Polling liveStreams.status every few seconds burns
    // YouTube Data API quota and can rate-limit the user while the local RTMP
    // encoder is already the signal VodLink needs during recording. Broadcast
    // finalization still performs its own one-shot lifecycle check when stopping.
    m_statusPollTimer.stop();
    m_statusBroadcastId.clear();
    m_statusStreamId.clear();
    m_statusProbeInFlight = false;
}

void YouTubeLiveClient::stopStreamStatusPolling(const QString &broadcastId, const QString &streamId)
{
    const QString trimmedBroadcast = broadcastId.trimmed();
    const QString trimmedStream = streamId.trimmed();
    if (!trimmedBroadcast.isEmpty() && trimmedBroadcast != m_statusBroadcastId) {
        return;
    }
    if (!trimmedStream.isEmpty() && trimmedStream != m_statusStreamId) {
        return;
    }

    m_statusPollTimer.stop();
    m_statusBroadcastId.clear();
    m_statusStreamId.clear();
    m_statusProbeInFlight = false;
}

void YouTubeLiveClient::checkStreamingEligibility(bool retrying)
{
    if (!isAuthenticated()) {
        return;
    }
    QUrl url(apiBase() + QStringLiteral("liveBroadcasts"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("part"), QStringLiteral("id"));
    query.addQueryItem(QStringLiteral("mine"), QStringLiteral("true"));
    query.addQueryItem(QStringLiteral("maxResults"), QStringLiteral("1"));
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setTransferTimeout(15000);
    request.setRawHeader("Authorization",
                         QByteArray("Bearer ") + (m_auth ? m_auth->accessToken().toUtf8() : QByteArray()));
    QNetworkReply *reply = m_network.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, retrying]() {
        const QByteArray payload = reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();

        if (status == 401 && !retrying && m_auth != nullptr && !m_auth->refreshToken().isEmpty()) {
            auto connection = std::make_shared<QMetaObject::Connection>();
            *connection = connect(m_auth, &GoogleAuth::tokensChanged, this, [this, connection]() {
                disconnect(*connection);
                checkStreamingEligibility(true);
            });
            m_auth->refreshNow();
            return;
        }

        if (status >= 200 && status < 300) {
            emit streamingEligible();
            return;
        }

        const QJsonObject apiError = QJsonDocument::fromJson(payload).object()
                                         .value(QStringLiteral("error")).toObject();
        const QJsonArray errors = apiError.value(QStringLiteral("errors")).toArray();
        const QString reason = errors.isEmpty()
                                   ? QString()
                                   : errors.first().toObject().value(QStringLiteral("reason")).toString();
        emit streamingUnavailable(reason, apiError.value(QStringLiteral("message")).toString());
    });
}

void YouTubeLiveClient::prepareBroadcast(const QString &game)
{
    if (!isAuthenticated()) {
        emit failed(QStringLiteral(
            "YouTube is not connected. Sign in with Google to enable streaming."));
        return;
    }

    const QString gameName = game.trimmed().isEmpty()
                                 ? QStringLiteral("Game")
                                 : game.trimmed();
    const QDateTime startedUtc = QDateTime::currentDateTimeUtc();
    const QString startedAt = QDateTime::currentDateTime()
                                  .toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    const QString title = QStringLiteral("%1 — %2").arg(gameName, startedAt);

    Vod vod;
    vod.game = gameName;
    vod.title = title;
    vod.startedAt = startedUtc;
    const QString description = descriptionWithVodLinkMetadata(
        QStringLiteral("Automatically captured by VodLink"), vod, {});

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("part"), QStringLiteral("snippet,status,contentDetails"));
    const QJsonObject body {
        {QStringLiteral("snippet"), QJsonObject {
             {QStringLiteral("title"), title},
             {QStringLiteral("description"), description},
             {QStringLiteral("scheduledStartTime"),
              startedUtc.addSecs(5).toString(Qt::ISODate)}
         }},
        {QStringLiteral("status"), QJsonObject {
             // Unlisted so friends can watch (and the in-app player can embed) the
             // VOD via its link; it stays unsearchable and off your channel.
             {QStringLiteral("privacyStatus"), QStringLiteral("unlisted")},
             {QStringLiteral("selfDeclaredMadeForKids"), false}
         }},
        {QStringLiteral("contentDetails"), QJsonObject {
             {QStringLiteral("enableAutoStart"), true},
             // VodLink stops RTMP gracefully and then completes the broadcast
             // itself.  Leaving YouTube auto-stop enabled can race that path and
             // end the archive before the final flushed packets are indexed.
             {QStringLiteral("enableAutoStop"), false},
             {QStringLiteral("enableDvr"), true},
             {QStringLiteral("recordFromStart"), true},
             {QStringLiteral("enableEmbed"), true},
             // Quality beats latency for VodLink: normal latency keeps the
             // largest viewer resolutions available, while low/ultra-low can
             // restrict smoothness and 4K availability. All linked VODs use the
             // same setting, so sync remains consistent enough for replay.
             {QStringLiteral("latencyPreference"), QStringLiteral("normal")}
         }}
    };
    post(QStringLiteral("liveBroadcasts"), query, body,
         [this, gameName](const QJsonObject &response) {
             const QString id = response.value(QStringLiteral("id")).toString();
             if (id.isEmpty()) {
                 emit failed(QStringLiteral("YouTube did not return a broadcast ID."));
                 return;
             }
             createStream(gameName, id);
         });
}

void YouTubeLiveClient::createStream(const QString &game, const QString &broadcastId)
{
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("part"), QStringLiteral("snippet,cdn,status,contentDetails"));
    const QJsonObject body {
        {QStringLiteral("snippet"), QJsonObject {{QStringLiteral("title"), game}}},
        {QStringLiteral("cdn"), QJsonObject {
             {QStringLiteral("frameRate"), m_frameRate},
             {QStringLiteral("ingestionType"), QStringLiteral("rtmp")},
             {QStringLiteral("resolution"), m_resolution}
         }},
        {QStringLiteral("contentDetails"), QJsonObject {{QStringLiteral("isReusable"), false}}}
    };
    post(QStringLiteral("liveStreams"), query, body,
         [this, broadcastId](const QJsonObject &response) {
             const QString streamId = response.value(QStringLiteral("id")).toString();
             const QJsonObject ingestion = response.value(QStringLiteral("cdn"))
                                               .toObject()
                                               .value(QStringLiteral("ingestionInfo"))
                                               .toObject();
             // Prefer RTMPS when YouTube returns it. RTMP on port 1935 is often
             // blocked/throttled by networks, which leaves the Live Control Room
             // stuck at "waiting for stream" even though the local encoder started.
             QString address = ingestion.value(QStringLiteral("rtmpsIngestionAddress")).toString();
             if (address.trimmed().isEmpty()) {
                 address = ingestion.value(QStringLiteral("ingestionAddress")).toString();
             }
             const QString streamName = ingestion.value(QStringLiteral("streamName")).toString();
             if (streamId.isEmpty() || address.isEmpty() || streamName.isEmpty()) {
                 emit failed(QStringLiteral("YouTube did not return complete RTMP stream details."));
                 return;
             }
             bind(broadcastId, streamId, address, streamName);
         });
}

void YouTubeLiveClient::bind(const QString &broadcastId, const QString &streamId,
                             const QString &ingestionAddress, const QString &streamName)
{
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("id"), broadcastId);
    query.addQueryItem(QStringLiteral("streamId"), streamId);
    query.addQueryItem(QStringLiteral("part"), QStringLiteral("id,status"));
    post(QStringLiteral("liveBroadcasts/bind"), query, {},
         [this, broadcastId, streamId, ingestionAddress, streamName](const QJsonObject &) {
             emit broadcastReady(broadcastId, streamId,
                                 QUrl(QStringLiteral("%1/%2").arg(ingestionAddress, streamName)));
         });
}

void YouTubeLiveClient::pollStreamStatus(bool retrying)
{
    if (m_statusProbeInFlight || !isAuthenticated()
        || m_statusBroadcastId.isEmpty() || m_statusStreamId.isEmpty()) {
        return;
    }

    m_statusProbeInFlight = true;
    const QString broadcastId = m_statusBroadcastId;
    const QString streamId = m_statusStreamId;

    QUrl url(apiBase() + QStringLiteral("liveStreams"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("part"), QStringLiteral("status"));
    query.addQueryItem(QStringLiteral("id"), streamId);
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setTransferTimeout(15000);
    request.setRawHeader("Authorization",
                         QByteArray("Bearer ") + (m_auth ? m_auth->accessToken().toUtf8() : QByteArray()));
    QNetworkReply *reply = m_network.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, broadcastId, streamId, retrying]() {
        const QByteArray payload = reply->readAll();
        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();
        m_statusProbeInFlight = false;

        if (broadcastId != m_statusBroadcastId || streamId != m_statusStreamId) {
            return;
        }

        if (httpStatus == 401 && !retrying && m_auth != nullptr && !m_auth->refreshToken().isEmpty()) {
            auto connection = std::make_shared<QMetaObject::Connection>();
            *connection = connect(m_auth, &GoogleAuth::tokensChanged, this,
                                  [this, connection]() mutable {
                                      disconnect(*connection);
                                      pollStreamStatus(true);
                                  });
            m_auth->refreshNow();
            return;
        }

        const QJsonDocument document = QJsonDocument::fromJson(payload);
        if (httpStatus < 200 || httpStatus >= 300) {
            const QJsonObject apiError = document.object().value(QStringLiteral("error")).toObject();
            const QString message = apiError.value(QStringLiteral("message")).toString();
            if (isRateLimitApiError(apiError) && m_statusPollTimer.interval() != kRateLimitedStatusPollMs) {
                m_statusPollTimer.setInterval(kRateLimitedStatusPollMs);
            }
            emit streamStatusProbeFailed(broadcastId, streamId,
                                         message.isEmpty()
                                             ? QStringLiteral("YouTube stream-status check failed with HTTP %1.").arg(httpStatus)
                                             : message);
            return;
        }
        if (m_statusPollTimer.interval() != kDefaultStatusPollMs) {
            m_statusPollTimer.setInterval(kDefaultStatusPollMs);
        }

        const QJsonArray items = document.object().value(QStringLiteral("items")).toArray();
        if (items.isEmpty()) {
            emit streamStatusChanged(broadcastId, streamId,
                                     QStringLiteral("missing"), QString(),
                                     QStringLiteral("YouTube did not return the live stream resource."));
            return;
        }

        const QJsonObject status = items.first().toObject().value(QStringLiteral("status")).toObject();
        const QString streamStatus = status.value(QStringLiteral("streamStatus")).toString(QStringLiteral("unknown"));
        const QJsonObject health = status.value(QStringLiteral("healthStatus")).toObject();
        const QString healthStatus = health.value(QStringLiteral("status")).toString();
        const QJsonArray issues = health.value(QStringLiteral("configurationIssues")).toArray();
        QStringList issueTexts;
        for (const QJsonValue &value : issues) {
            const QJsonObject issue = value.toObject();
            const QString type = issue.value(QStringLiteral("type")).toString();
            const QString reason = issue.value(QStringLiteral("reason")).toString();
            const QString description = issue.value(QStringLiteral("description")).toString();
            const QString severity = issue.value(QStringLiteral("severity")).toString();
            QString text = !type.isEmpty() ? type : reason;
            if (text.isEmpty()) {
                text = description;
            }
            if (!severity.isEmpty() && !text.isEmpty()) {
                text = QStringLiteral("%1: %2").arg(severity, text);
            }
            if (!text.isEmpty()) {
                issueTexts.push_back(text);
            }
        }

        emit streamStatusChanged(broadcastId, streamId, streamStatus, healthStatus,
                                 issueTexts.join(QStringLiteral("; ")));
    });
}

void YouTubeLiveClient::completeBroadcast(const QString &broadcastId)
{
    const QString trimmed = broadcastId.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }

    // YouTube rejects liveBroadcasts.transition(...complete...) unless the
    // broadcast actually reached testing/live. If RTMP never arrived, the Live
    // Control Room stays at "waiting for stream" and the old code produced an
    // "Invalid transition" error on shutdown. Check the lifecycle first and
    // delete unused scheduled broadcasts instead of trying to complete them.
    QUrlQuery statusQuery;
    statusQuery.addQueryItem(QStringLiteral("part"), QStringLiteral("status"));
    statusQuery.addQueryItem(QStringLiteral("id"), trimmed);
    get(QStringLiteral("liveBroadcasts"), statusQuery, [this, trimmed](const QJsonObject &response) {
        const QJsonArray items = response.value(QStringLiteral("items")).toArray();
        const QString lifeCycleStatus = items.isEmpty()
                                            ? QString()
                                            : items.first().toObject()
                                                  .value(QStringLiteral("status")).toObject()
                                                  .value(QStringLiteral("lifeCycleStatus")).toString();

        if (lifeCycleStatus == QStringLiteral("complete")) {
            emit broadcastCompleted(trimmed);
            return;
        }

        if (lifeCycleStatus == QStringLiteral("testing")
            || lifeCycleStatus == QStringLiteral("live")) {
            QUrlQuery query;
            query.addQueryItem(QStringLiteral("id"), trimmed);
            query.addQueryItem(QStringLiteral("broadcastStatus"), QStringLiteral("complete"));
            query.addQueryItem(QStringLiteral("part"), QStringLiteral("id,status"));
            post(QStringLiteral("liveBroadcasts/transition"), query, {},
                 [this, trimmed](const QJsonObject &) { emit broadcastCompleted(trimmed); });
            return;
        }

        QUrlQuery deleteQuery;
        deleteQuery.addQueryItem(QStringLiteral("id"), trimmed);
        deleteRequest(QStringLiteral("videos"), deleteQuery,
                      [this, trimmed](const QJsonObject &) {
                          emit videoDeleted(trimmed);
                          emit broadcastCompleted(trimmed);
                      },
                      false,
                      [this, trimmed](const QJsonObject &) {
                          emit videoAlreadyGone(trimmed);
                          emit broadcastCompleted(trimmed);
                      });
    });
}

void YouTubeLiveClient::ensureVodEmbeddable(const QString &videoId)
{
    if (!isAuthenticated()) {
        return;
    }
    const QString trimmed = videoId.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("part"), QStringLiteral("status"));
    query.addQueryItem(QStringLiteral("id"), trimmed);
    get(QStringLiteral("videos"), query, [this, trimmed](const QJsonObject &response) {
        const QJsonArray items = response.value(QStringLiteral("items")).toArray();
        if (items.isEmpty()) {
            return;
        }
        updateVideoStatusForEmbedding(trimmed,
                                      items.first().toObject().value(QStringLiteral("status")).toObject());
    });
}

void YouTubeLiveClient::updateVideoStatusForEmbedding(const QString &videoId, const QJsonObject &existingStatus)
{
    const QString trimmed = videoId.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }

    if (videoStatusMatchesVodLinkDefaults(existingStatus)) {
        return;
    }

    // A freshly completed live archive can still be visible to the owner on
    // youtube.com while the anonymous embedded player sees it as private/not
    // embeddable. Re-assert the safe VodLink defaults on the video resource too;
    // liveBroadcast.contentDetails.enableEmbed alone is not reliable enough for
    // the archived VOD. Keep this object to writable status fields only; copying
    // YouTube's read-only status fields (uploadStatus, failureReason, etc.) makes
    // videos.update fail. Comments/live-chat/Clips switches are not writable via
    // Data API v3; publicStatsViewable=false is the supported privacy knob for
    // hiding the extended public stats/ratings surface while preserving Clips.
    const QJsonObject status = vodLinkSafeVideoStatus();

    QUrlQuery updateQuery;
    updateQuery.addQueryItem(QStringLiteral("part"), QStringLiteral("status"));
    const QJsonObject body {
        {QStringLiteral("id"), trimmed},
        {QStringLiteral("status"), status}
    };
    put(QStringLiteral("videos"), updateQuery, body,
        [this, trimmed](const QJsonObject &) { emit vodMetadataUpdated(trimmed); });
}

void YouTubeLiveClient::deleteVideo(const QString &videoId)
{
    if (!isAuthenticated()) {
        emit failed(QStringLiteral("YouTube is not connected. Sign in with Google to delete this VOD."));
        return;
    }
    const QString trimmed = videoId.trimmed();
    if (trimmed.isEmpty()) {
        emit failed(QStringLiteral("This VOD has no YouTube video id to delete."));
        return;
    }

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("id"), trimmed);
    deleteRequest(QStringLiteral("videos"), query,
                  [this, trimmed](const QJsonObject &) { emit videoDeleted(trimmed); },
                  false,
                  [this, trimmed](const QJsonObject &) { emit videoAlreadyGone(trimmed); });
}

void YouTubeLiveClient::syncOwnLibrary()
{
    if (!isAuthenticated()) {
        return;
    }

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("part"), QStringLiteral("contentDetails"));
    query.addQueryItem(QStringLiteral("mine"), QStringLiteral("true"));
    query.addQueryItem(QStringLiteral("maxResults"), QStringLiteral("1"));
    get(QStringLiteral("channels"), query, [this](const QJsonObject &response) {
        const QJsonArray items = response.value(QStringLiteral("items")).toArray();
        if (items.isEmpty()) {
            emit ownLibrarySynced({}, {});
            return;
        }
        const QString playlistId = items.first().toObject()
                                       .value(QStringLiteral("contentDetails")).toObject()
                                       .value(QStringLiteral("relatedPlaylists")).toObject()
                                       .value(QStringLiteral("uploads")).toString();
        if (playlistId.isEmpty()) {
            emit ownLibrarySynced({}, {});
            return;
        }
        auto videoIds = std::make_shared<QStringList>();
        fetchUploadsPage(playlistId, {}, videoIds, 0);
    });
}

void YouTubeLiveClient::fetchUploadsPage(const QString &playlistId, const QString &pageToken,
                                         const std::shared_ptr<QStringList> &videoIds,
                                         int pagesScanned)
{
    if (pagesScanned >= kMaxUploadPagesToSync || videoIds->size() >= kMaxUploadVideosToSync) {
        auto vods = std::make_shared<QVector<Vod>>();
        auto clipsByVod = std::make_shared<QHash<QString, QVector<VodClip>>>();
        fetchVideoDetails(videoIds->mid(0, kMaxUploadVideosToSync), 0, vods, clipsByVod);
        return;
    }
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("part"), QStringLiteral("contentDetails"));
    query.addQueryItem(QStringLiteral("playlistId"), playlistId);
    query.addQueryItem(QStringLiteral("maxResults"), QStringLiteral("50"));
    if (!pageToken.isEmpty()) {
        query.addQueryItem(QStringLiteral("pageToken"), pageToken);
    }

    get(QStringLiteral("playlistItems"), query, [this, playlistId, videoIds, pagesScanned](const QJsonObject &response) {
        const QJsonArray items = response.value(QStringLiteral("items")).toArray();
        for (const QJsonValue &value : items) {
            const QString videoId = value.toObject()
                                        .value(QStringLiteral("contentDetails")).toObject()
                                        .value(QStringLiteral("videoId")).toString();
            if (!videoId.isEmpty()) {
                videoIds->append(videoId);
            }
        }

        const QString next = response.value(QStringLiteral("nextPageToken")).toString();
        if (!next.isEmpty()
            && pagesScanned + 1 < kMaxUploadPagesToSync
            && videoIds->size() < kMaxUploadVideosToSync) {
            fetchUploadsPage(playlistId, next, videoIds, pagesScanned + 1);
            return;
        }

        auto vods = std::make_shared<QVector<Vod>>();
        auto clipsByVod = std::make_shared<QHash<QString, QVector<VodClip>>>();
        fetchVideoDetails(videoIds->mid(0, kMaxUploadVideosToSync), 0, vods, clipsByVod);
    });
}

void YouTubeLiveClient::fetchVideoDetails(const QStringList &videoIds, int offset,
                                          const std::shared_ptr<QVector<Vod>> &vods,
                                          const std::shared_ptr<QHash<QString, QVector<VodClip>>> &clipsByVod)
{
    if (offset >= videoIds.size()) {
        emit ownLibrarySynced(*vods, *clipsByVod);
        return;
    }

    const QStringList batch = videoIds.mid(offset, 50);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("part"), QStringLiteral("snippet,contentDetails,status"));
    query.addQueryItem(QStringLiteral("id"), batch.join(QLatin1Char(',')));
    get(QStringLiteral("videos"), query, [this, videoIds, offset, vods, clipsByVod](const QJsonObject &response) {
        const QJsonArray items = response.value(QStringLiteral("items")).toArray();
        for (const QJsonValue &value : items) {
            const QJsonObject item = value.toObject();
            if (!isVodLinkVideo(item)) {
                continue;
            }
            Vod vod = vodFromVideoResource(item);
            if (vod.youtubeId.trimmed().isEmpty()) {
                continue;
            }
            QVector<VodClip> clips = clipsFromVideoResource(item);
            vods->push_back(vod);
            clipsByVod->insert(vod.youtubeId, clips);
        }
        fetchVideoDetails(videoIds, offset + 50, vods, clipsByVod);
    });
}

void YouTubeLiveClient::importClipUrl(const QString &clipUrl, const QString &fallbackVideoId,
                                      const QString &fallbackTitle, int fallbackStartSeconds,
                                      int fallbackEndSeconds)
{
    const QUrl url(clipUrl.trimmed());
    const QString host = url.host().toLower();
    if (!url.isValid()
        || !(host == QStringLiteral("youtube.com") || host == QStringLiteral("www.youtube.com")
             || host == QStringLiteral("m.youtube.com") || host == QStringLiteral("youtu.be"))
        || !url.path().contains(QStringLiteral("/clip/"))) {
        emit failed(QStringLiteral("Copy a real YouTube Clip link first."));
        return;
    }

    QNetworkRequest request(url);
    request.setTransferTimeout(15000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Mozilla/5.0 VodLink clip importer"));
    QNetworkReply *reply = m_network.get(request);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, url, fallbackVideoId, fallbackTitle, fallbackStartSeconds, fallbackEndSeconds]() {
        const QByteArray payload = reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();

        if (status < 200 || status >= 400) {
            emit failed(QStringLiteral("Could not import YouTube Clip link (HTTP %1).").arg(status));
            return;
        }

        const QString html = QString::fromUtf8(payload);
        QString decoded = htmlDecode(html);
        decoded.replace(QStringLiteral("\\\""), QStringLiteral("\""));
        QString clipId = clipIdFromUrl(url);
        if (clipId.isEmpty()) {
            clipId = firstCapture(decoded, QStringLiteral("\"clipId\"\\s*:\\s*\"([A-Za-z0-9_-]+)\""));
        }

        QString videoId = firstCapture(decoded, QStringLiteral("\"videoId\"\\s*:\\s*\"([A-Za-z0-9_-]{6,})\""));
        if (videoId.isEmpty()) {
            videoId = firstCapture(decoded, QStringLiteral("watch\\?v=([A-Za-z0-9_-]{6,})"));
        }
        if (videoId.isEmpty()) {
            videoId = fallbackVideoId.trimmed();
        }

        int startMs = firstIntCapture(decoded,
                                      {QStringLiteral("\"startTimeMs\"\\s*:\\s*\"?(\\d+)\"?"),
                                       QStringLiteral("\"startMs\"\\s*:\\s*\"?(\\d+)\"?")},
                                      -1);
        int endMs = firstIntCapture(decoded,
                                    {QStringLiteral("\"endTimeMs\"\\s*:\\s*\"?(\\d+)\"?"),
                                     QStringLiteral("\"endMs\"\\s*:\\s*\"?(\\d+)\"?")},
                                    -1);

        int start = fallbackStartSeconds;
        int end = fallbackEndSeconds;
        if (startMs >= 0) {
            start = startMs / 1000;
        } else {
            start = firstIntCapture(decoded,
                                    {QStringLiteral("\"startTimeSeconds\"\\s*:\\s*\"?(\\d+)\"?"),
                                     QStringLiteral("\"startSeconds\"\\s*:\\s*\"?(\\d+)\"?")},
                                    fallbackStartSeconds);
        }
        if (endMs >= 0) {
            end = endMs / 1000;
        } else {
            end = firstIntCapture(decoded,
                                  {QStringLiteral("\"endTimeSeconds\"\\s*:\\s*\"?(\\d+)\"?"),
                                   QStringLiteral("\"endSeconds\"\\s*:\\s*\"?(\\d+)\"?")},
                                  fallbackEndSeconds);
        }
        end = std::max(start + 1, end);

        QString title = firstCapture(decoded, QStringLiteral("<meta\\s+property=\"og:title\"\\s+content=\"([^\"]*)\""));
        if (title.isEmpty()) {
            title = firstCapture(decoded, QStringLiteral("<title>(.*?)</title>"));
        }
        title.remove(QStringLiteral(" - YouTube"));
        if (title.trimmed().isEmpty()) {
            title = fallbackTitle.trimmed().isEmpty()
                        ? QStringLiteral("YouTube Clip")
                        : fallbackTitle.trimmed();
        }

        VodClip clip;
        clip.youtubeId = videoId;
        clip.clipId = clipId;
        clip.clipUrl = canonicalClipUrl(clipId, url.toString());
        clip.title = title.left(120);
        clip.startSeconds = std::max(0, start);
        clip.endSeconds = std::max(clip.startSeconds + 1, end);
        clip.createdAt = QDateTime::currentDateTimeUtc();
        emit clipImported(clip);
    });
}

void YouTubeLiveClient::updateVodLinkMetadata(const Vod &vod, const QVector<VodClip> &clips)
{
    const QString videoId = vod.youtubeId.trimmed();
    if (!isAuthenticated() || videoId.isEmpty() || !vod.isMine()) {
        return;
    }

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("part"), QStringLiteral("snippet,status"));
    query.addQueryItem(QStringLiteral("id"), videoId);
    get(QStringLiteral("videos"), query, [this, vod, clips, videoId](const QJsonObject &response) {
        const QJsonArray items = response.value(QStringLiteral("items")).toArray();
        if (items.isEmpty()) {
            return;
        }

        const QJsonObject item = items.first().toObject();
        const QJsonObject snippet = item.value(QStringLiteral("snippet")).toObject();
        QJsonObject updatedSnippet;
        updatedSnippet.insert(QStringLiteral("title"),
                              snippet.value(QStringLiteral("title")).toString(vod.title));
        updatedSnippet.insert(QStringLiteral("categoryId"),
                              snippet.value(QStringLiteral("categoryId")).toString(QStringLiteral("20")));
        updatedSnippet.insert(QStringLiteral("description"),
                              descriptionWithVodLinkMetadata(
                                  snippet.value(QStringLiteral("description")).toString(),
                                  vod,
                                  clips));
        if (snippet.contains(QStringLiteral("tags"))) {
            updatedSnippet.insert(QStringLiteral("tags"), snippet.value(QStringLiteral("tags")).toArray());
        }

        const QJsonObject updatedStatus = vodLinkSafeVideoStatus();

        QUrlQuery updateQuery;
        updateQuery.addQueryItem(QStringLiteral("part"), QStringLiteral("snippet,status"));
        const QJsonObject body {
            {QStringLiteral("id"), videoId},
            {QStringLiteral("snippet"), updatedSnippet},
            {QStringLiteral("status"), updatedStatus}
        };
        put(QStringLiteral("videos"), updateQuery, body,
            [this, videoId](const QJsonObject &) { emit vodMetadataUpdated(videoId); });
    });
}

void YouTubeLiveClient::get(const QString &path, const QUrlQuery &query,
                            JsonHandler onSuccess, bool retrying)
{
    QUrl url(apiBase() + path);
    url.setQuery(query);
    QNetworkRequest request(url);
    request.setTransferTimeout(15000);
    request.setRawHeader("Authorization",
                         QByteArray("Bearer ") + (m_auth ? m_auth->accessToken().toUtf8() : QByteArray()));
    QNetworkReply *reply = m_network.get(request);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, path, query, handler = std::move(onSuccess), retrying]() mutable {
        const QByteArray payload = reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();

        if (status == 401 && !retrying && m_auth != nullptr && !m_auth->refreshToken().isEmpty()) {
            auto connection = std::make_shared<QMetaObject::Connection>();
            *connection = connect(m_auth, &GoogleAuth::tokensChanged, this,
                                  [this, connection, path, query, handler]() mutable {
                                      disconnect(*connection);
                                      get(path, query, std::move(handler), true);
                                  });
            m_auth->refreshNow();
            return;
        }

        const QJsonDocument document = QJsonDocument::fromJson(payload);
        if (status < 200 || status >= 300) {
            const QJsonObject apiError = document.object().value(QStringLiteral("error")).toObject();
            const QString message = apiError.value(QStringLiteral("message")).toString();
            emit failed(message.isEmpty()
                            ? QStringLiteral("YouTube request failed with HTTP %1.").arg(status)
                            : message);
            return;
        }
        handler(document.object());
    });
}

void YouTubeLiveClient::post(const QString &path, const QUrlQuery &query,
                             const QJsonObject &body, JsonHandler onSuccess, bool retrying)
{
    QUrl url(apiBase() + path);
    url.setQuery(query);
    QNetworkRequest request(url);
    request.setTransferTimeout(15000);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization",
                         QByteArray("Bearer ") + (m_auth ? m_auth->accessToken().toUtf8() : QByteArray()));
    QNetworkReply *reply = m_network.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, path, query, body, handler = std::move(onSuccess), retrying]() mutable {
        const QByteArray payload = reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();

        // Token likely expired — refresh once and replay the request.
        if (status == 401 && !retrying && m_auth != nullptr && !m_auth->refreshToken().isEmpty()) {
            auto connection = std::make_shared<QMetaObject::Connection>();
            *connection = connect(m_auth, &GoogleAuth::tokensChanged, this,
                                  [this, connection, path, query, body, handler]() mutable {
                                      disconnect(*connection);
                                      post(path, query, body, std::move(handler), true);
                                  });
            m_auth->refreshNow();
            return;
        }

        const QJsonDocument document = QJsonDocument::fromJson(payload);
        if (status < 200 || status >= 300) {
            const QJsonObject apiError = document.object().value(QStringLiteral("error")).toObject();
            const QString message = apiError.value(QStringLiteral("message")).toString();
            emit failed(message.isEmpty()
                            ? QStringLiteral("YouTube request failed with HTTP %1.").arg(status)
                            : message);
            return;
        }
        handler(document.object());
    });
}

void YouTubeLiveClient::put(const QString &path, const QUrlQuery &query,
                            const QJsonObject &body, JsonHandler onSuccess, bool retrying)
{
    QUrl url(apiBase() + path);
    url.setQuery(query);
    QNetworkRequest request(url);
    request.setTransferTimeout(15000);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization",
                         QByteArray("Bearer ") + (m_auth ? m_auth->accessToken().toUtf8() : QByteArray()));
    QNetworkReply *reply = m_network.sendCustomRequest(
        request,
        QByteArrayLiteral("PUT"),
        QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, path, query, body, handler = std::move(onSuccess), retrying]() mutable {
        const QByteArray payload = reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();

        if (status == 401 && !retrying && m_auth != nullptr && !m_auth->refreshToken().isEmpty()) {
            auto connection = std::make_shared<QMetaObject::Connection>();
            *connection = connect(m_auth, &GoogleAuth::tokensChanged, this,
                                  [this, connection, path, query, body, handler]() mutable {
                                      disconnect(*connection);
                                      put(path, query, body, std::move(handler), true);
                                  });
            m_auth->refreshNow();
            return;
        }

        const QJsonDocument document = QJsonDocument::fromJson(payload);
        if (status < 200 || status >= 300) {
            const QJsonObject apiError = document.object().value(QStringLiteral("error")).toObject();
            const QString message = apiError.value(QStringLiteral("message")).toString();
            emit failed(message.isEmpty()
                            ? QStringLiteral("YouTube update failed with HTTP %1.").arg(status)
                            : message);
            return;
        }
        handler(document.object());
    });
}

void YouTubeLiveClient::deleteRequest(const QString &path, const QUrlQuery &query,
                                      JsonHandler onSuccess, bool retrying,
                                      JsonHandler onNotFound)
{
    QUrl url(apiBase() + path);
    url.setQuery(query);
    QNetworkRequest request(url);
    request.setTransferTimeout(15000);
    request.setRawHeader("Authorization",
                         QByteArray("Bearer ") + (m_auth ? m_auth->accessToken().toUtf8() : QByteArray()));
    QNetworkReply *reply = m_network.deleteResource(request);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, path, query, handler = std::move(onSuccess),
             notFoundHandler = std::move(onNotFound), retrying]() mutable {
        const QByteArray payload = reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();

        if (status == 401 && !retrying && m_auth != nullptr && !m_auth->refreshToken().isEmpty()) {
            auto connection = std::make_shared<QMetaObject::Connection>();
            *connection = connect(m_auth, &GoogleAuth::tokensChanged, this,
                                  [this, connection, path, query,
                                       handler = std::move(handler),
                                       notFoundHandler = std::move(notFoundHandler)]() mutable {
                                      disconnect(*connection);
                                      deleteRequest(path, query, std::move(handler), true,
                                                    std::move(notFoundHandler));
                                  });
            m_auth->refreshNow();
            return;
        }

        const QJsonDocument document = QJsonDocument::fromJson(payload);
        if (status < 200 || status >= 300) {
            if ((status == 404 || status == 410) && notFoundHandler) {
                notFoundHandler(document.object());
                return;
            }

            const QJsonObject apiError = document.object().value(QStringLiteral("error")).toObject();
            const QString message = apiError.value(QStringLiteral("message")).toString();
            emit failed(message.isEmpty()
                            ? QStringLiteral("YouTube delete failed with HTTP %1.").arg(status)
                            : message);
            return;
        }
        handler(document.object());
    });
}
