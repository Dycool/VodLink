fn validate_recorder_bitrate(value: u32) -> Result<u32> {
    if !(2_500..=40_000).contains(&value) {
        bail!("Recorder bitrate must be between 2500 and 40000 Kbps");
    }
    Ok(value)
}

impl AppController {
    pub(crate) async fn update_settings(&self, update: SettingsUpdate) -> Result<()> {
        if let Some(value) = update.auto_record {
            self.repository
                .set_setting(AUTO_RECORD_SETTING, bool_text(value))?;
            self.status.write().await.auto_record = value;
            if !value {
                self.stop_recording().await?;
            }
        }
        if let Some(value) = update.share_vods {
            self.repository.set_setting(SHARE_SETTING, bool_text(value))?;
            self.status.write().await.share_vods = value;
        }
        if let Some(value) = update.microphone {
            self.repository
                .set_setting(MICROPHONE_SETTING, bool_text(value))?;
            self.status.write().await.microphone = value;
        }
        if let Some(value) = update.notifications {
            self.repository
                .set_setting(NOTIFICATIONS_SETTING, bool_text(value))?;
            self.status.write().await.notifications = value;
        }
        if let Some(value) = update.launch_at_startup {
            crate::startup::set_enabled(value)?;
            self.repository
                .set_setting(LAUNCH_AT_STARTUP_SETTING, bool_text(value))?;
            self.status.write().await.launch_at_startup = crate::startup::enabled();
        }
        if let Some(value) = update.privacy_mode {
            let normalized = normalized_privacy(&value)?;
            self.repository.set_setting(PRIVACY_SETTING, &normalized)?;
            self.status.write().await.privacy_mode = normalized;
        }
        if let Some(value) = update.encoder {
            let normalized = normalized_encoder(&value)?;
            self.repository.set_setting(ENCODER_SETTING, &normalized)?;
        }
        if let Some(value) = update.bitrate_kbps {
            let value = validate_recorder_bitrate(value)?;
            self.repository
                .set_setting(BITRATE_SETTING, &value.to_string())?;
        }
        if let Some(value) = update.resolution {
            let (width, height) = parse_resolution(&value)?;
            self.repository
                .set_setting(RESOLUTION_SETTING, &format!("{width}x{height}"))?;
        }
        if let Some(value) = update.fps {
            if !matches!(value, 30 | 60) {
                bail!("Recorder FPS must be 30 or 60");
            }
            self.repository.set_setting(FPS_SETTING, &value.to_string())?;
        }
        self.set_message("Settings saved").await;
        Ok(())
    }

    pub(crate) async fn add_friend(&self, email: &str) -> Result<()> {
        let email = email.trim().to_lowercase();
        if email.is_empty() || !email.contains('@') {
            bail!("Enter a valid email address");
        }
        let current = self.tokens.read().await.profile().email.clone();
        if !current.is_empty() && current == email {
            bail!("You cannot add your own Google account as a friend");
        }
        self.repository
            .add_friend(&AccountProfile::new(email, "", ""))?;
        self.set_message("Friend added").await;
        Ok(())
    }

    pub(crate) async fn remove_friend(&self, email: &str) -> Result<()> {
        self.repository.remove_friend(email)?;
        self.set_message("Friend removed").await;
        Ok(())
    }

    pub(crate) async fn add_manual_game(&self, executable: &str, display_name: &str) -> Result<()> {
        let executable = Path::new(executable);
        if !executable.is_file() {
            bail!("Select an existing game executable");
        }
        add_manual_game(&self.repository, executable, display_name)?;
        let catalog = GameCatalog::load(&self.repository)?;
        {
            let mut detector = self
                .detector
                .lock()
                .map_err(|_| anyhow::anyhow!("Game detector lock was poisoned"))?;
            *detector = GameDetector::new(catalog);
        }
        self.set_message(format!("Added {}", display_name.trim())).await;
        Ok(())
    }

