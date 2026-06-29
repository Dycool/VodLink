#include "AppController.h"

#include "app/DebugLog.h"

#include <algorithm>

#include <QDateTime>
#include <QFileInfo>
#include <QGuiApplication>
#include <QScreen>
#include <QSize>

namespace {
constexpr auto kShareSetting = "share_vods";
constexpr auto kAutoRecordSetting = "auto_record";
constexpr auto kLastGameSetting = "last_game";
constexpr auto kRefreshTokenSetting = "oauth_refresh_token";
constexpr auto kAccountEmailSetting = "account_email";
constexpr auto kPrivacyModeSetting = "privacy_mode";
constexpr auto kFullDesktopSetting = "capture_full_desktop"; // legacy migration
constexpr auto kPrivacyGameOnly = "game_only";
constexpr auto kPrivacyGameExternalAudio = "game_external_audio";
constexpr auto kPrivacyFullDesktop = "full_desktop";
constexpr auto kEncoderSetting = "recorder_encoder";
constexpr auto kBitrateSetting = "recorder_bitrate_kbps";
constexpr auto kResolutionSetting = "recorder_resolution";
constexpr auto kFpsSetting = "recorder_fps";
constexpr auto kYouTubeLibraryLastSyncMsSetting = "youtube_library_last_sync_ms";

QString normalizedPrivacyMode(QString value)
{
    value = value.trimmed().toLower();
    if (value == QString::fromLatin1(kPrivacyGameOnly)
        || value == QString::fromLatin1(kPrivacyGameExternalAudio)
        || value == QString::fromLatin1(kPrivacyFullDesktop)) {
        return value;
    }

    // New installs should not default into per-process audio capture.  OBS'
    // Windows application-audio source is the riskiest capture path in an
    // embedded private runtime.  Keep Game-only available when explicitly chosen,
    // but make the default a stable, honest mode: game/window video plus system
    // audio.
    return QString::fromLatin1(kPrivacyGameExternalAudio);
}

QSize nativeRecorderResolution()
{
    const QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        const QList<QScreen *> screens = QGuiApplication::screens();
        if (!screens.isEmpty()) {
            screen = screens.first();
        }
    }

    if (screen == nullptr) {
        return QSize(1920, 1080);
    }

    const qreal ratio = screen->devicePixelRatio();
    QSize native(qRound(screen->geometry().width() * ratio),
                 qRound(screen->geometry().height() * ratio));
    if (native.width() < 640 || native.height() < 360) {
        return QSize(1920, 1080);
    }

    // Keep encoder-compatible even dimensions without jumping to a different
    // preset. Real monitor modes are normally already even, but this avoids
    // sending odd dimensions to 4:2:0 encoders.
    native.setWidth(native.width() & ~1);
    native.setHeight(native.height() & ~1);
    return native.isEmpty() ? QSize(1920, 1080) : native;
}

QString nativeRecorderResolutionText()
{
    const QSize size = nativeRecorderResolution();
    return QStringLiteral("%1x%2").arg(size.width()).arg(size.height());
}


// Google/YouTube Live recommended bitrate table, in Kbps. For H.264 the
// table gives a single recommended value. For HEVC/AV1 the table gives
// min/max ranges, so VodLink uses the matching H.264 recommendation clamped
// into the HEVC/AV1 range. This preserves quality while staying inside
// Google's current ingest guidance.
int youtubeRecommendedBitrateKbps(const QSize &size, int fps, const QString &encoder)
{
    const int height = size.height();
    const bool highFps = fps >= 50;
    const QString e = encoder.toLower();
    const bool efficientCodec = e.contains(QStringLiteral("av1"))
        || e.contains(QStringLiteral("265")) || e.contains(QStringLiteral("hevc"));

    int h264 = 12000;
    int efficientMin = 4000;
    int efficientMax = 10000;
    if (height >= 2160) {
        h264 = highFps ? 35000 : 30000;
        efficientMin = highFps ? 10000 : 8000;
        efficientMax = highFps ? 40000 : 35000;
    } else if (height >= 1440) {
        h264 = highFps ? 24000 : 15000;
        efficientMin = highFps ? 6000 : 5000;
        efficientMax = highFps ? 30000 : 25000;
    } else if (height >= 1080) {
        h264 = highFps ? 12000 : 10000;
        efficientMin = highFps ? 4000 : 3000;
        efficientMax = highFps ? 10000 : 8000;
    } else if (height >= 720) {
        h264 = highFps ? 6000 : 4000;
        efficientMin = 3000;
        efficientMax = 8000;
    } else {
        h264 = 4000;
        efficientMin = 3000;
        efficientMax = 8000;
    }

    return efficientCodec ? std::clamp(h264, efficientMin, efficientMax) : h264;
}

QSize parseRecorderResolution(const QString &value)
{
    const QString normalized = value.trimmed().toLower();
    const QStringList parts = normalized.split(QLatin1Char('x'));
    if (parts.size() != 2) {
        return nativeRecorderResolution();
    }

    bool widthOk = false;
    bool heightOk = false;
    const int width = parts.at(0).trimmed().toInt(&widthOk);
    const int height = parts.at(1).trimmed().toInt(&heightOk);
    if (!widthOk || !heightOk) {
        return nativeRecorderResolution();
    }

    // Encoders/muxers generally require even dimensions for 4:2:0 video. Do not
    // silently scale to another preset; only accept the user's exact even WxH.
    if (width < 640 || height < 360 || (width % 2) != 0 || (height % 2) != 0) {
        return nativeRecorderResolution();
    }

    return QSize(width, height);
}

QString normalizedEmail(const QString &email)
{
    return email.trimmed().toLower();
}

bool looksLikeYouTubeRateLimit(const QString &message)
{
    const QString lower = message.toLower();
    return lower.contains(QStringLiteral("rate limit"))
        || lower.contains(QStringLiteral("quota"))
        || lower.contains(QStringLiteral("too many requests"));
}

QString friendlyYouTubeError(const QString &message)
{
    if (!looksLikeYouTubeRateLimit(message)) {
        return message;
    }
    return QStringLiteral(
        "YouTube is rate-limiting VodLink right now. Wait a bit and try again. "
        "Manual VOD sync is available from Settings when the quota recovers.");
}
} // namespace

