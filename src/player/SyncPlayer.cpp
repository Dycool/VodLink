#include "SyncPlayer.h"

#include "PlayerBridge.h"

#include "app/AppPaths.h"
#include "app/DebugLog.h"

#include <QFile>
#include <QIODevice>
#include <QUrl>
#include <QTimer>
#include <QDir>
#include <QStandardPaths>
#include <QStringList>
#include <QVBoxLayout>
#include <QWebChannel>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineView>

#include <algorithm>


namespace {
constexpr auto kPlayerOrigin = "https://vodlink.app/";

class PlayerRequestInterceptor final : public QWebEngineUrlRequestInterceptor
{
public:
    using QWebEngineUrlRequestInterceptor::QWebEngineUrlRequestInterceptor;

    void interceptRequest(QWebEngineUrlRequestInfo &info) override
    {
        const QUrl url = info.requestUrl();
        const QString host = url.host().toLower();
        if (host.endsWith(QStringLiteral("youtube.com"))
            || host.endsWith(QStringLiteral("youtube-nocookie.com"))
            || host.endsWith(QStringLiteral("ytimg.com"))
            || host.endsWith(QStringLiteral("googlevideo.com"))) {
            info.setHttpHeader(QByteArrayLiteral("Referer"), QByteArray(kPlayerOrigin));
            DebugLog::writeCategory(QStringLiteral("LiteYouTube/Request"),
                                    QStringLiteral("%1 %2")
                                        .arg(QString::fromLatin1(info.requestMethod()), url.toString(QUrl::RemoveQuery)));
        }
    }
};

class PlayerPage final : public QWebEnginePage
{
public:
    explicit PlayerPage(QWebEngineProfile *profile, QObject *parent = nullptr)
        : QWebEnginePage(profile, parent)
    {
    }

protected:
    void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel level, const QString &message,
                                  int lineNumber, const QString &sourceId) override
    {
        QString levelName = QStringLiteral("log");
        switch (level) {
        case InfoMessageLevel:
            levelName = QStringLiteral("info");
            break;
        case WarningMessageLevel:
            levelName = QStringLiteral("warning");
            break;
        case ErrorMessageLevel:
            levelName = QStringLiteral("error");
            break;
        }
        DebugLog::writeCategory(QStringLiteral("LiteYouTube/Console"),
                                QStringLiteral("%1:%2 [%3] %4")
                                    .arg(sourceId.isEmpty() ? QStringLiteral("<inline>") : sourceId)
                                    .arg(lineNumber)
                                    .arg(levelName, message));
        QWebEnginePage::javaScriptConsoleMessage(level, message, lineNumber, sourceId);
    }
};
}

SyncPlayer::SyncPlayer(QWidget *parent)
    : QWidget(parent),
      m_profile(new QWebEngineProfile(QStringLiteral("vodlink-player"), this)),
      m_view(new QWebEngineView(this)),
      m_channel(new QWebChannel(this)),
      m_bridge(new PlayerBridge(this))
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_view);

    // Keep the player page alive and let Chromium cache YouTube resources in RAM.
    // This avoids destroying/recreating the web view on every participant switch.
    m_profile->setHttpCacheType(QWebEngineProfile::MemoryHttpCache);
    m_profile->setHttpCacheMaximumSize(256 * 1024 * 1024);
    m_profile->setPersistentCookiesPolicy(QWebEngineProfile::NoPersistentCookies);
    m_profile->setHttpUserAgent(QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0 Safari/537.36 VodLink/1.0"));
    const QString cacheRoot = AppPaths::cacheRoot();
    if (!cacheRoot.isEmpty()) {
        const QString playerCache = QDir(cacheRoot).filePath(QStringLiteral("webengine-player"));
        QDir().mkpath(playerCache);
        m_profile->setPersistentStoragePath(playerCache);
    }
    m_profile->setUrlRequestInterceptor(new PlayerRequestInterceptor(m_profile));
    auto *page = new PlayerPage(m_profile, m_view);
    m_view->setPage(page);
    DebugLog::writeCategory(QStringLiteral("LiteYouTube"),
                            QStringLiteral("QWebEngine profile created origin=%1 cacheRoot=%2")
                                .arg(QString::fromLatin1(kPlayerOrigin), cacheRoot));
    connect(page, &QWebEnginePage::loadStarted, this, [] {
        DebugLog::writeCategory(QStringLiteral("LiteYouTube"), QStringLiteral("player page load started"));
    });
    connect(page, &QWebEnginePage::loadFinished, this, [](bool ok) {
        DebugLog::writeCategory(QStringLiteral("LiteYouTube"),
                                QStringLiteral("player page load finished ok=%1").arg(ok));
    });
    connect(page, &QWebEnginePage::renderProcessTerminated, this,
            [](QWebEnginePage::RenderProcessTerminationStatus status, int exitCode) {
                DebugLog::writeCategory(QStringLiteral("LiteYouTube"),
                                        QStringLiteral("Qt WebEngine render process terminated status=%1 exitCode=%2")
                                            .arg(static_cast<int>(status))
                                            .arg(exitCode));
            });
    m_view->settings()->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture, false);
    m_view->settings()->setAttribute(QWebEngineSettings::JavascriptEnabled, true);
    m_view->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, true);
    m_view->settings()->setAttribute(QWebEngineSettings::FullScreenSupportEnabled, true);

    m_channel->registerObject(QStringLiteral("bridge"), m_bridge);
    m_view->page()->setWebChannel(m_channel);

    connect(m_bridge, &PlayerBridge::pageReady, this, &SyncPlayer::onPageReady);
    connect(m_bridge, &PlayerBridge::timeUpdated, this, &SyncPlayer::onTimeUpdate);

    QFile playerHtml(QStringLiteral(":/player/player.html"));
    if (playerHtml.open(QIODevice::ReadOnly)) {
        // YouTube error 153 happens when desktop WebViews load the embed without an
        // HTTP Referer. Loading the bundled HTML with an HTTPS base URL gives the
        // document a stable app origin, and the interceptor reinforces the Referer
        // for YouTube subrequests made by Chromium.
        m_view->setHtml(QString::fromUtf8(playerHtml.readAll()),
                        QUrl(QString::fromLatin1(kPlayerOrigin)));
    } else {
        m_view->setUrl(QUrl(QStringLiteral("qrc:/player/player.html")));
    }
}

