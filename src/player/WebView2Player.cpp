#include "WebView2Player.h"

#include "app/AppPaths.h"
#include "app/DebugLog.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPointer>
#include <QResizeEvent>
#include <QSaveFile>
#include <QStringList>
#include <QVBoxLayout>

#include <cmath>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#include <windows.h>
#include <wrl.h>

#include <WebView2.h>
#ifdef __has_attribute
#undef __has_attribute
#endif
#include <WebView2EnvironmentOptions.h>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;

namespace {

constexpr auto kPlayerHtmlResource = ":/player/player.html";
constexpr auto kPlayerVirtualUrl = L"https://vodlink.app/player.html";
constexpr auto kPlayerVirtualHost = L"vodlink.app";

constexpr auto kYouTubeFrameCleanupScript = LR"JS(
(() => {
    const install = () => {
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
        document.addEventListener('contextmenu', event => event.preventDefault(), true);
    };
    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', install, { once: true });
    } else {
        install();
    }
})();
)JS";

QString hresultText(HRESULT result)
{
    return QStringLiteral("0x%1")
        .arg(static_cast<quint32>(result), 8, 16, QLatin1Char('0'));
}

bool writePlayerPage(QString *folder, QString *error)
{
    const QString targetFolder =
        QDir(AppPaths::cacheRoot()).filePath(QStringLiteral("webview2-player"));
    if (!QDir().mkpath(targetFolder)) {
        if (error != nullptr) {
            *error = QStringLiteral("Could not create the WebView2 player cache folder.");
        }
        return false;
    }

    QFile source(QString::fromLatin1(kPlayerHtmlResource));
    if (!source.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("The bundled VodLink player page is missing.");
        }
        return false;
    }

    const QByteArray html = source.readAll();
    QSaveFile output(QDir(targetFolder).filePath(QStringLiteral("player.html")));
    if (!output.open(QIODevice::WriteOnly)
        || output.write(html) != html.size()
        || !output.commit()) {
        if (error != nullptr) {
            *error = QStringLiteral("Could not prepare the WebView2 player page.");
        }
        return false;
    }

    *folder = QDir::toNativeSeparators(targetFolder);
    return true;
}

} // namespace

struct WebView2Player::Impl
{
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webView;
    EventRegistrationToken messageToken{};
    EventRegistrationToken navigationToken{};
    bool messageHandlerRegistered = false;
    bool navigationHandlerRegistered = false;
    bool shouldUninitializeCom = false;
};