AppController::AppController(QObject *parent)
    : QObject(parent), m_detector(&m_catalog)
{
    connect(this, &AppController::statusChanged, this, [](const QString &message, bool streaming) {
        DebugLog::writeCategory(QStringLiteral("status"),
                                QStringLiteral("%1 | streaming=%2")
                                    .arg(message, streaming ? QStringLiteral("true") : QStringLiteral("false")));
    });
    connect(this, &AppController::errorOccurred, this, [](const QString &message) {
        DebugLog::writeCategory(QStringLiteral("error"), message);
    });
    connect(this, &AppController::streamingUnavailable, this, [](const QString &message) {
        DebugLog::writeCategory(QStringLiteral("youtube"), QStringLiteral("streaming unavailable: %1").arg(message));
    });

    connect(&m_detector, &GameDetector::gameStarted, this, &AppController::onGameStarted);
    connect(&m_detector, &GameDetector::gameStopped, this, &AppController::onGameStopped);
    connect(&m_detector, &GameDetector::scanFailed, this, &AppController::errorOccurred);

    m_youtube.setAuth(&m_auth);
    connect(&m_youtube, &YouTubeLiveClient::broadcastReady,
            this, &AppController::onBroadcastReady);
    connect(&m_youtube, &YouTubeLiveClient::streamStatusChanged,
            this, &AppController::onYouTubeStreamStatus);
    connect(&m_youtube, &YouTubeLiveClient::streamStatusProbeFailed,
            this, &AppController::onYouTubeStreamStatusProbeFailed);
    connect(&m_youtube, &YouTubeLiveClient::broadcastCompleted, this,
            [this](const QString &broadcastId) {
                if (m_streamState == StreamState::Stopping
                    && broadcastId == m_broadcastId) {
                    resetStreamState();
                }
            });
    connect(&m_youtube, &YouTubeLiveClient::videoDeleted, this, [this](const QString &videoId) {
        if (!m_discardingBroadcastId.isEmpty() && videoId == m_discardingBroadcastId) {
            m_discardingBroadcastId.clear();
            emit statusChanged(QStringLiteral("Cancelled unused YouTube broadcast"), false);
            return;
        }

        QString error;
        if (!m_repository.removeOwnVod(videoId, &error)) {
            emit errorOccurred(QStringLiteral("The VOD was deleted on YouTube, but removing it locally failed: %1").arg(error));
            return;
        }
        emit vodDeleted(videoId);
        emit libraryChanged();
        emit statusChanged(QStringLiteral("Deleted VOD from YouTube"), false);
    });
    connect(&m_youtube, &YouTubeLiveClient::videoAlreadyGone, this, [this](const QString &videoId) {
        if (!m_discardingBroadcastId.isEmpty() && videoId == m_discardingBroadcastId) {
            m_discardingBroadcastId.clear();
            emit statusChanged(QStringLiteral("Cancelled unused YouTube broadcast"), false);
            return;
        }

        QString error;
        if (!m_repository.removeOwnVod(videoId, &error)) {
            emit errorOccurred(QStringLiteral("The VOD is already gone from YouTube, but removing it locally failed: %1").arg(error));
            return;
        }
        emit vodDeleted(videoId);
        emit libraryChanged();
        emit statusChanged(QStringLiteral("Removed local VOD entry; it was already gone from YouTube"), false);
    });
    connect(&m_youtube, &YouTubeLiveClient::ownLibrarySynced,
            this, &AppController::onOwnLibrarySynced);
    connect(&m_youtube, &YouTubeLiveClient::clipImported,
            this, &AppController::onClipImported);
    connect(&m_youtube, &YouTubeLiveClient::vodMetadataUpdated, this, [this](const QString &) {
        emit statusChanged(QStringLiteral("Synced VOD metadata to YouTube"), false);
    });
    connect(&m_youtube, &YouTubeLiveClient::streamingUnavailable, this,
            [this](const QString &reason, const QString &message) {
        QString explanation;
        if (reason == QStringLiteral("liveStreamingNotEnabled")) {
            explanation = QStringLiteral(
                "Live streaming isn't enabled on this YouTube account yet, so VodLink can't create VODs.\n\n"
                "Enable it at youtube.com/features and verify your phone number. The first time you enable "
                "live streaming, YouTube can take up to 24 hours to activate it — try again after that.");
        } else if (reason == QStringLiteral("livePermissionBlocked")
                   || reason.contains(QStringLiteral("blocked"), Qt::CaseInsensitive)) {
            explanation = QStringLiteral(
                "Live streaming is currently blocked on this YouTube account. This usually follows a recent "
                "live-streaming restriction (they last 90 days) or a community-guidelines strike. "
                "Check youtube.com for details.");
        } else {
            explanation = QStringLiteral(
                "YouTube live streaming isn't available on this account right now, so VodLink can't create "
                "VODs.\n\nMake sure live streaming is enabled at youtube.com/features and your phone is "
                "verified. Newly enabled accounts can take up to 24 hours to activate.%1")
                .arg(message.isEmpty() ? QString() : QStringLiteral("\n\nYouTube said: %1").arg(message));
        }
        emit streamingUnavailable(explanation);
    });
    connect(&m_youtube, &YouTubeLiveClient::failed, this, [this](const QString &message) {
        if (m_librarySyncActive && m_streamState == StreamState::Idle && !m_streamer.isStreaming()) {
            m_librarySyncActive = false;
            m_librarySyncStarted = false;
            emit statusChanged(looksLikeYouTubeRateLimit(message)
                                   ? QStringLiteral("YouTube is rate-limiting manual VOD sync; keeping the local cache for now.")
                                   : QStringLiteral("YouTube VOD sync failed; keeping the local cache for now."),
                               false);
            return;
        }

        emit errorOccurred(friendlyYouTubeError(message));
        if (!m_streamer.isStreaming()
            && (m_streamState == StreamState::Preparing
                || m_streamState == StreamState::WaitingForYouTube
                || m_streamState == StreamState::Stopping)) {
            resetStreamState();
        }
    });

    connect(&m_streamer, &RtmpStreamer::started, this, [this] {
        DebugLog::writeCategory(QStringLiteral("streamer"), QStringLiteral("started signal received"));
        m_encoderStarted = true;
        if (m_streamState == StreamState::Preparing
            || m_streamState == StreamState::WaitingForYouTube) {
            // Do not run YouTube's liveStreams health/status poll in the background.
            // The local RTMP output is the only live signal VodLink keeps watching;
            // YouTube finalization still checks lifecycle later to avoid invalid
            // transitions when the platform never received packets.
            m_streamState = StreamState::Streaming;
            m_youtubeLiveConfirmed = true;
            m_lastYouTubeWaitDetail.clear();
            emit statusChanged(QStringLiteral("Streaming %1 to YouTube").arg(m_currentGame), true);
            announceFriendSessionIfReady();
        }
    });
    connect(&m_streamer, &RtmpStreamer::stopped, this, &AppController::onEncoderStopped);
    connect(&m_streamer, &RtmpStreamer::failed, this, [this](const QString &message) {
        DebugLog::writeCategory(QStringLiteral("streamer"), QStringLiteral("failed signal: %1").arg(message));
        emit errorOccurred(message);
    });
    connect(&m_streamer, &RtmpStreamer::diagnosticsChanged, this,
            [this](const QString &message, bool localPacketsWritten) {
                Q_UNUSED(localPacketsWritten);
                DebugLog::writeCategory(QStringLiteral("streamer"), QStringLiteral("diagnostics: %1").arg(message.trimmed()));
                m_lastLocalStreamDetail = message.trimmed();
                if (m_streamState == StreamState::WaitingForYouTube && !m_youtubeLiveConfirmed) {
                    updateWaitingStatus();
                }
            });

    connect(&m_auth, &GoogleAuth::signedIn, this, [this](const QString &email) {
        if (m_explicitlySignedOut) {
            return;
        }
        adoptSignedInAccount(email);
        emit accountChanged(email);
        const QString who = m_auth.displayName().trimmed().isEmpty() ? email : m_auth.displayName().trimmed();
        emit statusChanged(QStringLiteral("Signed in as %1").arg(who), false);
    });
    connect(&m_auth, &GoogleAuth::signedOut, this, [this] {
        m_eligibilityChecked = false;
        emit accountChanged(QString());
    });
    // A silent restore (or periodic refresh) updates the email/profile without
    // firing signedIn; refresh the account UI (and avatar) whenever tokens change.
    connect(&m_auth, &GoogleAuth::tokensChanged, this, [this] {
        if (m_explicitlySignedOut) {
            return;
        }
        if (!m_auth.email().isEmpty()) {
            adoptSignedInAccount(m_auth.email());
            emit accountChanged(m_auth.email());
        }
        // Once we have a usable access token, probe live-streaming eligibility once
        // so we can warn the user up front instead of only failing at record time.
        if (!m_eligibilityChecked && !m_auth.accessToken().isEmpty()) {
            m_eligibilityChecked = true;
            m_youtube.checkStreamingEligibility();
        }
    });
    connect(&m_auth, &GoogleAuth::refreshTokenChanged, this, [this](const QString &token) {
        if (m_explicitlySignedOut && !token.isEmpty()) {
            return;
        }
        m_repository.setSetting(QString::fromLatin1(kRefreshTokenSetting), token);
    });
    connect(&m_auth, &GoogleAuth::authError, this, &AppController::errorOccurred);

    m_session.setIdTokenProvider([this] { return m_auth.idToken(); });
    connect(&m_session, &SessionClient::matchesReceived, this, &AppController::onMatchesReceived);
    connect(&m_session, &SessionClient::requestFailed, this, &AppController::errorOccurred);
}

