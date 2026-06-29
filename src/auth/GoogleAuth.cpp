#include "GoogleAuth.h"

#include <QByteArray>
#include <QDesktopServices>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QOAuth2AuthorizationCodeFlow>
#include <QSet>
#include <QOAuthHttpServerReplyHandler>
#include <QStringList>
#include <QtGlobal>
#include <QUrl>
#include <QUrlQuery>
#include <QVariant>

namespace {
constexpr auto kAuthUrl = "https://accounts.google.com/o/oauth2/v2/auth";
constexpr auto kTokenUrl = "https://oauth2.googleapis.com/token";
constexpr auto kUserInfoUrl = "https://openidconnect.googleapis.com/v1/userinfo";
constexpr auto kScope =
    "openid email profile https://www.googleapis.com/auth/youtube";

bool isUserCancelledOAuth(const QString &error, const QString &description)
{
    return error.trimmed().compare(QStringLiteral("access_denied"), Qt::CaseInsensitive) == 0
           || description.trimmed().compare(QStringLiteral("access_denied"), Qt::CaseInsensitive) == 0;
}

QString browserCallbackClosePage()
{
    return QStringLiteral(R"HTML(<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <title>VodLink</title>
  <script>
    function closeVodLinkTab() {
      window.open('', '_self');
      window.close();
      setTimeout(function () {
        document.body.className = 'visible';
      }, 700);
    }
  </script>
  <style>
    html, body { margin: 0; width: 100%; height: 100%; background: #070b12; color: #edf2fb; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif; }
    body { display: flex; align-items: center; justify-content: center; opacity: 0; transition: opacity 120ms ease; }
    body.visible { opacity: 1; }
    .card { padding: 28px 34px; border: 1px solid rgba(124,77,255,.35); border-radius: 18px; background: rgba(13,18,28,.92); box-shadow: 0 18px 50px rgba(0,0,0,.35); text-align: center; }
    .title { font-size: 18px; font-weight: 800; margin-bottom: 8px; }
    .hint { color: #9aa4b8; font-size: 14px; }
  </style>
</head>
<body onload="closeVodLinkTab()">
  <div class="card">
    <div class="title">VodLink</div>
    <div class="hint">You can close this tab.</div>
  </div>
</body>
</html>)HTML");
}
} // namespace

GoogleAuth::GoogleAuth(QObject *parent)
    : QObject(parent),
      m_flow(new QOAuth2AuthorizationCodeFlow(this)),
      m_network(new QNetworkAccessManager(this))
{
    m_flow->setAuthorizationUrl(QUrl(QString::fromLatin1(kAuthUrl)));
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    m_flow->setTokenUrl(QUrl(QString::fromLatin1(kTokenUrl)));
    QSet<QByteArray> scopeTokens;
    const QStringList scopes = QString::fromLatin1(kScope).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString &scope : scopes) {
        scopeTokens.insert(scope.toLatin1());
    }
    m_flow->setRequestedScopeTokens(scopeTokens);
#else
    m_flow->setAccessTokenUrl(QUrl(QString::fromLatin1(kTokenUrl)));
    m_flow->setScope(QString::fromLatin1(kScope));
#endif

    // PKCE (Qt 6.8+) — required for production installed apps.
    m_flow->setPkceMethod(QOAuth2AuthorizationCodeFlow::PkceMethod::S256);

    // Append Google-specific params so we always receive a refresh token.
    connect(m_flow, &QOAuth2AuthorizationCodeFlow::authorizeWithBrowser, this,
            [](QUrl url) {
                QUrlQuery query(url);
                query.addQueryItem(QStringLiteral("access_type"), QStringLiteral("offline"));
                query.addQueryItem(QStringLiteral("prompt"), QStringLiteral("consent"));
                url.setQuery(query);
                QDesktopServices::openUrl(url);
            });

    // Google access tokens last ~1h; refresh well before that while signed in.
    m_refreshTimer.setInterval(45 * 60 * 1000);
    connect(&m_refreshTimer, &QTimer::timeout, this, &GoogleAuth::refreshNow);

    connect(m_flow, &QOAuth2AuthorizationCodeFlow::granted, this, &GoogleAuth::handleGranted);
    // The server-error signal was renamed in Qt 6.9; error() is its 6.8 name
    // (and is deprecated in 6.9+). Pick the right one for the Qt we build against.
    auto onServerError = [this](const QString &error, const QString &description, const QUrl &) {
        if (isUserCancelledOAuth(error, description)) {
            return;
        }
        emit authError(description.isEmpty() ? error : description);
    };
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    connect(m_flow, &QAbstractOAuth2::serverReportedErrorOccurred, this, onServerError);
#else
    connect(m_flow, &QAbstractOAuth2::error, this, onServerError);
#endif
}

void GoogleAuth::configure(const QString &clientId, const QString &clientSecret)
{
    m_clientId = clientId;
    m_clientSecret = clientSecret;
    m_flow->setClientIdentifier(clientId);
    // Google's token endpoint requires the client secret in the authorization-code
    // exchange for Desktop-app clients, even alongside PKCE. Without it the redirect
    // ("Callback received") succeeds but the token exchange fails and granted() never
    // fires. Google documents this secret as non-confidential for installed apps.
    m_flow->setClientIdentifierSharedKey(clientSecret);

    if (m_replyHandler == nullptr) {
        // Port 0 -> an ephemeral loopback port. Google allows loopback redirects
        // with any port for "Desktop app" clients.
        m_replyHandler = new QOAuthHttpServerReplyHandler(QHostAddress::LocalHost, 0, this);
        m_replyHandler->setCallbackText(browserCallbackClosePage());
        m_flow->setReplyHandler(m_replyHandler);
    }
    m_configured = !clientId.isEmpty();
}

bool GoogleAuth::isConfigured() const
{
    return m_configured;
}

void GoogleAuth::restore(const QString &refreshToken)
{
    if (!m_configured || refreshToken.isEmpty()) {
        return;
    }
    m_flow->setRefreshToken(refreshToken);
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    m_flow->refreshTokens();
#else
    m_flow->refreshAccessToken();
#endif
}

void GoogleAuth::signIn()
{
    if (!m_configured) {
        emit authError(QStringLiteral(
            "Google sign-in is not configured. This build has no Google client ID "
            "baked in (see README)."));
        return;
    }
    m_flow->grant();
}

void GoogleAuth::signOut()
{
    m_refreshTimer.stop();
    m_flow->setToken(QString());
    m_flow->setRefreshToken(QString());
    m_idToken.clear();
    m_email.clear();
    m_displayName.clear();
    m_pictureUrl.clear();
    emit refreshTokenChanged(QString());
    emit signedOut();
}

void GoogleAuth::refreshNow()
{
    if (m_configured && !m_flow->refreshToken().isEmpty()) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
        m_flow->refreshTokens();
#else
        m_flow->refreshAccessToken();
#endif
    }
}

