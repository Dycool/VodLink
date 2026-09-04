impl AppController {
    async fn handle_game_started(&self, game: DetectedGame) -> Result<()> {
        {
            let mut status = self.status.write().await;
            if status.last_game != game.name {
                status.last_game = game.name.clone();
                self.repository.set_setting(LAST_GAME_SETTING, &game.name)?;
            }
            if !status.auto_record {
                status.message = format!("Auto-recording is off — {} not captured", game.name);
                return Ok(());
            }
        }

        let tokens = self.fresh_tokens().await?;
        if !tokens.is_signed_in() {
            bail!("Sign in with Google before enabling automatic YouTube recording");
        }

        {
            let mut stream = self.stream.lock().await;
            if stream.state != StreamState::Idle {
                return Ok(());
            }
            stream.state = StreamState::Preparing;
            stream.pending_game = game.name.clone();
            stream.cancel_requested = false;
        }
        {
            let mut status = self.status.write().await;
            status.current_game = game.name.clone();
            status.message = format!("Creating YouTube broadcast for {}…", game.name);
            status.streaming = false;
            status.error.clear();
        }

        let recorder = self.recorder_settings()?;
        let broadcast_settings = BroadcastSettings {
            width: recorder.width,
            height: recorder.height,
            fps: recorder.fps,
            game: game.name.clone(),
        };
        let prepared = match self
            .youtube
            .prepare_broadcast(tokens.access_token(), &broadcast_settings)
            .await
        {
            Ok(prepared) => prepared,
            Err(error) if auth_expired(&error) => {
                let refreshed = self.force_refresh().await?;
                self.youtube
                    .prepare_broadcast(refreshed.access_token(), &broadcast_settings)
                    .await?
            }
            Err(error) => return Err(error),
        };
        let tokens = self.tokens.read().await.clone();

        let cancelled = {
            let stream = self.stream.lock().await;
            stream.cancel_requested || stream.state != StreamState::Preparing
        };
        if cancelled {
            if !tokens.access_token().is_empty() {
                let _ = self
                    .youtube
                    .delete_video(tokens.access_token(), &prepared.youtube_id)
                    .await;
            }
            self.reset_stream_state().await;
            return Ok(());
        }

        let (privacy, microphone) = {
            let status = self.status.read().await;
            (status.privacy_mode.clone(), status.microphone)
        };
        let (capture_mode, audio_source) = capture_policy(&privacy)?;
        let mut hints = game.process_names.clone();
        hints.push(game.executable.to_string_lossy().into_owned());
        hints.push(game.name.clone());

        let streamer = self.streamer.clone();
        let request = StreamRequest::new(
            prepared.rtmp_server.clone(),
            prepared.stream_key.clone(),
            capture_mode,
            audio_source,
            hints,
            microphone,
            recorder.clone(),
        );
        let start_result = tokio::task::spawn_blocking(move || streamer.start(request))
            .await
            .context("OBS worker task failed")?;
        if let Err(error) = start_result {
            let _ = self
                .youtube
                .delete_video(tokens.access_token(), &prepared.youtube_id)
                .await;
            return Err(error);
        }

        let started_at = Utc::now();
        let share_enabled = self.status.read().await.share_vods;
        let mut share_announced = false;
        if share_enabled && self.session.configured() && !tokens.id_token().is_empty() {
            let friends = self
                .repository
                .friends()?
                .into_iter()
                .map(|profile| profile.email)
                .collect::<Vec<_>>();
            if self
                .session
                .start(
                    tokens.id_token(),
                    &game.name,
                    &prepared.youtube_id,
                    started_at.timestamp_millis(),
                    &friends,
                )
                .await
                .is_ok()
            {
                share_announced = true;
            }
        }

        tracing::debug!(
            broadcast_id = %prepared.broadcast_id,
            stream_id = %prepared.stream_id,
            "YouTube broadcast and ingest stream bound"
        );
        {
            let mut stream = self.stream.lock().await;
            stream.state = StreamState::Streaming;
            stream.pending_game.clear();
            stream.active = Some(ActiveStream {
                game: game.name.clone(),
                prepared,
                started_at,
                share_announced,
            });
        }
        {
            let mut status = self.status.write().await;
            status.streaming = true;
            status.message = format!("Streaming {} to YouTube", game.name);
        }
        Ok(())
    }

    async fn handle_game_stopped(&self, game: &str) -> Result<()> {
        let should_stop = {
            let mut stream = self.stream.lock().await;
            match stream.state {
                StreamState::Preparing if stream.pending_game == game => {
                    stream.cancel_requested = true;
                    false
                }
                StreamState::Streaming => stream
                    .active
                    .as_ref()
                    .is_some_and(|active| active.game == game),
                _ => false,
            }
        };
        if should_stop {
            self.stop_recording().await?;
        }
        Ok(())
    }

    async fn finalize_stream(&self, active: ActiveStream) -> Result<()> {
        let result = self.finalize_stream_inner(active).await;
        self.reset_stream_state().await;
        result
    }

    async fn finalize_stream_inner(&self, active: ActiveStream) -> Result<()> {
        self.set_message("Finishing YouTube stream…").await;
        let streamer = self.streamer.clone();
        tokio::task::spawn_blocking(move || streamer.stop())
            .await
            .context("OBS worker task failed")??;

        let tokens = self.fresh_tokens().await?;
        let duration_ms = Utc::now()
            .signed_duration_since(active.started_at)
            .num_milliseconds()
            .max(0);
        let mut vod = Vod::own(&active.game, &active.prepared.youtube_id, active.started_at);
        vod.account_email = tokens.profile().email.clone();
        vod.duration_ms = duration_ms;
        self.repository.upsert_own_vod(&vod)?;

        let clips = self.repository.clips_for_vod(&vod.youtube_id)?;
        if !tokens.access_token().is_empty() {
            let _ = self
                .youtube
                .update_vod_metadata(tokens.access_token(), &vod, &clips)
                .await;
        }

        if active.share_announced && !tokens.id_token().is_empty() {
            match self.session.stop(tokens.id_token()).await {
                Ok(friend_vods) => {
                    for friend in friend_vods {
                        self.repository.upsert_friend_vod(&friend)?;
                    }
                }
                Err(error) => {
                    self.set_error(format!("Could not fetch friends' VODs: {error}"))
                        .await;
                }
            }
        }

        self.set_message("Saving the final seconds to YouTube…").await;
        tokio::time::sleep(INGEST_DRAIN).await;
        if !tokens.access_token().is_empty() {
            self.youtube
                .complete_broadcast(tokens.access_token(), &active.prepared.broadcast_id)
                .await?;
            let _ = self
                .youtube
                .ensure_vod_embeddable(tokens.access_token(), &vod.youtube_id)
                .await;
        }
        Ok(())
    }

}