bool AppController::initialize(QString *error)
{
    if (!m_repository.open(error)) {
        return false;
    }

    m_catalog.setUserEntries(m_repository.userGames(nullptr));
    m_shareEnabled = m_repository.setting(QString::fromLatin1(kShareSetting),
                                          QStringLiteral("0")) == QStringLiteral("1");
    m_privacyMode = normalizedPrivacyMode(m_repository.setting(QString::fromLatin1(kPrivacyModeSetting)));
    if (m_privacyMode == QString::fromLatin1(kPrivacyGameOnly)
        && m_repository.setting(QString::fromLatin1(kFullDesktopSetting), QStringLiteral("0")) == QStringLiteral("1")) {
        // Migrate the old checkbox without changing existing users' behavior.
        m_privacyMode = QString::fromLatin1(kPrivacyFullDesktop);
        m_repository.setSetting(QString::fromLatin1(kPrivacyModeSetting), m_privacyMode);
    }
    m_captureFullDesktop = m_privacyMode == QString::fromLatin1(kPrivacyFullDesktop);
    m_autoRecordEnabled = m_repository.setting(QString::fromLatin1(kAutoRecordSetting),
                                               QStringLiteral("0")) == QStringLiteral("1");
    m_lastGame = m_repository.setting(QString::fromLatin1(kLastGameSetting));

    m_auth.configure(m_config.googleClientId(), m_config.googleClientSecret());
    m_session.setWorkerUrl(m_config.workerUrl());
    applyRecorderSettings();

    // Match Streamlabs/OBS lifecycle: initialize the private OBS runtime while
    // VodLink starts, not at the exact moment a game is detected. If OBS cannot
    // initialize, fail early with a normal error dialog instead of crashing later
    // in the game-start path.
    if (!m_streamer.warmUp(error)) {
        return false;
    }

    const QString refreshToken = m_repository.setting(QString::fromLatin1(kRefreshTokenSetting));
    if (!refreshToken.isEmpty()) {
        m_explicitlySignedOut = false;
        m_auth.restore(refreshToken);
    }
    return true;
}

void AppController::startMonitoring()
{
    m_detector.start();
    emit statusChanged(QStringLiteral("Watching for games"), false);
}

QVector<Vod> AppController::vods(const QString &game, QString *error) const
{
    return m_repository.list(game, error);
}

QVector<Vod> AppController::libraryVods(const QString &game, QString *error) const
{
    QVector<Vod> result = m_repository.list(game, error);
    if (error != nullptr && !error->isEmpty()) {
        return result;
    }
    QString friendError;
    QVector<Vod> friends = m_repository.friendVods(game, &friendError);
    if (!friendError.isEmpty()) {
        if (error != nullptr) {
            *error = friendError;
        }
        return result;
    }
    result += friends;
    std::sort(result.begin(), result.end(), [](const Vod &a, const Vod &b) {
        return a.startedAt > b.startedAt;
    });
    return result;
}

QVector<Vod> AppController::sessionVods(const QString &game, QString *error) const
{
    return m_repository.sessionVods(game, error);
}

QStringList AppController::games(QString *error) const
{
    return m_repository.games(error);
}

QVector<VodClip> AppController::clipsForVod(const QString &youtubeId, QString *error) const
{
    return m_repository.clipsForVod(youtubeId, error);
}

