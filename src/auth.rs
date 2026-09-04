use crate::config::Config;
use crate::models::AccountProfile;
use anyhow::{Context, Result, bail};
use base64::{Engine as _, engine::general_purpose::{URL_SAFE_NO_PAD, URL_SAFE}};
use rand::RngCore;
use reqwest::Client;
use serde::Deserialize;
use serde_json::Value;
use chrono::{DateTime, Duration as ChronoDuration, Utc};
use sha2::{Digest, Sha256};
use std::collections::HashMap;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpListener;
use url::Url;

const AUTH_URL: &str = "https://accounts.google.com/o/oauth2/v2/auth";
const TOKEN_URL: &str = "https://oauth2.googleapis.com/token";
const USERINFO_URL: &str = "https://openidconnect.googleapis.com/v1/userinfo";
const SCOPE: &str = "openid email profile https://www.googleapis.com/auth/youtube";

#[derive(Clone, Debug, Default)]
pub(crate) struct AuthTokens {
    access_token: String,
    refresh_token: String,
    id_token: String,
    profile: AccountProfile,
    expires_at: Option<DateTime<Utc>>,
}

impl AuthTokens {
    pub(crate) fn access_token(&self) -> &str { &self.access_token }
    pub(crate) fn refresh_token(&self) -> &str { &self.refresh_token }
    pub(crate) fn id_token(&self) -> &str { &self.id_token }
    pub(crate) fn profile(&self) -> &AccountProfile { &self.profile }
    pub(crate) fn is_signed_in(&self) -> bool { !self.access_token.is_empty() && !self.profile.email.is_empty() }
    pub(crate) fn needs_refresh(&self) -> bool {
        self.expires_at.is_some_and(|expires| expires <= Utc::now() + ChronoDuration::seconds(60))
    }
}

#[derive(Clone)]
pub(crate) struct GoogleAuth {
    client: Client,
    config: Config,
}

impl GoogleAuth {
    pub(crate) fn new(config: Config) -> Result<Self> {
        let client = Client::builder()
            .timeout(std::time::Duration::from_secs(15))
            .build()
            .context("Could not initialize the Google HTTP client")?;
        Ok(Self { client, config })
    }

    pub(crate) async fn sign_in(&self) -> Result<AuthTokens> {
        if self.config.google_client_id().trim().is_empty() {
            bail!("Google OAuth client id is not configured");
        }
        let listener = TcpListener::bind(("127.0.0.1", 0)).await
            .context("Could not open the local OAuth callback listener")?;
        let port = listener.local_addr()?.port();
        let redirect_uri = format!("http://127.0.0.1:{port}/oauth2callback");

        let verifier = random_urlsafe(48);
        let challenge = URL_SAFE_NO_PAD.encode(Sha256::digest(verifier.as_bytes()));
        let state = random_urlsafe(24);

        let mut auth_url = Url::parse(AUTH_URL)?;
        auth_url.query_pairs_mut()
            .append_pair("client_id", self.config.google_client_id())
            .append_pair("redirect_uri", &redirect_uri)
            .append_pair("response_type", "code")
            .append_pair("scope", SCOPE)
            .append_pair("code_challenge", &challenge)
            .append_pair("code_challenge_method", "S256")
            .append_pair("state", &state)
            .append_pair("access_type", "offline")
            .append_pair("prompt", "consent");
        webbrowser::open(auth_url.as_str()).context("Could not open the browser for Google sign-in")?;

        let (mut stream, _) = tokio::time::timeout(std::time::Duration::from_secs(180), listener.accept())
            .await
            .context("Google sign-in timed out")??;
        let mut buffer = vec![0_u8; 16 * 1024];
        let read = stream.read(&mut buffer).await?;
        let request = String::from_utf8_lossy(&buffer[..read]);
        let path = request.lines().next()
            .and_then(|line| line.split_whitespace().nth(1))
            .context("Invalid OAuth callback request")?;
        let callback = Url::parse(&format!("http://127.0.0.1:{port}{path}"))?;
        let params = callback.query_pairs().collect::<HashMap<_, _>>();
        if params.get("state").map(|value| value.as_ref()) != Some(state.as_str()) {
            let _ = respond(&mut stream, 400, "OAuth state mismatch. You can close this tab.").await;
            bail!("Google OAuth callback state did not match");
        }
        if let Some(error) = params.get("error") {
            let _ = respond(&mut stream, 400, "Google sign-in was cancelled. You can close this tab.").await;
            bail!("Google sign-in failed: {error}");
        }
        let code = params.get("code").context("Google OAuth callback did not contain a code")?.to_string();
        respond(&mut stream, 200, "VodLink is signed in. You can close this tab.").await?;

        let mut form = vec![
            ("client_id", self.config.google_client_id().to_owned()),
            ("code", code),
            ("code_verifier", verifier),
            ("grant_type", "authorization_code".to_owned()),
            ("redirect_uri", redirect_uri),
        ];
        if !self.config.google_client_secret().is_empty() {
            form.push(("client_secret", self.config.google_client_secret().to_owned()));
        }
        let response = self.client.post(TOKEN_URL).form(&form).send().await?;
        let status = response.status();
        let payload = response.text().await?;
        if !status.is_success() {
            bail!("Google token exchange failed ({status}): {payload}");
        }
        let token: TokenResponse = serde_json::from_str(&payload).context("Invalid Google token response")?;
        self.finish_tokens(
            token.access_token,
            token.refresh_token.unwrap_or_default(),
            token.id_token.unwrap_or_default(),
            token.expires_in,
        ).await
    }