void SyncPlayer::setGroup(const QVector<Vod> &vods)
{
    m_group = vods;
    m_current = -1;
    m_currentTime = 0.0;
    m_pendingIndex = -1;
    m_pendingOffsetSeconds = 0.0;

    if (m_pageReady && m_bridge != nullptr) {
        QStringList ids;
        ids.reserve(m_group.size());
        for (const Vod &vod : m_group) {
            const QString id = vod.youtubeId.trimmed();
            if (!id.isEmpty() && !ids.contains(id)) {
                ids.push_back(id);
            }
        }
        m_bridge->cacheVideos(ids);
    }
}


void SyncPlayer::clear()
{
    m_group.clear();
    m_current = -1;
    m_currentTime = 0.0;
    m_pendingIndex = -1;
    m_pendingOffsetSeconds = 0.0;
    if (m_pageReady && m_bridge != nullptr) {
        m_bridge->pause();
        m_bridge->showMessage(QStringLiteral("Select a VOD to watch"));
    }
}

void SyncPlayer::playIndex(int index)
{
    if (index < 0 || index >= m_group.size()) {
        return;
    }

    const Vod &target = m_group.at(index);
    double offsetSeconds = 0.0;
    QString note;

    if (m_current >= 0 && m_current < m_group.size()) {
        // Align to the same real-world instant as the currently playing VOD.
        const qint64 targetStartMs = target.startedAt.toMSecsSinceEpoch();
        const qint64 deltaMs = absolutePositionMs() - targetStartMs;
        offsetSeconds = static_cast<double>(deltaMs) / 1000.0;

        if (offsetSeconds < 0.0) {
            offsetSeconds = 0.0;
            note = QStringLiteral("%1 wasn't recording yet at this moment — starting from their beginning.")
                       .arg(target.isMine() ? QStringLiteral("This VOD") : target.ownerEmail);
        } else if (target.durationMs > 0 && offsetSeconds > target.durationMs / 1000.0) {
            offsetSeconds = target.durationMs / 1000.0;
            note = QStringLiteral("%1 had already stopped recording at this moment.")
                       .arg(target.isMine() ? QStringLiteral("This VOD") : target.ownerEmail);
        }
    }

    playIndexAt(index, offsetSeconds);
    if (!note.isEmpty() && m_pageReady) {
        m_bridge->showMessage(note);
    }
}

void SyncPlayer::playIndexAt(int index, double offsetSeconds)
{
    if (index < 0 || index >= m_group.size()) {
        return;
    }

    const Vod &target = m_group.at(index);
    const double maxSeconds = target.durationMs > 0 ? target.durationMs / 1000.0 : 0.0;
    if (maxSeconds > 0.0) {
        offsetSeconds = std::clamp(offsetSeconds, 0.0, maxSeconds);
    } else {
        offsetSeconds = std::max(0.0, offsetSeconds);
    }

    m_current = index;
    m_currentTime = offsetSeconds;

    if (!m_pageReady) {
        m_pendingIndex = index;
        m_pendingOffsetSeconds = offsetSeconds;
        return;
    }

    loadCurrent(offsetSeconds);
}

void SyncPlayer::loadCurrent(double offsetSeconds, const QString &note)
{
    if (m_current < 0 || m_current >= m_group.size() || m_bridge == nullptr) {
        return;
    }

    const Vod &target = m_group.at(m_current);
    m_bridge->loadVideo(target.youtubeId, offsetSeconds);
    if (!note.isEmpty()) {
        m_bridge->showMessage(note);
    }
    emit currentChanged(m_current);
}

void SyncPlayer::onPageReady()
{
    m_pageReady = true;
    if (m_bridge != nullptr && !m_group.isEmpty()) {
        QStringList ids;
        ids.reserve(m_group.size());
        for (const Vod &vod : m_group) {
            const QString id = vod.youtubeId.trimmed();
            if (!id.isEmpty() && !ids.contains(id)) {
                ids.push_back(id);
            }
        }
        m_bridge->cacheVideos(ids);
    }
    if (m_pendingIndex >= 0) {
        const int index = m_pendingIndex;
        const double offset = m_pendingOffsetSeconds;
        m_pendingIndex = -1;
        m_pendingOffsetSeconds = 0.0;
        playIndexAt(index, offset);
    }
}

void SyncPlayer::onTimeUpdate(double seconds)
{
    m_currentTime = seconds;
}

qint64 SyncPlayer::absolutePositionMs() const
{
    if (m_current < 0 || m_current >= m_group.size()) {
        return 0;
    }
    return m_group.at(m_current).startedAt.toMSecsSinceEpoch()
           + static_cast<qint64>(m_currentTime * 1000.0);
}