bool AppController::addClipForVod(const QString &youtubeId, const QString &title,
                                  int startSeconds, int endSeconds, QString *error)
{
    VodClip clip;
    clip.youtubeId = youtubeId;
    clip.title = title;
    clip.startSeconds = startSeconds;
    clip.endSeconds = endSeconds;
    return m_repository.addClip(clip, error);
}

void AppController::importClipForVod(const QString &clipUrl, const Vod &vod,
                                     const QString &title, int startSeconds, int endSeconds)
{
    if (vod.youtubeId.trimmed().isEmpty()) {
        emit errorOccurred(QStringLiteral("Select a VOD before importing a YouTube Clip."));
        return;
    }
    if (!canAttachClipsToVod(vod)) {
        emit errorOccurred(QStringLiteral("Only the signed-in YouTube owner can attach clips to this VOD."));
        return;
    }
    m_pendingClipVodByVideoId.insert(vod.youtubeId.trimmed(), vod);
    emit statusChanged(QStringLiteral("Importing YouTube Clip…"), false);
    m_youtube.importClipUrl(clipUrl, vod.youtubeId, title, startSeconds, endSeconds);
}

bool AppController::isRecording() const
{
    return m_streamState != StreamState::Idle;
}

bool AppController::isLiveConfirmed() const
{
    return m_youtubeLiveConfirmed;
}

QString AppController::currentGame() const
{
    return m_currentGame;
}

QString AppController::lastGame() const
{
    return m_lastGame;
}

bool AppController::isAuthConfigured() const
{
    return m_auth.isConfigured();
}

bool AppController::isSignedIn() const
{
    return m_auth.isSignedIn();
}

QString AppController::accountEmail() const
{
    return m_auth.email();
}

QString AppController::accountName() const
{
    return m_auth.displayName();
}

QString AppController::accountPictureUrl() const
{
    return m_auth.pictureUrl();
}

bool AppController::canDeleteVod(const Vod &vod) const
{
    if (!vod.ownerEmail.trimmed().isEmpty()) {
        return false;
    }
    const QString current = normalizedEmail(m_auth.email());
    const QString owner = normalizedEmail(vod.accountEmail);
    return !current.isEmpty() && !owner.isEmpty() && current == owner;
}

bool AppController::canAttachClipsToVod(const Vod &vod) const
{
    return canDeleteVod(vod);
}

void AppController::ensureVodEmbeddable(const Vod &vod)
{
    if (!canDeleteVod(vod) || vod.youtubeId.trimmed().isEmpty()) {
        return;
    }
    m_youtube.ensureVodEmbeddable(vod.youtubeId);
}

QStringList AppController::friends(QString *error) const
{
    return m_repository.friends(error);
}

QVector<AccountProfile> AppController::friendProfiles(QString *error) const
{
    return m_repository.friendProfiles(error);
}

bool AppController::shareEnabled() const
{
    return m_shareEnabled;
}

QString AppController::privacyMode() const
{
    return normalizedPrivacyMode(m_privacyMode);
}

bool AppController::captureFullDesktop() const
{
    return privacyMode() == QString::fromLatin1(kPrivacyFullDesktop);
}

bool AppController::autoRecordEnabled() const
{
    return m_autoRecordEnabled;
}

bool AppController::hasStoredCredentials() const
{
    return !m_explicitlySignedOut
           && !m_repository.setting(QString::fromLatin1(kRefreshTokenSetting)).isEmpty();
}

bool AppController::isWorkerConfigured() const
{
    return !m_config.workerUrl().isEmpty();
}

QString AppController::appSetting(const QString &key, const QString &defaultValue) const
{
    return m_repository.setting(key, defaultValue);
}

void AppController::setAppSetting(const QString &key, const QString &value)
{
    m_repository.setSetting(key, value);
    if (key == QString::fromLatin1(kEncoderSetting)
        || key == QString::fromLatin1(kBitrateSetting)
        || key == QString::fromLatin1(kResolutionSetting)
        || key == QString::fromLatin1(kFpsSetting)) {
        applyRecorderSettings();
    }
}

QStringList AppController::runningProcesses() const
{
    QString error;
    return m_detector.runningProcesses(&error);
}

bool AppController::shutdownForQuit()
{
    if (m_shutdownStarted) {
        return false;
    }
    m_shutdownStarted = true;
    return finishActiveStreamBeforeAccountDisconnect(QStringLiteral("Closing active stream before quitting…"));
}

bool AppController::factoryResetLocalData(QString *error)
{
    const bool activeCleanup = finishActiveStreamBeforeAccountDisconnect(
        QStringLiteral("Resetting VodLink and deleting local data…"));

    m_detector.stop();
    m_explicitlySignedOut = true;
    m_eligibilityChecked = false;
    m_librarySyncStarted = false;
    m_librarySyncActive = false;
    m_pendingClipVodByVideoId.clear();

    // Let GoogleAuth clear in-memory tokens and emit the normal sign-out state
    // before the settings database disappears.
    m_auth.signOut();

    // OBS DLLs/resources live under AppData on Windows. Because the runtime is
    // now intentionally kept alive between sessions, release it before deleting
    // the VodLink data root; otherwise Windows can keep files locked.
    m_streamer.shutdownRuntime();

    if (!m_repository.deleteAllLocalData(error)) {
        return activeCleanup;
    }

    emit statusChanged(QStringLiteral("VodLink local data reset. Closing…"), false);
    return activeCleanup;
}

void AppController::signIn()
{
    m_explicitlySignedOut = false;
    m_auth.signIn();
}

void AppController::signOut()
{
    // Keep the current token alive long enough to stop RTMP, transition the
    // YouTube broadcast, and send the Worker /stop request for friends. Only then
    // clear credentials.
    finishActiveStreamBeforeAccountDisconnect(QStringLiteral("Stopping stream before signing out…"));

    m_explicitlySignedOut = true;
    m_eligibilityChecked = false;
    m_librarySyncStarted = false;
    m_librarySyncActive = false;
    m_pendingClipVodByVideoId.clear();
    // Clear persisted credentials immediately so the UI can leave the main page
    // and a later app launch cannot silently restore the old account.
    m_repository.setSetting(QString::fromLatin1(kRefreshTokenSetting), QString());
    m_auth.signOut();
    emit accountChanged(QString());
}