    pub(crate) async fn refresh(&self, refresh_token: &str) -> Result<AuthTokens> {
        if refresh_token.trim().is_empty() {
            bail!("No Google refresh token is available");
        }
        let mut form = vec![
            ("client_id", self.config.google_client_id().to_owned()),
            ("refresh_token", refresh_token.to_owned()),
            ("grant_type", "refresh_token".to_owned()),
        ];
        if !self.config.google_client_secret().is_empty() {
            form.push(("client_secret", self.config.google_client_secret().to_owned()));
        }
        let response = self.client.post(TOKEN_URL).form(&form).send().await?;
        let status = response.status();
        let payload = response.text().await?;
        if !status.is_success() {
            bail!("Google token refresh failed ({status}): {payload}");
        }
        let token: TokenResponse = serde_json::from_str(&payload).context("Invalid Google refresh response")?;
        self.finish_tokens(
            token.access_token,
            refresh_token.to_owned(),
            token.id_token.unwrap_or_default(),
            token.expires_in,
        ).await
    }

    async fn finish_tokens(
        &self,
        access_token: String,
        refresh_token: String,
        id_token: String,
        expires_in: Option<i64>,
    ) -> Result<AuthTokens> {
        let profile = if !id_token.is_empty() {
            profile_from_id_token(&id_token).unwrap_or_default()
        } else {
            AccountProfile::default()
        };
        let profile = if profile.email.is_empty() {
            self.fetch_userinfo(&access_token).await?
        } else {
            profile
        };
        let expires_at = expires_in.map(|seconds| Utc::now() + ChronoDuration::seconds(seconds.max(60)));
        Ok(AuthTokens { access_token, refresh_token, id_token, profile, expires_at })
    }

    async fn fetch_userinfo(&self, access_token: &str) -> Result<AccountProfile> {
        let response = self.client.get(USERINFO_URL).bearer_auth(access_token).send().await?;
        let status = response.status();
        let body = response.text().await?;
        if !status.is_success() {
            bail!("Google userinfo failed ({status}): {body}");
        }
        let value: Value = serde_json::from_str(&body)?;
        Ok(AccountProfile::new(
            value.get("email").and_then(Value::as_str).unwrap_or_default(),
            value.get("name").and_then(Value::as_str).unwrap_or_default(),
            sanitize_picture(value.get("picture").and_then(Value::as_str).unwrap_or_default()),
        ))
    }
}

#[derive(Debug, Deserialize)]
struct TokenResponse {
    access_token: String,
    refresh_token: Option<String>,
    id_token: Option<String>,
    expires_in: Option<i64>,
}

fn random_urlsafe(bytes: usize) -> String {
    let mut data = vec![0_u8; bytes];
    rand::rng().fill_bytes(&mut data);
    URL_SAFE_NO_PAD.encode(data)
}

fn profile_from_id_token(token: &str) -> Option<AccountProfile> {
    let payload = token.split('.').nth(1)?;
    let decoded = URL_SAFE_NO_PAD.decode(payload)
        .or_else(|_| URL_SAFE.decode(payload))
        .ok()?;
    let value: Value = serde_json::from_slice(&decoded).ok()?;
    Some(AccountProfile::new(
        value.get("email")?.as_str()?,
        value.get("name").and_then(Value::as_str).unwrap_or_default(),
        sanitize_picture(value.get("picture").and_then(Value::as_str).unwrap_or_default()),
    ))
}

fn sanitize_picture(raw: &str) -> String {
    let Ok(url) = Url::parse(raw) else { return String::new() };
    if url.scheme() != "https" {
        return String::new();
    }
    let Some(host) = url.host_str().map(str::to_lowercase) else { return String::new() };
    if host == "googleusercontent.com" || host.ends_with(".googleusercontent.com") {
        raw.to_owned()
    } else {
        String::new()
    }
}

async fn respond(stream: &mut tokio::net::TcpStream, status: u16, message: &str) -> Result<()> {
    let reason = if status == 200 { "OK" } else { "Bad Request" };
    let body = format!(
        "<!doctype html><meta charset=utf-8><title>VodLink</title><style>body{{font:16px system-ui;margin:4rem;max-width:40rem}}</style><h1>VodLink</h1><p>{}</p>",
        message
    );
    let response = format!(
        "HTTP/1.1 {status} {reason}\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
        body.len()
    );
    stream.write_all(response.as_bytes()).await?;
    stream.shutdown().await?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rejects_non_google_profile_picture() {
        assert!(sanitize_picture("https://example.com/avatar.png").is_empty());
        assert_eq!(
            sanitize_picture("https://lh3.googleusercontent.com/a/test"),
            "https://lh3.googleusercontent.com/a/test"
        );
    }
}
