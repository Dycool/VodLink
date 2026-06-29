#pragma once

#include "app/Config.h"
#include "auth/GoogleAuth.h"
#include "cloud/SessionClient.h"
#include "games/GameCatalog.h"
#include "games/GameDetector.h"
#include "library/VodRepository.h"
#include "streaming/RtmpStreamer.h"
#include "youtube/YouTubeLiveClient.h"

#include <QHash>
#include <QObject>
#include <QStringList>

class AppController final : public QObject
{
    Q_OBJECT

public:
    explicit AppController(QObject *parent = nullptr);

    bool initialize(QString *error);
    void startMonitoring();

    [[nodiscard]] QVector<Vod> vods(const QString &game, QString *error) const;
    [[nodiscard]] QVector<Vod> libraryVods(const QString &game, QString *error) const;
    [[nodiscard]] QVector<Vod> sessionVods(const QString &game, QString *error) const;
    [[nodiscard]] QStringList games(QString *error) const;
    [[nodiscard]] QVector<VodClip> clipsForVod(const QString &youtubeId, QString *error) const;
    bool addClipForVod(const QString &youtubeId, const QString &title,
                       int startSeconds, int endSeconds, QString *error);
    void importClipForVod(const QString &clipUrl, const Vod &vod, const QString &title,
                          int startSeconds, int endSeconds);
    [[nodiscard]] bool isRecording() const;
    [[nodiscard]] bool isLiveConfirmed() const;
    [[nodiscard]] QString currentGame() const;
    // Name of the most recently played (detected) game, persisted across restarts.
    [[nodiscard]] QString lastGame() const;

    // Google account.
    [[nodiscard]] bool isAuthConfigured() const;
    [[nodiscard]] bool isSignedIn() const;
    [[nodiscard]] QString accountEmail() const;
    [[nodiscard]] QString accountName() const;
    [[nodiscard]] QString accountPictureUrl() const;
    [[nodiscard]] bool canDeleteVod(const Vod &vod) const;
    [[nodiscard]] bool canAttachClipsToVod(const Vod &vod) const;
    void ensureVodEmbeddable(const Vod &vod);

    // Friends (local list; mutuality resolved by the Worker).
    [[nodiscard]] QStringList friends(QString *error) const;
    [[nodiscard]] QVector<AccountProfile> friendProfiles(QString *error) const;

    // Sharing toggle (gates all Worker traffic).
    [[nodiscard]] bool shareEnabled() const;
    [[nodiscard]] QString privacyMode() const;
    [[nodiscard]] bool captureFullDesktop() const;
    [[nodiscard]] bool autoRecordEnabled() const;
    [[nodiscard]] bool isWorkerConfigured() const;

    // True when a refresh token is stored, i.e. a silent sign-in is being restored
    // on startup. Used to avoid prompting the user to sign in again.
    [[nodiscard]] bool hasStoredCredentials() const;

    // Generic local settings used by the UI/recorder.
    [[nodiscard]] QString appSetting(const QString &key, const QString &defaultValue = {}) const;
    void setAppSetting(const QString &key, const QString &value);

    // Manual "add current game" support.
    [[nodiscard]] QStringList runningProcesses() const;

public slots:
    void stopRecording();
    // Called before the app exits so an active RTMP session is stopped and the
    // Worker / YouTube finalization requests are launched while tokens still exist.
    bool shutdownForQuit();
    // Stops any active stream/session, signs out, deletes all local VodLink data,
    // and returns whether async network cleanup was launched before the app exits.
    bool factoryResetLocalData(QString *error);
    void signIn();
    void signOut();
    void addFriend(const QString &email);
    void removeFriend(const QString &email);
    void setShareEnabled(bool enabled);
    void setPrivacyMode(const QString &mode);
    void setCaptureFullDesktop(bool enabled);
    void setAutoRecordEnabled(bool enabled);
    void syncOwnLibrary();
    void addUserGame(const QString &executable, const QString &name);
    void deleteVod(const Vod &vod);
    void removeFriendVodFromLibrary(const Vod &vod);

signals:
    void libraryChanged();
    void statusChanged(const QString &message, bool streaming);
    void errorOccurred(const QString &message);
    void accountChanged(const QString &email); // empty when signed out
    void friendsChanged();
    void vodDeleted(const QString &youtubeId);
    void clipImported(const QString &youtubeId);
    void shareEnabledChanged(bool enabled);
    void autoRecordEnabledChanged(bool enabled);
    void lastGameChanged(const QString &game);
    // Emitted once after sign-in when YouTube live streaming is not available on
    // the account; carries a user-facing explanation.
    void streamingUnavailable(const QString &explanation);

private slots:
    void onGameStarted(const GameDefinition &game);
    void onGameStopped(const GameDefinition &game);
    void onBroadcastReady(const QString &broadcastId, const QString &streamId, const QUrl &ingestUrl);
    void onEncoderStopped();
    void onYouTubeStreamStatus(const QString &broadcastId, const QString &streamId,
                               const QString &streamStatus, const QString &healthStatus,
                               const QString &configurationIssues);
    void onYouTubeStreamStatusProbeFailed(const QString &broadcastId, const QString &streamId,
                                          const QString &message);
    void onMatchesReceived(const QVector<Vod> &friendVods);
    void onOwnLibrarySynced(const QVector<Vod> &vods,
                            const QHash<QString, QVector<VodClip>> &clipsByVod);
    void onClipImported(const VodClip &clip);

private:
    enum class StreamState { Idle, Preparing, WaitingForYouTube, Streaming, Stopping };

    void resetStreamState();
    void announceFriendSessionIfReady();
    void applyRecorderSettings();
    void updateWaitingStatus();
    bool finishActiveStreamBeforeAccountDisconnect(const QString &statusMessage);
    void finishFriendSessionIfNeeded();
    void adoptSignedInAccount(const QString &email);
    void startOwnLibrarySync();
    Vod ownVodByYoutubeId(const QString &youtubeId) const;

    Config m_config;
    VodRepository m_repository;
    GameCatalog m_catalog;
    GameDetector m_detector;
    RtmpStreamer m_streamer;
    GoogleAuth m_auth;
    YouTubeLiveClient m_youtube;
    SessionClient m_session;

    QString m_currentGame;
    QString m_lastGame;
    GameDefinition m_currentGameDefinition;
    QString m_broadcastId;
    QString m_streamId;
    QString m_discardingBroadcastId;
    QString m_lastYouTubeWaitDetail;
    QString m_lastLocalStreamDetail;
    QDateTime m_startedAt;
    StreamState m_streamState = StreamState::Idle;
    bool m_cancelRequested = false;
    bool m_encoderStarted = false;
    bool m_youtubeLiveConfirmed = false;
    bool m_shareEnabled = false;
    QString m_privacyMode;
    bool m_captureFullDesktop = false; // kept only to migrate old settings
    bool m_autoRecordEnabled = false;
    bool m_eligibilityChecked = false;
    bool m_explicitlySignedOut = false;
    bool m_friendSessionAnnounced = false;
    bool m_shutdownStarted = false;
    bool m_librarySyncStarted = false;
    bool m_librarySyncActive = false;
    QHash<QString, Vod> m_pendingClipVodByVideoId;
};
