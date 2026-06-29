#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

class QOAuth2AuthorizationCodeFlow;
class QOAuthHttpServerReplyHandler;
class QNetworkAccessManager;

// Google Sign-In for desktop: OAuth 2.0 Authorization Code flow with PKCE and a
// loopback redirect. Yields a YouTube access token (for the Data API) and an
// OpenID Connect ID token (sent to the Worker as the user's verified identity).
//
// Requires a Google Cloud "Desktop app" OAuth client. The flow uses PKCE; Google
// additionally requires the client secret in the code exchange for Desktop-app
// clients (Google documents it as non-confidential for installed apps), so it is
// baked in alongside the client ID. The refresh token is persisted by the caller
// via the refreshTokenChanged() signal so sign-in survives restarts.
class GoogleAuth final : public QObject
{
    Q_OBJECT

public:
    explicit GoogleAuth(QObject *parent = nullptr);

    void configure(const QString &clientId, const QString &clientSecret = QString());
    [[nodiscard]] bool isConfigured() const;

    // Restore a previously persisted refresh token and silently obtain a fresh
    // access/ID token (no browser) if possible.
    void restore(const QString &refreshToken);

    void signIn();   // interactive: opens the system browser
    void signOut();
    void refreshNow(); // force an access-token refresh using the stored refresh token

    [[nodiscard]] bool isSignedIn() const;
    [[nodiscard]] QString email() const;
    [[nodiscard]] QString displayName() const;
    [[nodiscard]] QString pictureUrl() const;
    [[nodiscard]] QString accessToken() const;
    [[nodiscard]] QString idToken() const;
    [[nodiscard]] QString refreshToken() const;

    // Decodes the "email" claim from an OIDC ID token (no signature check; the
    // Worker verifies signatures). Returns empty on malformed input.
    [[nodiscard]] static QString emailFromIdToken(const QString &idToken);

signals:
    void signedIn(const QString &email);
    void signedOut();
    void tokensChanged();                              // access/ID token refreshed
    void refreshTokenChanged(const QString &refreshToken); // persist this
    void authError(const QString &message);

private:
    void handleGranted();
    // Fallback when the granted/refresh response carries no id_token (common on
    // Google refreshes): fetch email/name/picture from the OIDC userinfo endpoint.
    void fetchUserInfo();

    QOAuth2AuthorizationCodeFlow *m_flow = nullptr;
    QOAuthHttpServerReplyHandler *m_replyHandler = nullptr;
    QNetworkAccessManager *m_network = nullptr;
    QTimer m_refreshTimer; // proactively refreshes the access token before it expires
    QString m_clientId;
    QString m_clientSecret;
    QString m_idToken;
    QString m_email;
    QString m_displayName;
    QString m_pictureUrl;
    bool m_configured = false;
};