WebView2Player::WebView2Player(QWidget *parent)
    : QWidget(parent),
      m_impl(std::make_unique<Impl>()),
      m_status(new QLabel(QStringLiteral("Loading YouTube player..."), this))
{
    setAttribute(Qt::WA_NativeWindow);
    setFocusPolicy(Qt::StrongFocus);
    setAutoFillBackground(false);

    m_status->setAlignment(Qt::AlignCenter);
    m_status->setWordWrap(true);
    m_status->setStyleSheet(QStringLiteral(
        "QLabel { color: #9da4b5; background: #000; padding: 24px; }"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_status);

    initialize();
}

WebView2Player::~WebView2Player()
{
    if (m_impl->webView && m_impl->messageHandlerRegistered) {
        m_impl->webView->remove_WebMessageReceived(m_impl->messageToken);
    }
    if (m_impl->webView && m_impl->navigationHandlerRegistered) {
        m_impl->webView->remove_NavigationCompleted(m_impl->navigationToken);
    }
    if (m_impl->controller) {
        m_impl->controller->Close();
    }
    m_impl->webView.Reset();
    m_impl->controller.Reset();
    m_impl->environment.Reset();
    if (m_impl->shouldUninitializeCom) {
        CoUninitialize();
    }
}

void WebView2Player::initialize()
{
    QString preparationError;
    if (!writePlayerPage(&m_contentFolder, &preparationError)) {
        fail(preparationError);
        return;
    }

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (comResult == S_OK || comResult == S_FALSE) {
        m_impl->shouldUninitializeCom = true;
    } else if (comResult != RPC_E_CHANGED_MODE) {
        fail(QStringLiteral("Could not initialize the Windows browser runtime (%1).")
                 .arg(hresultText(comResult)));
        return;
    }

    auto options = Make<CoreWebView2EnvironmentOptions>();
    options->put_AdditionalBrowserArguments(
        L"--autoplay-policy=no-user-gesture-required "
        L"--enable-gpu --enable-accelerated-video-decode "
        L"--ignore-gpu-blocklist");

    const std::wstring userDataFolder =
        QDir::toNativeSeparators(
            QDir(AppPaths::cacheRoot()).filePath(QStringLiteral("webview2-profile")))
            .toStdWString();
    QPointer<WebView2Player> self(this);
    const HRESULT createResult = CreateCoreWebView2EnvironmentWithOptions(
        nullptr,
        userDataFolder.c_str(),
        options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [self](HRESULT result, ICoreWebView2Environment *environment) -> HRESULT {
                if (!self) return S_OK;
                if (FAILED(result) || environment == nullptr) {
                    self->fail(
                        QStringLiteral(
                            "Microsoft Edge WebView2 could not start (%1). "
                            "Install or repair the WebView2 Runtime, then reopen VodLink.")
                            .arg(hresultText(result)));
                    return S_OK;
                }

                self->m_impl->environment = environment;
                const HWND parentWindow =
                    reinterpret_cast<HWND>(self->winId());
                QPointer<WebView2Player> controllerSelf(self);
                const HRESULT controllerResult =
                    environment->CreateCoreWebView2Controller(
                        parentWindow,
                        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [controllerSelf](HRESULT controllerStatus,
                                             ICoreWebView2Controller *controller) -> HRESULT {
                                if (!controllerSelf) return S_OK;
                                if (FAILED(controllerStatus) || controller == nullptr) {
                                    controllerSelf->fail(
                                        QStringLiteral("Could not create the VodLink video view (%1).")
                                            .arg(hresultText(controllerStatus)));
                                    return S_OK;
                                }
                                controllerSelf->m_impl->controller = controller;
                                controller->get_CoreWebView2(
                                    &controllerSelf->m_impl->webView);
                                controllerSelf->finishInitialization();
                                return S_OK;
                            })
                            .Get());
                if (FAILED(controllerResult)) {
                    self->fail(
                        QStringLiteral("Could not create the VodLink video view (%1).")
                            .arg(hresultText(controllerResult)));
                }
                return S_OK;
            })
            .Get());

    if (FAILED(createResult)) {
        fail(QStringLiteral("Microsoft Edge WebView2 could not start (%1).")
                 .arg(hresultText(createResult)));
    }
}

void WebView2Player::finishInitialization()
{
    if (!m_impl->webView || !m_impl->controller) {
        fail(QStringLiteral("The Windows video view did not initialize correctly."));
        return;
    }

    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(m_impl->webView->get_Settings(&settings)) && settings) {
        settings->put_IsScriptEnabled(TRUE);
        settings->put_AreDefaultContextMenusEnabled(FALSE);
        settings->put_AreDevToolsEnabled(DebugLog::enabled() ? TRUE : FALSE);
        settings->put_IsStatusBarEnabled(FALSE);
        settings->put_IsZoomControlEnabled(FALSE);
    }

    ComPtr<ICoreWebView2_3> webView3;
    const HRESULT mappingResult = m_impl->webView.As(&webView3);
    if (FAILED(mappingResult) || !webView3) {
        fail(QStringLiteral("This WebView2 Runtime is too old for the VodLink player."));
        return;
    }
    const std::wstring contentFolder = m_contentFolder.toStdWString();
    const HRESULT hostResult = webView3->SetVirtualHostNameToFolderMapping(
        kPlayerVirtualHost,
        contentFolder.c_str(),
        COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
    if (FAILED(hostResult)) {
        fail(QStringLiteral("Could not map the bundled VodLink player (%1).")
                 .arg(hresultText(hostResult)));
        return;
    }

    m_impl->webView->AddScriptToExecuteOnDocumentCreated(
        kYouTubeFrameCleanupScript, nullptr);

    QPointer<WebView2Player> self(this);
    const HRESULT messageResult = m_impl->webView->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [self](ICoreWebView2 *,
                   ICoreWebView2WebMessageReceivedEventArgs *arguments) -> HRESULT {
                if (!self || arguments == nullptr) return S_OK;
                LPWSTR rawJson = nullptr;
                if (SUCCEEDED(arguments->get_WebMessageAsJson(&rawJson))
                    && rawJson != nullptr) {
                    const QString json =
                        QString::fromWCharArray(rawJson);
                    CoTaskMemFree(rawJson);
                    self->handleWebMessage(json);
                }
                return S_OK;
            })
            .Get(),
        &m_impl->messageToken);
    if (FAILED(messageResult)) {
        fail(QStringLiteral("Could not connect VodLink to its video page (%1).")
                 .arg(hresultText(messageResult)));
        return;
    }
    m_impl->messageHandlerRegistered = true;

    const HRESULT navigationResult = m_impl->webView->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [self](ICoreWebView2 *,
                   ICoreWebView2NavigationCompletedEventArgs *arguments) -> HRESULT {
                if (!self || arguments == nullptr) return S_OK;
                BOOL succeeded = FALSE;
                arguments->get_IsSuccess(&succeeded);
                DebugLog::writeCategory(
                    QStringLiteral("LiteYouTube/WebView2"),
                    QStringLiteral("player navigation completed success=%1")
                        .arg(succeeded != FALSE));
                if (!succeeded) {
                    self->fail(QStringLiteral("The VodLink video page could not be opened."));
                }
                return S_OK;
            })
            .Get(),
        &m_impl->navigationToken);
    if (SUCCEEDED(navigationResult)) {
        m_impl->navigationHandlerRegistered = true;
    }

    updateBounds();
    m_impl->controller->put_IsVisible(TRUE);
    const HRESULT navigateResult = m_impl->webView->Navigate(kPlayerVirtualUrl);
    if (FAILED(navigateResult)) {
        fail(QStringLiteral("The VodLink video page could not be opened (%1).")
                 .arg(hresultText(navigateResult)));
    }
}

