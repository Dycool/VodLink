#include "SyncPlayer.h"

#include "PlayerBridge.h"

#if defined(Q_OS_WIN)
#include "WebView2Player.h"
#else
#include "app/AppPaths.h"
#include "app/DebugLog.h"

#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QUrl>
#include <QWebChannel>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineView>
#endif

#include <QBoxLayout>
#include <QKeyEvent>
#include <QStringList>
#include <QVBoxLayout>
#include <algorithm>


#if !defined(Q_OS_WIN)
namespace {
constexpr auto kPlayerOrigin = "https://vodlink.app/";
constexpr auto kYouTubeFrameCleanupScript = R"JS(
(() => {
    const installVodLinkPlayerStyle = () => {
        let style = document.getElementById('vodlink-youtube-cleanup');
        if (!style) {
            style = document.createElement('style');
            style.id = 'vodlink-youtube-cleanup';
            (document.head || document.documentElement).appendChild(style);
        }
        style.textContent = `
            .ytp-chrome-top,
            .ytp-chrome-top-buttons,
            .ytp-title,
            .ytp-title-text,
            .ytp-title-link,
            .ytp-title-channel,
            .ytp-watch-later-button,
            .ytp-share-button,
            .ytp-youtube-button,
            .ytp-watermark,
            .branding-img-container,
            .ytp-impression-link {
                display: none !important;
                visibility: hidden !important;
                opacity: 0 !important;
                pointer-events: none !important;
            }
        `;
    };

    const installVodLinkVideoBridge = () => {
        if (window.parent === window
            || !/(^|\.)youtube(?:-nocookie)?\.com$/i.test(window.location.hostname)) {
            return;
        }

        const report = () => {
            const video = document.querySelector('video');
            window.parent.postMessage({
                vodlinkVideoFrame: true,
                hasVideo: !!video,
                currentTime: video ? Number(video.currentTime || 0) : 0,
                duration: video ? Number(video.duration || 0) : 0,
                paused: video ? !!video.paused : true,
                ended: video ? !!video.ended : false,
                readyState: video ? Number(video.readyState || 0) : 0,
                playbackRate: video ? Number(video.playbackRate || 1) : 1
            }, '*');
        };

        window.addEventListener('message', event => {
            if (event.source !== window.parent
                || !event.data
                || event.data.vodlinkVideoCommand !== true) {
                return;
            }
            const video = document.querySelector('video');
            if (!video) return;

            const command = String(event.data.command || '');
            if (command === 'play') {
                video.play().catch(() => {});
            } else if (command === 'pause') {
                video.pause();
            } else if (command === 'seek') {
                video.currentTime = Math.max(0, Number(event.data.seconds || 0));
            } else if (command === 'rate') {
                video.playbackRate = 1;
            }
            report();
        });

        document.addEventListener('contextmenu', event => event.preventDefault(), true);
        setInterval(() => {
            const video = document.querySelector('video');
            if (video && video.playbackRate !== 1) video.playbackRate = 1;
            report();
        }, 250);
        report();
    };

    const install = () => {
        installVodLinkPlayerStyle();
        installVodLinkVideoBridge();
    };

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', install, { once: true });
    } else {
        install();
    }
})();
)JS";

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
#endif

SyncPlayer::SyncPlayer(QWidget *parent)
    : QWidget(parent),
#if defined(Q_OS_WIN)
      m_view(new WebView2Player(this)),
      m_bridge(new PlayerBridge(this))
#else
      m_profile(new QWebEngineProfile(QStringLiteral("vodlink-player"), this)),
      m_view(new QWebEngineView(this)),
      m_channel(new QWebChannel(this)),
      m_bridge(new PlayerBridge(this))
#endif
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_view);