    pub(crate) async fn delete_vod(&self, youtube_id: &str) -> Result<()> {
        let vod = self
            .repository
            .own_vod(youtube_id)?
            .context("This VOD is not in your local library")?;
        let tokens = self.fresh_tokens().await?;
        if tokens.profile().email.is_empty()
            || vod.account_email.trim().to_lowercase() != tokens.profile().email
        {
            bail!("This VOD belongs to another Google account and cannot be deleted");
        }
        match self.youtube.delete_video(tokens.access_token(), youtube_id).await {
            Ok(()) => {}
            Err(error) if auth_expired(&error) => {
                let refreshed = self.force_refresh().await?;
                self.youtube
                    .delete_video(refreshed.access_token(), youtube_id)
                    .await?;
            }
            Err(error) => return Err(error),
        }
        self.repository.remove_own_vod(youtube_id)?;
        self.set_message("Deleted VOD from YouTube").await;
        Ok(())
    }

    pub(crate) async fn remove_friend_vod(&self, youtube_id: &str) -> Result<()> {
        self.repository.remove_friend_vod(youtube_id)?;
        self.set_message("Removed friend VOD from the local library")
            .await;
        Ok(())
    }

    pub(crate) fn clips_for_vod(&self, youtube_id: &str) -> Result<Vec<VodClip>> {
        self.repository.clips_for_vod(youtube_id)
    }

    pub(crate) async fn import_clip(&self, youtube_id: &str, clip_url: &str) -> Result<VodClip> {
        let vod = self
            .repository
            .own_vod(youtube_id)?
            .context("The parent VOD is not in your local library")?;
        let tokens = self.fresh_tokens().await?;
        if tokens.profile().email.is_empty()
            || vod.account_email.trim().to_lowercase() != tokens.profile().email
        {
            bail!("Only the signed-in YouTube owner can attach clips to this VOD");
        }
        let clip = self.youtube.import_youtube_clip(clip_url, &vod).await?;
        if clip.youtube_id != youtube_id {
            bail!("That YouTube Clip belongs to a different VOD");
        }
        self.repository.add_clip(clip.clone())?;
        let clips = self.repository.clips_for_vod(youtube_id)?;
        match self
            .youtube
            .update_vod_metadata(tokens.access_token(), &vod, &clips)
            .await
        {
            Ok(()) => {}
            Err(error) if auth_expired(&error) => {
                let refreshed = self.force_refresh().await?;
                self.youtube
                    .update_vod_metadata(refreshed.access_token(), &vod, &clips)
                    .await?;
            }
            Err(error) => return Err(error),
        }
        self.set_message("Imported YouTube Clip").await;
        Ok(clip)
    }

    pub(crate) async fn stop_recording(&self) -> Result<()> {
        enum StopAction {
            None,
            CancelPreparing,
            Finalize(ActiveStream),
        }

        let action = {
            let mut stream = self.stream.lock().await;
            match stream.state {
                StreamState::Idle | StreamState::Stopping => StopAction::None,
                StreamState::Preparing => {
                    stream.cancel_requested = true;
                    StopAction::CancelPreparing
                }
                StreamState::Streaming => {
                    stream.state = StreamState::Stopping;
                    stream
                        .active
                        .take()
                        .map_or(StopAction::None, StopAction::Finalize)
                }
            }
        };

        match action {
            StopAction::None => Ok(()),
            StopAction::CancelPreparing => {
                self.set_message("Cancelling stream startup…").await;
                Ok(())
            }
            StopAction::Finalize(active) => self.finalize_stream(active).await,
        }
    }

    pub(crate) async fn request_shutdown(&self) -> Result<()> {
        if self.shutdown_requested.swap(true, Ordering::AcqRel) {
            return Ok(());
        }
        let result = self.stop_recording().await;
        self.shutdown_notify.notify_waiters();
        result
    }

    pub(crate) async fn wait_shutdown(&self) {
        if self.shutdown_requested.load(Ordering::Acquire) {
            return;
        }
        self.shutdown_notify.notified().await;
    }

    pub(crate) fn data_root(&self) -> &Path {
        self.paths.root()
    }

}

#[cfg(test)]
mod controller_2_tests {
    use super::*;

    #[test]
    fn recorder_bitrate_matches_cpp_settings_range() {
        assert_eq!(validate_recorder_bitrate(2_500).expect("minimum"), 2_500);
        assert_eq!(validate_recorder_bitrate(40_000).expect("maximum"), 40_000);
        assert!(validate_recorder_bitrate(2_499).is_err());
        assert!(validate_recorder_bitrate(40_001).is_err());
    }
}