void AppController::addFriend(const QString &email)
{
    const QString trimmed = email.trimmed().toLower();
    if (trimmed.isEmpty() || !trimmed.contains(u'@')) {
        emit errorOccurred(QStringLiteral("Enter a valid email address to add a friend."));
        return;
    }
    QString error;
    if (!m_repository.addFriend(trimmed, &error)) {
        emit errorOccurred(error);
        return;
    }
    emit friendsChanged();
}

void AppController::removeFriend(const QString &email)
{
    QString error;
    if (!m_repository.removeFriend(email, &error)) {
        emit errorOccurred(error);
        return;
    }
    emit friendsChanged();
    emit libraryChanged();
}

void AppController::setShareEnabled(bool enabled)
{
    if (m_shareEnabled == enabled) {
        return;
    }
    m_shareEnabled = enabled;
    m_repository.setSetting(QString::fromLatin1(kShareSetting),
                            enabled ? QStringLiteral("1") : QStringLiteral("0"));
    emit shareEnabledChanged(enabled);
}

void AppController::setAutoRecordEnabled(bool enabled)
{
    if (m_autoRecordEnabled == enabled) {
        return;
    }

    const bool wasRecording = isRecording();
    m_autoRecordEnabled = enabled;
    m_repository.setSetting(QString::fromLatin1(kAutoRecordSetting),
                            enabled ? QStringLiteral("1") : QStringLiteral("0"));
    emit autoRecordEnabledChanged(enabled);

    if (!enabled && wasRecording) {
        stopRecording();
        return;
    }

    emit statusChanged(enabled ? QStringLiteral("Auto-recording enabled")
                               : QStringLiteral("Auto-recording paused"),
                       m_youtubeLiveConfirmed);
}

void AppController::setPrivacyMode(const QString &mode)
{
    const QString normalized = normalizedPrivacyMode(mode);
    if (m_privacyMode == normalized) {
        return;
    }
    m_privacyMode = normalized;
    m_captureFullDesktop = normalized == QString::fromLatin1(kPrivacyFullDesktop);
    m_repository.setSetting(QString::fromLatin1(kPrivacyModeSetting), normalized);
    m_repository.setSetting(QString::fromLatin1(kFullDesktopSetting),
                            m_captureFullDesktop ? QStringLiteral("1") : QStringLiteral("0"));
}

void AppController::setCaptureFullDesktop(bool enabled)
{
    setPrivacyMode(enabled ? QString::fromLatin1(kPrivacyFullDesktop)
                           : QString::fromLatin1(kPrivacyGameOnly));
}

void AppController::applyRecorderSettings()
{
    const QString encoder = m_repository.setting(QString::fromLatin1(kEncoderSetting), QStringLiteral("H.264"));
    bool ok = false;

    const QString resolution = m_repository.setting(QString::fromLatin1(kResolutionSetting), nativeRecorderResolutionText());
    const QSize outputSize = parseRecorderResolution(resolution);

    int fps = m_repository.setting(QString::fromLatin1(kFpsSetting), QStringLiteral("60")).toInt(&ok);
    if (!ok || (fps != 30 && fps != 60)) {
        fps = 60;
    }

    // Do not let stale installs keep the old 12 Mbps default at high
    // resolutions. Stream start normalizes to YouTube's current recommended
    // ingest bitrate for the selected resolution/framerate/codec.
    const int bitrate = youtubeRecommendedBitrateKbps(outputSize, fps, encoder);
    if (m_repository.setting(QString::fromLatin1(kBitrateSetting), QString()).toInt(&ok) != bitrate || !ok) {
        m_repository.setSetting(QString::fromLatin1(kBitrateSetting), QString::number(bitrate));
    }

    m_streamer.setRecorderSettings(encoder, bitrate, outputSize, fps);
    m_youtube.setStreamSettings(QStringLiteral("%1x%2").arg(outputSize.width()).arg(outputSize.height()), fps);
}

void AppController::addUserGame(const QString &executable, const QString &name)
{
    const QString displayName = name.trimmed();
    if (executable.trimmed().isEmpty() || displayName.isEmpty()) {
        return;
    }

    QFileInfo file(executable.trimmed());
    QString fileName = file.fileName().trimmed().toLower();
    QString fullPath = file.absoluteFilePath().trimmed().toLower();
    fullPath.replace(QLatin1Char('\\'), QLatin1Char('/'));

    if (fileName.isEmpty()) {
        fileName = executable.trimmed().toLower();
    }

    QString fileNameNoExe = fileName;
    if (fileNameNoExe.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)
        || fileNameNoExe.endsWith(QStringLiteral(".app"), Qt::CaseInsensitive)) {
        fileNameNoExe.chop(4);
    }

    QString error;
    const auto saveEntry = [this, &error, &displayName](const QString &key) {
        const QString normalized = key.trimmed().toLower();
        if (normalized.isEmpty()) {
            return true;
        }
        if (!m_repository.addUserGame(normalized, displayName, &error)) {
            return false;
        }
        m_catalog.addUserEntry(normalized, displayName);
        return true;
    };

    // Store both the exact path and the basename. The exact path prevents false
    // matches when two games ship with the same helper executable; the basename
    // keeps detection working when OS process enumeration only exposes fileName().
    if (!saveEntry(fullPath) || !saveEntry(fileName) || !saveEntry(fileNameNoExe)) {
        emit errorOccurred(error);
        return;
    }

    emit statusChanged(QStringLiteral("Added %1 — VodLink will capture it next time").arg(displayName),
                       m_youtubeLiveConfirmed);
}

void AppController::deleteVod(const Vod &vod)
{
    const QString videoId = vod.youtubeId.trimmed();
    if (videoId.isEmpty()) {
        emit errorOccurred(QStringLiteral("This VOD has no YouTube video id."));
        return;
    }
    if (!canDeleteVod(vod)) {
        emit errorOccurred(QStringLiteral("This VOD belongs to another Google account, so VodLink will not delete it."));
        return;
    }

    QString error;
    if (!m_repository.hasOwnVod(videoId, &error)) {
        emit errorOccurred(error.isEmpty()
                               ? QStringLiteral("This VOD is not in your local YouTube VOD library.")
                               : error);
        return;
    }
    emit statusChanged(QStringLiteral("Deleting VOD from YouTube…"), false);
    m_youtube.deleteVideo(videoId);
}