#if defined(Q_OS_WIN)
    connect(m_bridge, &PlayerBridge::loadRequested,
            m_view, &WebView2Player::loadVideo);
    connect(m_bridge, &PlayerBridge::seekRequested,
            m_view, &WebView2Player::seek);
    connect(m_bridge, &PlayerBridge::playRequested,
            m_view, &WebView2Player::play);
    connect(m_bridge, &PlayerBridge::pauseRequested,
            m_view, &WebView2Player::pause);
    connect(m_bridge, &PlayerBridge::messageRequested,
            m_view, &WebView2Player::showMessage);
    connect(m_bridge, &PlayerBridge::cacheRequested,
            m_view, &WebView2Player::cacheVideos);
    connect(m_view, &WebView2Player::pageReady,
            m_bridge, &PlayerBridge::ready);
    connect(m_view, &WebView2Player::timeUpdated,
            m_bridge, &PlayerBridge::onTimeUpdate);
    connect(m_view, &WebView2Player::playerError,
            this, &SyncPlayer::onPlayerError);
    connect(m_view, &WebView2Player::fullscreenToggleRequested,
            this, &SyncPlayer::toggleFullscreen);
    connect(m_view, &WebView2Player::debugMessage,
            m_bridge, &PlayerBridge::debugLog);
#else
    // Keep the player page alive and let Chromium cache YouTube resources in RAM.
    // This avoids destroying/recreating the web view on every participant switch.
    m_profile->setHttpCacheType(QWebEngineProfile::MemoryHttpCache);
    m_profile->setHttpCacheMaximumSize(256 * 1024 * 1024);
    m_profile->setPersistentCookiesPolicy(QWebEngineProfile::NoPersistentCookies);
    // Keep Qt WebEngine's real Chromium version and capability identity. A
    // hard-coded Chrome version can make YouTube select an incompatible player
    // response before it ever requests the media stream.
    const QString webEngineUserAgent = m_profile->httpUserAgent();
    const QString cacheRoot = AppPaths::cacheRoot();
    if (!cacheRoot.isEmpty()) {
        const QString playerCache = QDir(cacheRoot).filePath(QStringLiteral("webengine-player"));
        QDir().mkpath(playerCache);
        m_profile->setPersistentStoragePath(playerCache);
    }
    m_profile->setUrlRequestInterceptor(new PlayerRequestInterceptor(m_profile));
    auto *page = new PlayerPage(m_profile, m_view);
    m_view->setPage(page);
    QWebEngineScript cleanupScript;
    cleanupScript.setName(QStringLiteral("vodlink-youtube-cleanup"));
    cleanupScript.setInjectionPoint(QWebEngineScript::DocumentCreation);
    cleanupScript.setWorldId(QWebEngineScript::ApplicationWorld);
    cleanupScript.setRunsOnSubFrames(true);
    cleanupScript.setSourceCode(QString::fromUtf8(kYouTubeFrameCleanupScript));
    page->scripts().insert(cleanupScript);

    // The app owns the interaction surface and controls. Disabling the native
    // context menu at the view level also covers YouTube's cross-origin frame.
    m_view->setContextMenuPolicy(Qt::NoContextMenu);
    DebugLog::writeCategory(QStringLiteral("LiteYouTube"),
                            QStringLiteral("QWebEngine profile created origin=%1 cacheRoot=%2 userAgent=%3")
                                .arg(QString::fromLatin1(kPlayerOrigin), cacheRoot, webEngineUserAgent));
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
    m_view->settings()->setAttribute(QWebEngineSettings::FullScreenSupportEnabled, false);
    m_view->settings()->setAttribute(QWebEngineSettings::WebGLEnabled, true);
    m_view->settings()->setAttribute(QWebEngineSettings::Accelerated2dCanvasEnabled, true);

    m_channel->registerObject(QStringLiteral("bridge"), m_bridge);
    m_view->page()->setWebChannel(m_channel);

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
#endif

    connect(m_bridge, &PlayerBridge::pageReady, this, &SyncPlayer::onPageReady);
    connect(m_bridge, &PlayerBridge::timeUpdated, this, &SyncPlayer::onTimeUpdate);