bool GoogleAuth::isSignedIn() const
{
    return !m_flow->token().isEmpty();
}

QString GoogleAuth::email() const
{
    return m_email;
}

QString GoogleAuth::displayName() const
{
    return m_displayName;
}

QString GoogleAuth::pictureUrl() const
{
    return m_pictureUrl;
}

QString GoogleAuth::accessToken() const
{
    return m_flow->token();
}

QString GoogleAuth::idToken() const
{
    return m_idToken;
}

QString GoogleAuth::refreshToken() const
{
    return m_flow->refreshToken();
}

void GoogleAuth::handleGranted()
{
    // extraTokens() (Qt 6.6+) carries the OIDC id_token alongside the access token.
    const QVariant idTokenVariant = m_flow->extraTokens().value(QStringLiteral("id_token"));
    const QString idToken = idTokenVariant.toString();
    const bool firstSignIn = m_email.isEmpty();

    if (!idToken.isEmpty()) {
        m_idToken = idToken;
        const QString email = emailFromIdToken(idToken);
        if (!email.isEmpty()) {
            m_email = email;
        }
        QStringList parts = idToken.split(u'.');
        if (parts.size() == 3) {
            QByteArray payload = parts.at(1).toUtf8();
            payload.replace('-', '+').replace('_', '/');
            while (payload.size() % 4 != 0) {
                payload.append('=');
            }
            const QJsonObject claims = QJsonDocument::fromJson(
                QByteArray::fromBase64(payload)).object();
            m_displayName = claims.value(QStringLiteral("name")).toString().left(100);
            const QUrl picture(claims.value(QStringLiteral("picture")).toString());
            const QString host = picture.host().toLower();
            if (picture.scheme() == QStringLiteral("https")
                && (host == QStringLiteral("googleusercontent.com")
                    || host.endsWith(QStringLiteral(".googleusercontent.com")))) {
                m_pictureUrl = picture.toString();
            } else {
                m_pictureUrl.clear();
            }
        }
    }

    if (!m_refreshTimer.isActive()) {
        m_refreshTimer.start();
    }

    // Persist the refresh token (Google issues it on first consent, and it may be
    // rotated on later refreshes) so sign-in survives restarts.
    const QString refresh = m_flow->refreshToken();
    if (!refresh.isEmpty()) {
        emit refreshTokenChanged(refresh);
    }

    emit tokensChanged();
    if (firstSignIn && !m_email.isEmpty()) {
        emit signedIn(m_email);
    }

    // Google refresh responses frequently omit the id_token, so after a silent
    // restore we may still lack the email/profile. Fetch them from userinfo.
    if (m_email.isEmpty() || m_pictureUrl.isEmpty()) {
        fetchUserInfo();
    }
}

