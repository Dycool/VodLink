impl YouTubeLiveClient {
    pub(crate) fn new() -> Result<Self> {
        Ok(Self {
            client: Client::builder()
                .timeout(Duration::from_secs(15))
                .build()
                .context("Could not initialize the YouTube HTTP client")?,
        })
    }

    pub(crate) async fn check_live_eligibility(&self, token: &str) -> Result<()> {
        self.request(
            Method::GET,
            "/liveBroadcasts",
            &[("part", "id"), ("mine", "true"), ("maxResults", "1")],
            token,
            None,
        )
        .await?;
        Ok(())
    }

    pub(crate) async fn prepare_broadcast(
        &self,
        token: &str,
        settings: &BroadcastSettings,
    ) -> Result<PreparedBroadcast> {
        self.check_live_eligibility(token).await?;
        let tier = youtube_quality_tier(settings.width, settings.height);
        let frame_rate = if settings.fps <= 30 { "30fps" } else { "60fps" };
        let title = format!("{} — {}", settings.game, Local::now().format("%Y-%m-%d %H:%M"));
        let scheduled = (Utc::now() + ChronoDuration::seconds(5)).to_rfc3339();

        let broadcast = self
            .request(
                Method::POST,
                "/liveBroadcasts",
                &[("part", "snippet,status,contentDetails")],
                token,
                Some(json!({
                    "snippet": {
                        "title": title,
                        "description": DEFAULT_DESCRIPTION,
                        "scheduledStartTime": scheduled
                    },
                    "status": {
                        "privacyStatus": "private",
                        "selfDeclaredMadeForKids": false
                    },
                    "contentDetails": {
                        "enableAutoStart": true,
                        "enableAutoStop": false,
                        "enableDvr": true,
                        "recordFromStart": true,
                        "latencyPreference": "normal"
                    }
                })),
            )
            .await?;
        let broadcast_id =
            require_string(&broadcast, "id", "YouTube did not return a broadcast id")?;

        let stream = match self
            .request(
                Method::POST,
                "/liveStreams",
                &[("part", "snippet,cdn,status,contentDetails")],
                token,
                Some(json!({
                    "snippet": { "title": format!("VodLink {}", settings.game) },
                    "cdn": {
                        "frameRate": frame_rate,
                        "ingestionType": "rtmp",
                        "resolution": tier
                    },
                    "contentDetails": { "isReusable": false }
                })),
            )
            .await
        {
            Ok(value) => value,
            Err(error) => {
                let _ = self.delete_video(token, &broadcast_id).await;
                return Err(error);
            }
        };
        let stream_id = require_string(&stream, "id", "YouTube did not return a stream id")?;
        let ingestion = stream
            .pointer("/cdn/ingestionInfo")
            .context("YouTube did not return RTMP ingestion information")?;
        let server = ingestion
            .get("rtmpsIngestionAddress")
            .or_else(|| ingestion.get("ingestionAddress"))
            .and_then(Value::as_str)
            .unwrap_or_default()
            .trim()
            .to_owned();
        let key = ingestion
            .get("streamName")
            .and_then(Value::as_str)
            .unwrap_or_default()
            .trim()
            .to_owned();
        if server.is_empty() || key.is_empty() {
            let _ = self.delete_video(token, &broadcast_id).await;
            bail!("YouTube returned incomplete RTMP ingestion information");
        }

        if let Err(error) = self
            .request(
                Method::POST,
                "/liveBroadcasts/bind",
                &[
                    ("part", "id,status"),
                    ("id", &broadcast_id),
                    ("streamId", &stream_id),
                ],
                token,
                None,
            )
            .await
        {
            let _ = self.delete_video(token, &broadcast_id).await;
            return Err(error);
        }

        Ok(PreparedBroadcast {
            broadcast_id: broadcast_id.clone(),
            stream_id,
            youtube_id: broadcast_id,
            rtmp_server: server,
            stream_key: key,
        })
    }

    pub(crate) async fn complete_broadcast(&self, token: &str, broadcast_id: &str) -> Result<()> {
        let current = self
            .request(
                Method::GET,
                "/liveBroadcasts",
                &[("part", "status"), ("id", broadcast_id)],
                token,
                None,
            )
            .await?;
        let item = current
            .get("items")
            .and_then(Value::as_array)
            .and_then(|items| items.first())
            .context("YouTube broadcast disappeared before it could be finalized")?;
        let status = item
            .pointer("/status/lifeCycleStatus")
            .and_then(Value::as_str)
            .unwrap_or_default();
        match status {
            "complete" => Ok(()),
            "testing" | "live" => {
                self.request(
                    Method::POST,
                    "/liveBroadcasts/transition",
                    &[
                        ("part", "id,status"),
                        ("id", broadcast_id),
                        ("broadcastStatus", "complete"),
                    ],
                    token,
                    None,
                )
                .await?;
                Ok(())
            }
            other => bail!(
                "YouTube broadcast is in lifecycle state '{other}' and was preserved instead of being deleted"
            ),
        }
    }

    pub(crate) async fn ensure_vod_embeddable(&self, token: &str, youtube_id: &str) -> Result<()> {
        let response = self
            .request(
                Method::GET,
                "/videos",
                &[("part", "status"), ("id", youtube_id)],
                token,
                None,
            )
            .await?;
        let Some(item) = response
            .get("items")
            .and_then(Value::as_array)
            .and_then(|items| items.first())
        else {
            return Ok(());
        };
        let privacy = item
            .pointer("/status/privacyStatus")
            .and_then(Value::as_str)
            .unwrap_or_default();
        let embeddable = item
            .pointer("/status/embeddable")
            .and_then(Value::as_bool)
            .unwrap_or(false);
        if matches!(privacy, "unlisted" | "public") && embeddable {
            return Ok(());
        }
        self.request(
            Method::PUT,
            "/videos",
            &[("part", "status")],
            token,
            Some(json!({
                "id": youtube_id,
                "status": {
                    "privacyStatus": "unlisted",
                    "selfDeclaredMadeForKids": false,
                    "embeddable": true,
                    "publicStatsViewable": false
                }
            })),
        )
        .await?;
        Ok(())
    }

}
