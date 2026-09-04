impl YouTubeLiveClient {
    pub(crate) async fn update_vod_metadata(
        &self,
        token: &str,
        vod: &Vod,
        clips: &[VodClip],
    ) -> Result<()> {
        let current = self
            .request(
                Method::GET,
                "/videos",
                &[("part", "snippet"), ("id", &vod.youtube_id)],
                token,
                None,
            )
            .await?;
        let Some(item) = current
            .get("items")
            .and_then(Value::as_array)
            .and_then(|items| items.first())
        else {
            bail!("YouTube VOD {} no longer exists", vod.youtube_id);
        };
        let snippet = item.get("snippet").cloned().unwrap_or_else(|| json!({}));
        let title = if vod.title.trim().is_empty() {
            snippet
                .get("title")
                .and_then(Value::as_str)
                .unwrap_or(&vod.game)
                .to_owned()
        } else {
            vod.title.clone()
        };
        let existing_description = snippet
            .get("description")
            .and_then(Value::as_str)
            .unwrap_or_default();
        let metadata = json!({
            "version": 1,
            "app": "VodLink",
            "game": vod.game,
            "youtubeId": vod.youtube_id,
            "startedAt": vod.started_at.to_rfc3339(),
            "durationMs": vod.duration_ms.max(0),
            "clips": clips.iter().map(|clip| json!({
                "clipId": clip.clip_id,
                "clipUrl": clip.clip_url,
                "title": clip.title,
                "startSeconds": clip.start_seconds,
                "endSeconds": clip.end_seconds,
                "createdAt": clip.created_at.to_rfc3339()
            })).collect::<Vec<_>>()
        });
        let description = replace_metadata_block(existing_description, &metadata)?;
        let category = snippet
            .get("categoryId")
            .and_then(Value::as_str)
            .unwrap_or("20");
        self.request(
            Method::PUT,
            "/videos",
            &[("part", "snippet")],
            token,
            Some(json!({
                "id": vod.youtube_id,
                "snippet": {
                    "title": title,
                    "description": description,
                    "categoryId": category
                }
            })),
        )
        .await?;
        Ok(())
    }

    pub(crate) async fn delete_video(&self, token: &str, youtube_id: &str) -> Result<()> {
        if token.trim().is_empty() {
            bail!("A Google access token is required to delete a YouTube VOD");
        }
        let url = format!("{API_BASE}/videos?id={}", urlencoding::encode(youtube_id));
        let response = self.client.delete(url).bearer_auth(token).send().await?;
        if response.status().is_success()
            || response.status() == StatusCode::NOT_FOUND
            || response.status() == StatusCode::GONE
        {
            return Ok(());
        }
        let status = response.status();
        bail!(
            "YouTube video delete failed ({status}): {}",
            response.text().await.unwrap_or_default()
        );
    }

    pub(crate) async fn sync_own_library(
        &self,
        token: &str,
        account_email: &str,
    ) -> Result<Vec<SyncedVod>> {
        let channels = self
            .request(
                Method::GET,
                "/channels",
                &[("part", "contentDetails"), ("mine", "true")],
                token,
                None,
            )
            .await?;
        let uploads = channels
            .pointer("/items/0/contentDetails/relatedPlaylists/uploads")
            .and_then(Value::as_str)
            .context("Could not find the account uploads playlist")?
            .to_owned();

        let mut ids = Vec::<String>::new();
        let mut page_token = String::new();
        for _ in 0..2 {
            let mut owned = vec![
                ("part".to_owned(), "contentDetails".to_owned()),
                ("playlistId".to_owned(), uploads.clone()),
                ("maxResults".to_owned(), "50".to_owned()),
            ];
            if !page_token.is_empty() {
                owned.push(("pageToken".to_owned(), page_token.clone()));
            }
            let borrowed = owned
                .iter()
                .map(|(key, value)| (key.as_str(), value.as_str()))
                .collect::<Vec<_>>();
            let page = self
                .request(Method::GET, "/playlistItems", &borrowed, token, None)
                .await?;
            if let Some(items) = page.get("items").and_then(Value::as_array) {
                ids.extend(items.iter().filter_map(|item| {
                    item.pointer("/contentDetails/videoId")
                        .and_then(Value::as_str)
                        .map(ToOwned::to_owned)
                }));
            }
            page_token = page
                .get("nextPageToken")
                .and_then(Value::as_str)
                .unwrap_or_default()
                .to_owned();
            if page_token.is_empty() {
                break;
            }
        }

        let mut vods = Vec::new();
        for batch in ids.chunks(50) {
            let joined = batch.join(",");
            if joined.is_empty() {
                continue;
            }
            let videos = self
                .request(
                    Method::GET,
                    "/videos",
                    &[
                        ("part", "snippet,contentDetails,status,processingDetails"),
                        ("id", &joined),
                    ],
                    token,
                    None,
                )
                .await?;
            let Some(items) = videos.get("items").and_then(Value::as_array) else {
                continue;
            };
            for item in items {
                if let Some(vod) = parse_library_item(item, account_email) {
                    vods.push(SyncedVod {
                        clips: parse_library_clips(item, &vod.youtube_id),
                        vod,
                    });
                }
            }
        }
        vods.sort_by(|left, right| right.vod.started_at.cmp(&left.vod.started_at));
        Ok(vods)
    }

}
