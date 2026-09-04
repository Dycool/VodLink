impl YouTubeLiveClient {
    pub(crate) async fn import_youtube_clip(
        &self,
        clip_url: &str,
        fallback_vod: &Vod,
    ) -> Result<VodClip> {
        let parsed = Url::parse(clip_url).context("Invalid YouTube clip URL")?;
        if parsed
            .host_str()
            .is_none_or(|host| host != "youtube.com" && !host.ends_with(".youtube.com"))
            || !parsed.path().starts_with("/clip/")
        {
            bail!("Only YouTube /clip/ URLs are supported");
        }
        let response = self.client.get(parsed).send().await?;
        if !response.status().is_success() {
            bail!("Could not read that YouTube Clip ({})", response.status());
        }
        let text = response.text().await?;
        let unescaped = text
            .replace("\\u0026", "&")
            .replace("\\/", "/")
            .replace("\\\"", "\"");

        let clip_id = capture_first(
            &unescaped,
            &[r#""clipId":"([^"]+)""#, r#"/clip/([A-Za-z0-9_-]+)"#],
        )
        .unwrap_or_default();
        let parent = capture_first(
            &unescaped,
            &[r#""videoId":"([^"]+)""#, r#""externalVideoId":"([^"]+)""#],
        )
        .unwrap_or_else(|| fallback_vod.youtube_id.clone());
        let start_raw = capture_first(
            &unescaped,
            &[
                r#""startTimeMs":"?(\d+)"?"#,
                r#""startSeconds":([0-9.]+)"#,
            ],
        )
        .and_then(|value| value.parse::<f64>().ok())
        .unwrap_or(0.0);
        let end_raw = capture_first(
            &unescaped,
            &[r#""endTimeMs":"?(\d+)"?"#, r#""endSeconds":([0-9.]+)"#],
        )
        .and_then(|value| value.parse::<f64>().ok())
        .unwrap_or(0.0);
        let use_millis = start_raw > 10_000.0 || end_raw > 10_000.0;
        let start = if use_millis {
            start_raw / 1000.0
        } else {
            start_raw
        };
        let end = if use_millis { end_raw / 1000.0 } else { end_raw };
        let title = capture_first(
            &unescaped,
            &[
                r#"<meta property="og:title" content="([^"]+)""#,
                r#""title":"([^"]+)""#,
            ],
        )
        .unwrap_or_else(|| format!("{} clip", fallback_vod.game));

        Ok(VodClip {
            id: 0,
            youtube_id: parent,
            clip_id,
            clip_url: clip_url.to_owned(),
            title: html_unescape(&title),
            start_seconds: start.round() as i32,
            end_seconds: end.max(start + 1.0).round() as i32,
            created_at: Utc::now(),
        }
        .normalize())
    }

}