#if !defined(Q_OS_WIN)
    connect(m_bridge, &PlayerBridge::playerErrorOccurred, this, &SyncPlayer::onPlayerError);
    connect(m_bridge, &PlayerBridge::fullscreenToggleRequested, this, &SyncPlayer::toggleFullscreen);
#endif
}

SyncPlayer::~SyncPlayer()
{
#if !defined(Q_OS_WIN)
    // m_profile was created before m_view, so default QObject child destruction
    // would tear the profile down while the page (a child of the view) still
    // uses it — Qt WebEngine warns and can crash on exit. Destroy the view (and
    // with it the page) first; the profile then dies safely as a child.
    delete m_view;
    m_view = nullptr;
#endif
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

void SyncPlayer::showMessage(const QString &message)
{
    if (m_pageReady && m_bridge != nullptr) {
        m_bridge->showMessage(message);
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

void SyncPlayer::onPlayerError(int code)
{
    if (m_current < 0 || m_current >= m_group.size() || m_bridge == nullptr) {
        return;
    }
    const Vod &target = m_group.at(m_current);
    // 100/101/150 are YouTube's "unavailable / embedding disallowed" codes. For
    // a friend's VOD the usual cause is VodLink's private-while-live default:
    // the owner has not finished streaming, so the archive is still private and
    // only becomes unlisted (watchable) once their stream ends.
    if (target.isMine() || (code != 100 && code != 101 && code != 150)) {
        return;
    }
    const QString owner = !target.ownerName.trimmed().isEmpty()
                              ? target.ownerName.trimmed()
                              : target.ownerEmail.trimmed();
    m_bridge->showMessage(
        QStringLiteral("%1 is still playing — this VOD becomes available once their "
                       "stream ends. Check back later.")
            .arg(owner.isEmpty() ? QStringLiteral("Your friend") : owner));
}

void SyncPlayer::toggleFullscreen()
{
    if (!m_fullscreen) {
        // Remember the exact slot in the parent's box layout so the widget can
        // be dropped back in place (with its stretch factor) on exit.
        m_normalParent = parentWidget();
        m_normalLayoutIndex = -1;
        m_normalStretch = 0;
        if (auto *box = m_normalParent
                            ? qobject_cast<QBoxLayout *>(m_normalParent->layout())
                            : nullptr) {
            m_normalLayoutIndex = box->indexOf(this);
            if (m_normalLayoutIndex >= 0) {
                m_normalStretch = box->stretch(m_normalLayoutIndex);
            }
        }
        setParent(nullptr);
        setWindowFlag(Qt::Window, true);
        setWindowTitle(QStringLiteral("VodLink"));
        showFullScreen();
        raise();
        activateWindow();
        setFocus(Qt::OtherFocusReason);
        m_fullscreen = true;
        return;
    }

    setWindowFlag(Qt::Window, false);
    if (auto *box = m_normalParent
                        ? qobject_cast<QBoxLayout *>(m_normalParent->layout())
                        : nullptr;
        box != nullptr && m_normalLayoutIndex >= 0) {
        box->insertWidget(m_normalLayoutIndex, this, m_normalStretch);
    } else if (m_normalParent != nullptr) {
        setParent(m_normalParent);
    }
    showNormal();
    show();
    m_fullscreen = false;
}

void SyncPlayer::keyPressEvent(QKeyEvent *event)
{
    if (m_fullscreen && event->key() == Qt::Key_Escape) {
        toggleFullscreen();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

qint64 SyncPlayer::absolutePositionMs() const
{
    if (m_current < 0 || m_current >= m_group.size()) {
        return 0;
    }
    return m_group.at(m_current).startedAt.toMSecsSinceEpoch()
           + static_cast<qint64>(m_currentTime * 1000.0);
}