void AppController::removeFriendVodFromLibrary(const Vod &vod)
{
    if (vod.youtubeId.trimmed().isEmpty()) {
        return;
    }
    QString error;
    if (!m_repository.removeFriendVod(vod.youtubeId, &error)) {
        emit errorOccurred(error);
        return;
    }
    emit vodDeleted(vod.youtubeId);
    emit libraryChanged();
}

void AppController::stopRecording()
{
    DebugLog::writeCategory(QStringLiteral("recording"),
                            QStringLiteral("stopRecording state=%1 streaming=%2 broadcast=%3 stream=%4")
                                .arg(static_cast<int>(m_streamState))
                                .arg(m_streamer.isStreaming() ? QStringLiteral("true") : QStringLiteral("false"),
                                     m_broadcastId, m_streamId));
    if (m_streamState == StreamState::Preparing
        || m_streamState == StreamState::WaitingForYouTube
        || m_streamState == StreamState::Streaming) {
        m_cancelRequested = true;
        m_streamState = StreamState::Stopping;
        m_youtube.stopStreamStatusPolling(m_broadcastId, m_streamId);
        emit statusChanged(m_youtubeLiveConfirmed
                               ? QStringLiteral("Finishing YouTube stream…")
                               : QStringLiteral("Cancelling stream startup…"),
                           m_youtubeLiveConfirmed);
        if (m_streamer.isStreaming()) {
            m_streamer.stop();
        } else if (!m_broadcastId.isEmpty()) {
            m_discardingBroadcastId = m_broadcastId;
            m_youtube.deleteVideo(m_broadcastId);
            resetStreamState();
        }
    }
}

void AppController::onGameStarted(const GameDefinition &game)
{
    DebugLog::writeCategory(QStringLiteral("game"),
                            QStringLiteral("started name=%1 processes=[%2] state=%3 autoRecord=%4 privacy=%5")
                                .arg(game.name, game.processNames.join(QStringLiteral(", ")))
                                .arg(static_cast<int>(m_streamState))
                                .arg(m_autoRecordEnabled ? QStringLiteral("true") : QStringLiteral("false"), privacyMode()));
    if (m_streamState != StreamState::Idle) {
        return;
    }
    if (m_lastGame != game.name) {
        m_lastGame = game.name;
        m_repository.setSetting(QString::fromLatin1(kLastGameSetting), m_lastGame);
        emit lastGameChanged(m_lastGame);
    }
    if (!m_autoRecordEnabled) {
        emit statusChanged(QStringLiteral("Auto-recording is off — %1 not captured").arg(game.name), false);
        return;
    }
    m_currentGame = game.name;
    m_currentGameDefinition = game;
    m_streamState = StreamState::Preparing;
    m_cancelRequested = false;
    m_encoderStarted = false;
    m_youtubeLiveConfirmed = false;
    m_streamId.clear();
    m_lastYouTubeWaitDetail.clear();
    m_lastLocalStreamDetail.clear();
    emit statusChanged(QStringLiteral("Creating YouTube broadcast for %1…").arg(game.name), false);
    m_youtube.prepareBroadcast(game.name);
}

void AppController::onGameStopped(const GameDefinition &game)
{
    DebugLog::writeCategory(QStringLiteral("game"), QStringLiteral("stopped name=%1 current=%2").arg(game.name, m_currentGame));
    if (game.name == m_currentGame) {
        stopRecording();
    }
}

void AppController::onBroadcastReady(const QString &broadcastId, const QString &streamId, const QUrl &ingestUrl)
{
    DebugLog::writeCategory(QStringLiteral("youtube"),
                            QStringLiteral("broadcastReady broadcast=%1 stream=%2 ingestHost=%3 state=%4 cancel=%5")
                                .arg(broadcastId, streamId, ingestUrl.host())
                                .arg(static_cast<int>(m_streamState))
                                .arg(m_cancelRequested ? QStringLiteral("true") : QStringLiteral("false")));
    if (m_streamState != StreamState::Preparing || m_cancelRequested) {
        m_broadcastId = broadcastId;
        m_discardingBroadcastId = broadcastId;
        m_youtube.deleteVideo(broadcastId);
        resetStreamState();
        return;
    }
    m_broadcastId = broadcastId;
    m_streamId = streamId;
    m_startedAt = QDateTime::currentDateTimeUtc();
    QString error;
    QStringList windowHints = m_currentGameDefinition.processNames;
    windowHints.prepend(m_currentGameDefinition.name);
    const QString privacy = privacyMode();
    const auto captureMode = privacy == QString::fromLatin1(kPrivacyFullDesktop)
                                 ? CaptureMode::FullDesktop
                                 : CaptureMode::GameWindow;
    const auto audioSource = privacy == QString::fromLatin1(kPrivacyGameOnly)
                                 ? AudioCaptureSource::GameOnly
                                 : AudioCaptureSource::System;
    if (!m_streamer.start(ingestUrl, captureMode, audioSource, windowHints, &error)) {
        emit errorOccurred(error);
        m_discardingBroadcastId = m_broadcastId;
        m_youtube.deleteVideo(m_broadcastId);
        resetStreamState();
        return;
    }
    if (!m_youtubeLiveConfirmed) {
        m_streamState = StreamState::Streaming;
        m_youtubeLiveConfirmed = true;
        m_lastYouTubeWaitDetail.clear();
        emit statusChanged(QStringLiteral("Streaming %1 to YouTube").arg(m_currentGame), true);
        announceFriendSessionIfReady();
    }
}

void AppController::onEncoderStopped()
{
    DebugLog::writeCategory(QStringLiteral("recording"),
                            QStringLiteral("encoder stopped state=%1 broadcast=%2 encoderStarted=%3 liveConfirmed=%4")
                                .arg(static_cast<int>(m_streamState), 0, 10)
                                .arg(m_broadcastId,
                                     m_encoderStarted ? QStringLiteral("true") : QStringLiteral("false"),
                                     m_youtubeLiveConfirmed ? QStringLiteral("true") : QStringLiteral("false")));
    m_youtube.stopStreamStatusPolling(m_broadcastId, m_streamId);
    if (m_broadcastId.isEmpty()) {
        resetStreamState();
        return;
    }
    if (!m_encoderStarted || !m_youtubeLiveConfirmed) {
        m_discardingBroadcastId = m_broadcastId;
        m_youtube.deleteVideo(m_broadcastId);
        resetStreamState();
        return;
    }
    const qint64 duration = m_startedAt.msecsTo(QDateTime::currentDateTimeUtc());
    Vod vod;
    vod.accountEmail = normalizedEmail(m_auth.email());
    vod.game = m_currentGame;
    vod.youtubeId = m_broadcastId;
    vod.streamStatus = QStringLiteral("processing");
    vod.startedAt = m_startedAt;
    vod.durationMs = duration;

    QString error;
    if (!m_repository.upsertOwnVod(vod, &error)) {
        emit errorOccurred(QStringLiteral("YouTube kept the VOD, but cataloging failed: %1").arg(error));
    } else {
        emit libraryChanged();
        m_youtube.updateVodLinkMetadata(vod, {});
        m_youtube.ensureVodEmbeddable(vod.youtubeId);
    }
    m_youtube.completeBroadcast(m_broadcastId);

    // Ask the Worker which friends streamed the same game during this session.
    finishFriendSessionIfNeeded();

    m_streamState = StreamState::Stopping;
    emit statusChanged(QStringLiteral("Finalizing YouTube VOD…"), true);
}

