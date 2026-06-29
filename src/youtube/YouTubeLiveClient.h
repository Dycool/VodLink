#pragma once

#include "library/Vod.h"

#include <QHash>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVector>

#include <functional>
#include <memory>

class GoogleAuth;

class YouTubeLiveClient final : public QObject
{
    Q_OBJECT

public:
    explicit YouTubeLiveClient(QObject *parent = nullptr);

    void setAuth(GoogleAuth *auth);
    void setStreamSettings(const QString &resolution, int fps);
    [[nodiscard]] bool isAuthenticated() const;
    // Probes whether this account may go live (liveBroadcasts.list?mine=true fails
    // with liveStreamingNotEnabled when streaming isn't activated yet).
    void checkStreamingEligibility(bool retrying = false);
    void prepareBroadcast(const QString &game);
    void startStreamStatusPolling(const QString &broadcastId, const QString &streamId);
    void stopStreamStatusPolling(const QString &broadcastId = {}, const QString &streamId = {});
    void completeBroadcast(const QString &broadcastId);
    void ensureVodEmbeddable(const QString &videoId);
    void deleteVideo(const QString &videoId);

    // YouTube is the source of truth. This imports all VodLink-marked videos from
    // the signed-in channel and rebuilds the local VOD/clip cache.
    void syncOwnLibrary();

    // Imports a real YouTube Clip URL by scraping the public clip page, because the
    // official YouTube Data API does not expose clips.list/clips.insert.
    void importClipUrl(const QString &clipUrl, const QString &fallbackVideoId,
                       const QString &fallbackTitle, int fallbackStartSeconds,
                       int fallbackEndSeconds);

    // Writes the local clip index back into the parent VOD description so a fresh
    // PC can rebuild the same VOD + clip cache from YouTube.
    void updateVodLinkMetadata(const Vod &vod, const QVector<VodClip> &clips);

signals:
    void broadcastReady(const QString &broadcastId, const QString &streamId, const QUrl &ingestUrl);
    void broadcastCompleted(const QString &broadcastId);
    void videoDeleted(const QString &videoId);
    void videoAlreadyGone(const QString &videoId);
    void failed(const QString &message);
    void streamingEligible();
    void streamingUnavailable(const QString &reason, const QString &message);
    void streamStatusChanged(const QString &broadcastId, const QString &streamId,
                             const QString &streamStatus, const QString &healthStatus,
                             const QString &configurationIssues);
    void streamStatusProbeFailed(const QString &broadcastId, const QString &streamId,
                                 const QString &message);
    void ownLibrarySynced(const QVector<Vod> &vods,
                          const QHash<QString, QVector<VodClip>> &clipsByVod);
    void clipImported(const VodClip &clip);
    void vodMetadataUpdated(const QString &videoId);

private:
    using JsonHandler = std::function<void(const QJsonObject &)>;

    void get(const QString &path, const QUrlQuery &query,
             JsonHandler onSuccess, bool retrying = false);
    void post(const QString &path, const QUrlQuery &query, const QJsonObject &body,
              JsonHandler onSuccess, bool retrying = false);
    void put(const QString &path, const QUrlQuery &query, const QJsonObject &body,
             JsonHandler onSuccess, bool retrying = false);
    void deleteRequest(const QString &path, const QUrlQuery &query,
                       JsonHandler onSuccess, bool retrying = false,
                       JsonHandler onNotFound = {});
    void createStream(const QString &game, const QString &broadcastId);
    void bind(const QString &broadcastId, const QString &streamId,
              const QString &ingestionAddress, const QString &streamName);
    void pollStreamStatus(bool retrying = false);
    void fetchUploadsPage(const QString &playlistId, const QString &pageToken,
                          const std::shared_ptr<QStringList> &videoIds,
                          int pagesScanned);
    void fetchVideoDetails(const QStringList &videoIds, int offset,
                           const std::shared_ptr<QVector<Vod>> &vods,
                           const std::shared_ptr<QHash<QString, QVector<VodClip>>> &clipsByVod);
    void updateVideoStatusForEmbedding(const QString &videoId, const QJsonObject &existingStatus);

    QNetworkAccessManager m_network;
    GoogleAuth *m_auth = nullptr;
    QString m_resolution = QStringLiteral("1080p");
    QString m_frameRate = QStringLiteral("60fps");
    QString m_statusBroadcastId;
    QString m_statusStreamId;
    QTimer m_statusPollTimer;
    bool m_statusProbeInFlight = false;
};
