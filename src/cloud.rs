use crate::config::Config;
use crate::models::Vod;
use anyhow::{Context, Result, bail};
use chrono::{TimeZone, Utc};
use reqwest::Client;
use serde::{Deserialize, Serialize};
use std::time::Duration;

#[derive(Clone)]
pub(crate) struct SessionClient {
    client: Client,
    base_url: String,
}

impl SessionClient {
    pub(crate) fn new(config: &Config) -> Result<Self> {
        let client = Client::builder()
            .timeout(Duration::from_secs(10))
            .build()
            .context("Could not initialize the VodLink sharing client")?;
        Ok(Self { client, base_url: config.worker_url().trim_end_matches('/').to_owned() })
    }

    pub(crate) fn configured(&self) -> bool { !self.base_url.is_empty() }

    pub(crate) async fn start(
        &self,
        id_token: &str,
        game: &str,
        youtube_id: &str,
        started_at_ms: i64,
        friends: &[String],
    ) -> Result<()> {
        if !self.configured() || id_token.is_empty() {
            return Ok(());
        }
        let body = StartRequest {
            game,
            youtube_id,
            started_at: started_at_ms,
            friends: friends.iter().map(|email| email.trim().to_lowercase()).collect(),
        };
        let response = self.client.post(format!("{}/start", self.base_url))
            .bearer_auth(id_token)
            .json(&body)
            .send()
            .await?;
        let status = response.status();
        if !status.is_success() {
            bail!("Vod sharing start failed ({status}): {}", response.text().await.unwrap_or_default());
        }
        Ok(())
    }

    pub(crate) async fn stop(&self, id_token: &str) -> Result<Vec<Vod>> {
        if !self.configured() || id_token.is_empty() {
            return Ok(Vec::new());
        }
        let response = self.client.get(format!("{}/stop", self.base_url))
            .bearer_auth(id_token)
            .send()
            .await?;
        let status = response.status();
        let text = response.text().await?;
        if !status.is_success() {
            bail!("Vod sharing stop failed ({status}): {text}");
        }
        let payload: StopResponse = serde_json::from_str(&text).context("Invalid worker stop response")?;
        Ok(payload.vods.into_iter().filter_map(FriendVod::into_vod).collect())
    }
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct StartRequest<'a> {
    game: &'a str,
    youtube_id: &'a str,
    started_at: i64,
    friends: Vec<String>,
}

#[derive(Deserialize, Default)]
struct StopResponse {
    #[serde(default)]
    vods: Vec<FriendVod>,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct FriendVod {
    #[serde(default)]
    email: String,
    #[serde(default)]
    name: String,
    #[serde(default)]
    picture: String,
    #[serde(default)]
    youtube_id: String,
    #[serde(default)]
    game: String,
    started_at: Option<i64>,
    stopped_at: Option<i64>,
    duration_ms: Option<i64>,
}

impl FriendVod {
    fn into_vod(self) -> Option<Vod> {
        if self.email.trim().is_empty() || self.youtube_id.trim().is_empty() {
            return None;
        }
        let started_ms = self.started_at.unwrap_or_default();
        let started_at = Utc.timestamp_millis_opt(started_ms).single().unwrap_or_else(Utc::now);
        let duration = self.duration_ms.unwrap_or_else(|| {
            self.stopped_at.unwrap_or(started_ms).saturating_sub(started_ms)
        }).max(0);
        Some(Vod {
            id: 0,
            game: self.game,
            youtube_id: self.youtube_id,
            stream_status: "shared".to_owned(),
            started_at,
            duration_ms: duration,
            account_email: String::new(),
            owner_email: self.email.to_lowercase(),
            owner_name: self.name,
            owner_picture_url: self.picture,
            title: String::new(),
        })
    }
}