bool AppController::finishActiveStreamBeforeAccountDisconnect(const QString &statusMessage)
{
    if (m_streamState == StreamState::Idle && !m_streamer.isStreaming() && m_broadcastId.isEmpty()) {
        return false;
    }

    if (!statusMessage.isEmpty()) {
        emit statusChanged(statusMessage, m_youtubeLiveConfirmed);
    }
    m_cancelRequested = true;
    m_youtube.stopStreamStatusPolling(m_broadcastId, m_streamId);

    if (m_streamer.isStreaming()) {
        // RtmpStreamer::stop() emits stopped() synchronously in the GUI thread, so
        // onEncoderStopped() gets to catalog the VOD, transition YouTube, and send
        // the Worker /stop request before signOut() clears auth tokens.
        if (m_streamState == StreamState::Idle) {
            m_streamState = StreamState::Stopping;
        }
        m_streamer.stop();
        return true;
    }

    // We may have created a YouTube broadcast but not reached RTMP yet. It has
    // never gone live, so completing it can produce YouTube's "Invalid transition".
    // Delete the unused scheduled broadcast instead.
    if (!m_broadcastId.isEmpty()) {
        finishFriendSessionIfNeeded();
        m_discardingBroadcastId = m_broadcastId;
        m_youtube.deleteVideo(m_broadcastId);
    }
    resetStreamState();
    return true;
}

void AppController::finishFriendSessionIfNeeded()
{
    if (!m_friendSessionAnnounced) {
        return;
    }
    m_friendSessionAnnounced = false;
    m_session.requestStopMatches();
}

void AppController::adoptSignedInAccount(const QString &email)
{
    const QString normalized = email.trimmed().toLower();
    if (normalized.isEmpty()) {
        return;
    }

    const QString previous = m_repository.setting(QString::fromLatin1(kAccountEmailSetting)).trimmed().toLower();
    if (!previous.isEmpty() && previous != normalized) {
        m_librarySyncStarted = false;
        m_pendingClipVodByVideoId.clear();
        emit statusChanged(QStringLiteral("Switched Google account — keeping cached VODs. Use Settings to sync YouTube VODs when needed."), false);
        emit libraryChanged();
    }

    m_repository.setSetting(QString::fromLatin1(kAccountEmailSetting), normalized);
}

void AppController::syncOwnLibrary()
{
    DebugLog::writeCategory(QStringLiteral("youtube"), QStringLiteral("manual sync requested"));
    startOwnLibrarySync();
}

void AppController::startOwnLibrarySync()
{
    if (m_librarySyncActive) {
        emit statusChanged(QStringLiteral("YouTube VOD sync is already running…"), false);
        return;
    }
    if (m_streamState != StreamState::Idle || m_streamer.isStreaming()) {
        emit errorOccurred(QStringLiteral("Stop the current recording before syncing YouTube VODs."));
        return;
    }
    if (m_explicitlySignedOut || m_auth.accessToken().isEmpty() || m_auth.email().isEmpty()) {
        emit errorOccurred(QStringLiteral("Sign in with Google before syncing YouTube VODs."));
        return;
    }

    m_librarySyncStarted = true;
    m_librarySyncActive = true;
    m_repository.setSetting(QString::fromLatin1(kYouTubeLibraryLastSyncMsSetting),
                            QString::number(QDateTime::currentMSecsSinceEpoch()));
    emit statusChanged(QStringLiteral("Syncing latest YouTube VODs…"), false);
    m_youtube.syncOwnLibrary();
}

Vod AppController::ownVodByYoutubeId(const QString &youtubeId) const
{
    QString error;
    const QVector<Vod> vods = m_repository.list(QString(), &error);
    if (!error.isEmpty()) {
        return {};
    }
    const QString id = youtubeId.trimmed();
    for (const Vod &vod : vods) {
        if (vod.youtubeId == id && vod.isMine()) {
            return vod;
        }
    }
    return {};
}

void AppController::onOwnLibrarySynced(const QVector<Vod> &vods,
                                       const QHash<QString, QVector<VodClip>> &clipsByVod)
{
    m_librarySyncActive = false;
    m_librarySyncStarted = false;
    bool changed = false;
    const QString ownerEmail = normalizedEmail(m_auth.email());
    for (Vod vod : vods) {
        vod.accountEmail = ownerEmail;
        QString error;
        if (m_repository.upsertOwnVod(vod, &error)) {
            changed = true;
        } else {
            emit errorOccurred(QStringLiteral("YouTube VOD sync failed: %1").arg(error));
        }

        const QVector<VodClip> clips = clipsByVod.value(vod.youtubeId);
        if (!m_repository.replaceClipsForVod(vod.youtubeId, clips, &error)) {
            emit errorOccurred(QStringLiteral("YouTube clip sync failed: %1").arg(error));
        } else if (!clips.isEmpty()) {
            changed = true;
        }
    }

    if (changed) {
        emit libraryChanged();
    }
    emit statusChanged(QStringLiteral("YouTube VOD library synced"), false);
}

