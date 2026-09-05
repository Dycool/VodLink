impl AppController {
    pub(crate) async fn new() -> Result<Arc<Self>> {
        let paths = AppPaths::discover()?;
        let repository = VodRepository::open(&paths.database())?;
        let config = Config::load();
        let auth = GoogleAuth::new(config.clone())?;
        let session = SessionClient::new(&config)?;
        let youtube = YouTubeLiveClient::new()?;
        let catalog = GameCatalog::load(&repository)?;
        let detector = GameDetector::new(catalog);
        let streamer = StreamerHandle::spawn()?;

        let status = AppStatus {
            auto_record: read_bool(&repository, AUTO_RECORD_SETTING, false)?,
            share_vods: read_bool(&repository, SHARE_SETTING, false)?,
            microphone: read_bool(&repository, MICROPHONE_SETTING, false)?,
            privacy_mode: normalized_privacy(
                repository
                    .setting(PRIVACY_SETTING)?
                    .as_deref()
                    .unwrap_or("game_external_audio"),
            )?,
            last_game: repository.setting(LAST_GAME_SETTING)?.unwrap_or_default(),
            ..AppStatus::default()
        };

        Ok(Arc::new(Self {
            config,
            paths,
            repository,
            auth,
            session,
            youtube,
            streamer,
            detector: StdMutex::new(detector),
            tokens: RwLock::new(AuthTokens::default()),
            status: RwLock::new(status),
            stream: Mutex::new(StreamRuntime::default()),
            explicitly_signed_out: AtomicBool::new(false),
            shutdown_requested: AtomicBool::new(false),
            shutdown_notify: Notify::new(),
        }))
    }

    fn auth_configured(&self) -> bool {
        !self.config.google_client_id().trim().is_empty()
    }

    pub(crate) async fn snapshot(&self) -> Result<Snapshot> {
        let stored_credentials = self
            .repository
            .setting(REFRESH_TOKEN_SETTING)?
            .is_some_and(|token| !token.trim().is_empty());
        Ok(Snapshot {
            status: self.status.read().await.clone(),
            vods: self.repository.list(None)?,
            games: self.repository.games()?,
            friends: self.repository.friends()?,
            recorder: self.recorder_settings()?,
            worker_configured: self.config.worker_configured(),
            auth_configured: self.auth_configured(),
            stored_credentials,
        })
    }

    pub(crate) async fn restore_stored_credentials(&self) {
        let refresh_token = match self.repository.setting(REFRESH_TOKEN_SETTING) {
            Ok(Some(token)) if !token.trim().is_empty() => token,
            Ok(_) => return,
            Err(error) => {
                self.set_error(format!("Could not read stored Google credentials: {error}"))
                    .await;
                return;
            }
        };

        self.set_message("Restoring Google session…").await;
        match self.auth.refresh(&refresh_token).await {
            Ok(restored) => {
                if self.explicitly_signed_out.load(Ordering::Acquire) {
                    return;
                }
                if let Err(error) = self.store_tokens(restored.clone()).await {
                    self.set_error(format!("Could not store restored Google session: {error}"))
                        .await;
                    return;
                }
                let profile = restored.profile().clone();
                if let Err(error) = self.adopt_account(&profile).await {
                    self.set_error(format!("Could not restore Google account state: {error}"))
                        .await;
                    return;
                }
                self.apply_profile(&profile).await;
                let label = if profile.display_name.is_empty() {
                    profile.email.clone()
                } else {
                    profile.display_name.clone()
                };
                self.set_message(format!("Signed in as {label}")).await;
            }
            Err(error) => {
                if !self.explicitly_signed_out.load(Ordering::Acquire) {
                    self.set_error(format!("Stored Google sign-in could not be restored: {error}"))
                        .await;
                }
            }
        }
    }

    pub(crate) async fn run_monitor(self: Arc<Self>) {
        loop {
            if self.shutdown_requested.load(Ordering::Acquire) {
                break;
            }
            tokio::select! {
                _ = tokio::time::sleep(SCAN_INTERVAL) => {
                    match self.scan_games() {
                        Ok((started, stopped)) => {
                            for game in started {
                                if let Err(error) = self.handle_game_started(game).await {
                                    self.set_error(error.to_string()).await;
                                    self.reset_stream_state().await;
                                }
                            }
                            for game in stopped {
                                if let Err(error) = self.handle_game_stopped(&game).await {
                                    self.set_error(error.to_string()).await;
                                }
                            }
                        }
                        Err(error) => {
                            self.set_error(format!("Game detection failed: {error}")).await;
                        }
                    }
                }
                _ = self.shutdown_notify.notified() => {
                    if self.shutdown_requested.load(Ordering::Acquire) {
                        break;
                    }
                }
            }
        }
    }

    fn scan_games(&self) -> Result<(Vec<DetectedGame>, Vec<String>)> {
        let mut detector = self
            .detector
            .lock()
            .map_err(|_| anyhow::anyhow!("Game detector lock was poisoned"))?;
        Ok(detector.scan())
    }

    pub(crate) async fn sign_in(&self) -> Result<()> {
        self.explicitly_signed_out.store(false, Ordering::Release);
        let tokens = self.auth.sign_in().await?;
        self.store_tokens(tokens).await?;
        let profile = self.tokens.read().await.profile().clone();
        self.adopt_account(&profile).await?;
        self.apply_profile(&profile).await;
        let label = if profile.display_name.is_empty() {
            profile.email.clone()
        } else {
            profile.display_name.clone()
        };
        self.set_message(format!("Signed in as {label}")).await;
        Ok(())
    }

    pub(crate) async fn sign_out(&self) -> Result<()> {
        self.explicitly_signed_out.store(true, Ordering::Release);
        self.stop_recording().await?;
        self.repository.remove_setting(REFRESH_TOKEN_SETTING)?;
        *self.tokens.write().await = AuthTokens::default();
        let mut status = self.status.write().await;
        status.signed_in_email.clear();
        status.signed_in_name.clear();
        status.signed_in_picture.clear();
        status.message = "Signed out".to_owned();
        status.error.clear();
        Ok(())
    }

    pub(crate) async fn sync_library(&self) -> Result<()> {
        if self.stream.lock().await.state != StreamState::Idle {
            bail!("Stop the active recording before syncing the YouTube library");
        }
        let tokens = self.fresh_tokens().await?;
        if !tokens.is_signed_in() {
            bail!("Sign in with Google before syncing YouTube VODs");
        }
        self.set_message("Syncing latest YouTube VODs…").await;
        let synced = match self
            .youtube
            .sync_own_library(tokens.access_token(), &tokens.profile().email)
            .await
        {
            Ok(vods) => vods,
            Err(error) if auth_expired(&error) => {
                let refreshed = self.force_refresh().await?;
                self.youtube
                    .sync_own_library(refreshed.access_token(), &refreshed.profile().email)
                    .await?
            }
            Err(error) => return Err(error),
        };
        for item in synced {
            self.repository.upsert_own_vod(&item.vod)?;
            self.repository
                .replace_clips_for_vod(&item.vod.youtube_id, &item.clips)?;
        }
        self.repository.set_setting(
            YOUTUBE_SYNC_SETTING,
            &Utc::now().timestamp_millis().to_string(),
        )?;
        self.set_message("YouTube VOD library synced").await;
        Ok(())
    }

}