void WebView2Player::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateBounds();
}

void WebView2Player::updateBounds()
{
    if (!m_impl->controller) return;
    const RECT bounds{0, 0, width(), height()};
    m_impl->controller->put_Bounds(bounds);
}

void WebView2Player::handleWebMessage(const QString &json)
{
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(json.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        DebugLog::writeCategory(
            QStringLiteral("LiteYouTube/WebView2"),
            QStringLiteral("invalid player message: %1").arg(json));
        return;
    }

    const QJsonObject object = document.object();
    const QString type = object.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("ready")) {
        m_status->hide();
        emit pageReady();
        return;
    }
    if (type == QStringLiteral("time")) {
        const double seconds =
            object.value(QStringLiteral("seconds")).toDouble(-1.0);
        if (seconds >= 0.0 && std::isfinite(seconds)) {
            emit timeUpdated(seconds);
        }
        return;
    }
    if (type == QStringLiteral("error")) {
        emit playerError(object.value(QStringLiteral("code")).toInt());
        return;
    }
    if (type == QStringLiteral("fullscreen")) {
        emit fullscreenToggleRequested();
        return;
    }
    if (type == QStringLiteral("debug")) {
        emit debugMessage(
            object.value(QStringLiteral("category")).toString(),
            object.value(QStringLiteral("text")).toString());
    }
}

void WebView2Player::sendCommand(const QJsonObject &command)
{
    if (!m_impl->webView) return;
    const QByteArray json = QJsonDocument(command).toJson(QJsonDocument::Compact);
    const QString wideJson = QString::fromUtf8(json);
    const HRESULT result = m_impl->webView->PostWebMessageAsJson(
        reinterpret_cast<LPCWSTR>(wideJson.utf16()));
    if (FAILED(result)) {
        DebugLog::writeCategory(
            QStringLiteral("LiteYouTube/WebView2"),
            QStringLiteral("player command failed result=%1 command=%2")
                .arg(hresultText(result), QString::fromUtf8(json)));
    }
}

void WebView2Player::loadVideo(const QString &videoId, double seekSeconds)
{
    sendCommand({
        {QStringLiteral("command"), QStringLiteral("load")},
        {QStringLiteral("videoId"), videoId},
        {QStringLiteral("seek"), seekSeconds},
    });
}

void WebView2Player::seek(double seconds)
{
    sendCommand({
        {QStringLiteral("command"), QStringLiteral("seek")},
        {QStringLiteral("seconds"), seconds},
    });
}

void WebView2Player::play()
{
    sendCommand({{QStringLiteral("command"), QStringLiteral("play")}});
}

void WebView2Player::pause()
{
    sendCommand({{QStringLiteral("command"), QStringLiteral("pause")}});
}

void WebView2Player::showMessage(const QString &text)
{
    sendCommand({
        {QStringLiteral("command"), QStringLiteral("message")},
        {QStringLiteral("text"), text},
    });
}

void WebView2Player::cacheVideos(const QStringList &videoIds)
{
    QJsonArray ids;
    for (const QString &id : videoIds) ids.append(id);
    sendCommand({
        {QStringLiteral("command"), QStringLiteral("cache")},
        {QStringLiteral("videoIds"), ids},
    });
}

void WebView2Player::fail(const QString &message)
{
    DebugLog::writeCategory(QStringLiteral("LiteYouTube/WebView2"), message);
    m_status->setText(message);
    m_status->show();
    emit fatalError(message);
}