void GoogleAuth::fetchUserInfo()
{
    const QString token = m_flow->token();
    if (token.isEmpty()) {
        return;
    }
    QNetworkRequest request{QUrl(QString::fromLatin1(kUserInfoUrl))};
    request.setRawHeader("Authorization", QByteArray("Bearer ") + token.toUtf8());
    request.setTransferTimeout(8000);
    QNetworkReply *reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            return;
        }
        const QJsonObject info = QJsonDocument::fromJson(reply->readAll()).object();
        const bool firstSignIn = m_email.isEmpty();
        const QString email = info.value(QStringLiteral("email")).toString();
        if (!email.isEmpty()) {
            m_email = email;
        }
        const QString name = info.value(QStringLiteral("name")).toString();
        if (!name.isEmpty()) {
            m_displayName = name.left(100);
        }
        const QUrl picture(info.value(QStringLiteral("picture")).toString());
        const QString host = picture.host().toLower();
        if (picture.scheme() == QStringLiteral("https")
            && (host == QStringLiteral("googleusercontent.com")
                || host.endsWith(QStringLiteral(".googleusercontent.com")))) {
            m_pictureUrl = picture.toString();
        }
        emit tokensChanged();
        if (firstSignIn && !m_email.isEmpty()) {
            emit signedIn(m_email);
        }
    });
}

QString GoogleAuth::emailFromIdToken(const QString &idToken)
{
    const QStringList parts = idToken.split(u'.');
    if (parts.size() != 3) {
        return {};
    }
    QByteArray payload = parts.at(1).toUtf8();
    // Base64url -> base64 with padding for QByteArray::fromBase64.
    payload.replace('-', '+').replace('_', '/');
    while (payload.size() % 4 != 0) {
        payload.append('=');
    }
    const QJsonDocument document = QJsonDocument::fromJson(QByteArray::fromBase64(payload));
    return document.object().value(QStringLiteral("email")).toString();
}