void AppController::onClipImported(const VodClip &clip)
{
    const QString videoId = clip.youtubeId.trimmed();
    if (videoId.isEmpty()) {
        emit errorOccurred(QStringLiteral("The copied YouTube Clip did not expose its parent VOD."));
        return;
    }

    if (!m_pendingClipVodByVideoId.isEmpty() && !m_pendingClipVodByVideoId.contains(videoId)) {
        emit errorOccurred(QStringLiteral("That YouTube Clip belongs to a different VOD."));
        return;
    }

    Vod vod = m_pendingClipVodByVideoId.value(videoId);
    if (vod.youtubeId.isEmpty()) {
        vod = ownVodByYoutubeId(videoId);
    }
    if (vod.youtubeId.isEmpty()) {
        emit errorOccurred(QStringLiteral("This clip belongs to a VOD that is not in your local VodLink library yet."));
        return;
    }
    if (!canAttachClipsToVod(vod)) {
        emit errorOccurred(QStringLiteral("This VOD belongs to another Google account, so VodLink will not attach clips to it."));
        return;
    }

    QString error;
    if (!m_repository.addClip(clip, &error)) {
        emit errorOccurred(QStringLiteral("Could not save YouTube Clip: %1").arg(error));
        return;
    }

    const QVector<VodClip> clips = m_repository.clipsForVod(videoId, &error);
    if (!error.isEmpty()) {
        emit errorOccurred(error);
        return;
    }
    m_youtube.updateVodLinkMetadata(vod, clips);

    emit clipImported(videoId);
    emit libraryChanged();
    emit statusChanged(QStringLiteral("Imported YouTube Clip for %1").arg(vod.game), false);
}

void AppController::updateWaitingStatus()
{
    if (m_streamState != StreamState::WaitingForYouTube || m_youtubeLiveConfirmed) {
        return;
    }

    QStringList details;
    if (!m_lastYouTubeWaitDetail.trimmed().isEmpty()) {
        details << QStringLiteral("YouTube: %1").arg(m_lastYouTubeWaitDetail.trimmed());
    }
    if (!m_lastLocalStreamDetail.trimmed().isEmpty()) {
        details << m_lastLocalStreamDetail.trimmed();
    }

    const QString suffix = details.isEmpty()
                               ? QString()
                               : QStringLiteral(" — %1").arg(details.join(QStringLiteral(" | ")));
    emit statusChanged(QStringLiteral("Waiting for YouTube to receive stream%1").arg(suffix), false);
}

void AppController::resetStreamState()
{
    DebugLog::writeCategory(QStringLiteral("recording"),
                            QStringLiteral("resetStreamState previousState=%1 broadcast=%2 stream=%3 currentGame=%4")
                                .arg(static_cast<int>(m_streamState))
                                .arg(m_broadcastId, m_streamId, m_currentGame));
    m_youtube.stopStreamStatusPolling(m_broadcastId, m_streamId);
    m_streamState = StreamState::Idle;
    m_cancelRequested = false;
    m_encoderStarted = false;
    m_youtubeLiveConfirmed = false;
    m_broadcastId.clear();
    m_streamId.clear();
    m_lastYouTubeWaitDetail.clear();
    m_lastLocalStreamDetail.clear();
    m_friendSessionAnnounced = false;
    m_currentGame.clear();
    m_currentGameDefinition = {};
    m_startedAt = {};
    emit statusChanged(QStringLiteral("Watching for games"), false);
}

void AppController::announceFriendSessionIfReady()
{
    // Announce after the local RTMP encoder starts. VodLink intentionally avoids
    // constant YouTube stream-health polling to preserve Data API quota.
    if (m_friendSessionAnnounced || !m_youtubeLiveConfirmed
        || m_broadcastId.isEmpty() || !m_shareEnabled || !m_session.isReady()) {
        return;
    }
    m_friendSessionAnnounced = true;
    m_session.announceStart(m_currentGame, m_broadcastId, m_startedAt,
                            m_repository.friends(nullptr));
}

void AppController::onYouTubeStreamStatus(const QString &broadcastId, const QString &streamId,
                                          const QString &streamStatus, const QString &healthStatus,
                                          const QString &configurationIssues)
{
    if (broadcastId != m_broadcastId || streamId != m_streamId
        || m_streamState == StreamState::Idle || m_streamState == StreamState::Stopping) {
        return;
    }

    const QString normalized = streamStatus.trimmed().toLower();
    if (normalized == QStringLiteral("active")) {
        if (!m_youtubeLiveConfirmed || m_streamState != StreamState::Streaming) {
            m_youtubeLiveConfirmed = true;
            m_streamState = StreamState::Streaming;
            m_lastYouTubeWaitDetail.clear();
            emit statusChanged(QStringLiteral("Streaming %1 to YouTube").arg(m_currentGame), true);
            announceFriendSessionIfReady();
        } else if (!configurationIssues.trimmed().isEmpty()) {
            emit statusChanged(QStringLiteral("Streaming %1 to YouTube — %2").arg(m_currentGame, configurationIssues), true);
        }
        return;
    }

    m_youtubeLiveConfirmed = false;
    m_streamState = StreamState::WaitingForYouTube;
    QString detail = normalized.isEmpty() ? QStringLiteral("unknown") : normalized;
    if (!healthStatus.trimmed().isEmpty()) {
        detail += QStringLiteral("/") + healthStatus.trimmed();
    }
    if (!configurationIssues.trimmed().isEmpty()) {
        detail += QStringLiteral(" — ") + configurationIssues.trimmed();
    }
    m_lastYouTubeWaitDetail = detail;
    updateWaitingStatus();
}

void AppController::onYouTubeStreamStatusProbeFailed(const QString &broadcastId, const QString &streamId,
                                                     const QString &message)
{
    if (broadcastId != m_broadcastId || streamId != m_streamId
        || m_streamState == StreamState::Idle || m_streamState == StreamState::Stopping) {
        return;
    }
    m_youtubeLiveConfirmed = false;
    m_lastYouTubeWaitDetail = message.trimmed().isEmpty()
                                   ? QStringLiteral("stream-status check pending")
                                   : QStringLiteral("status check failed: %1").arg(message.trimmed());
    updateWaitingStatus();
}

void AppController::onMatchesReceived(const QVector<Vod> &friendVods)
{
    if (friendVods.isEmpty()) {
        return;
    }
    bool changed = false;
    for (const Vod &vod : friendVods) {
        QString error;
        if (m_repository.upsertFriendVod(vod, &error)) {
            changed = true;
        } else {
            emit errorOccurred(error);
        }
    }
    if (changed) {
        emit libraryChanged();
        // A match may have taught us a mutual friend's name/picture; refresh the
        // friends panel so it upgrades from the email + letter badge.
        emit friendsChanged();
        emit statusChanged(QStringLiteral("Received %1 friend VOD(s) from this session")
                               .arg(friendVods.size()),
                           false);
    }
}
