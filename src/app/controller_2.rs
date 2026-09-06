fn validate_recorder_bitrate(value: u32) -> Result<u32> {
    if !(2_500..=40_000).contains(&value) {
        bail!("Recorder bitrate must be between 2500 and 40000 Kbps");
    }
    Ok(value)
}

fn validate_manual_game_path(executable: &Path) -> Result<()> {
    let metadata = executable.metadata().ok();
    #[cfg(target_os = "linux")]
    let is_executable = {
        use std::os::unix::fs::PermissionsExt;
        metadata.as_ref().is_some_and(|entry| entry.permissions().mode() & 0o111 != 0)
    };
    #[cfg(not(target_os = "linux"))]
    let is_executable = true;
    validate_manual_game_entry(
        executable, std::env::consts::OS,
        metadata.as_ref().is_some_and(std::fs::Metadata::is_file),
        metadata.as_ref().is_some_and(std::fs::Metadata::is_dir),
        is_executable,
    )
}

// Native metadata access stays outside the policy so Miri can exercise all
// platform rules without granting host filesystem access.
fn validate_manual_game_entry(executable: &Path, platform: &str, is_file: bool, is_dir: bool, is_executable: bool) -> Result<()> {
    let extension = executable.extension().and_then(|value| value.to_str()).unwrap_or("");
    let is_app_bundle = platform == "macos" && is_dir && extension.eq_ignore_ascii_case("app");
    if !is_file && !is_app_bundle {
        bail!("That executable does not exist.");
    }
    if platform == "windows" && !extension.eq_ignore_ascii_case("exe") {
        bail!("Please select the game's .exe file.");
    }
    if platform == "linux" && !is_executable {
        bail!("Please select an executable file.");
    }

    Ok(())
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
        validate_manual_game_path(executable)?;
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
        wait_for_shutdown(&self.shutdown_requested, &self.shutdown_notify).await;
    }

    pub(crate) fn data_root(&self) -> &Path {
        self.paths.root()
    }

}

async fn wait_for_shutdown(requested: &AtomicBool, notify: &Notify) {
    // Register before inspecting the flag: notify_waiters does not retain a
    // permit for a future waiter. Otherwise Quit can fall between the flag
    // check and registration and leave HTTP graceful shutdown asleep forever.
    let notified = notify.notified();
    tokio::pin!(notified);
    notified.as_mut().enable();
    if !requested.load(Ordering::Acquire) {
        notified.await;
    }
}

#[cfg(test)]
mod controller_2_tests {
    use super::*;

    #[test]
    fn shutdown_requested_before_wait_returns_without_another_notification() {
        use std::future::Future;
        use std::task::{Context, Waker};
        let requested = AtomicBool::new(true);
        let notify = Notify::new();
        let mut waiter = Box::pin(wait_for_shutdown(&requested, &notify));
        assert!(waiter.as_mut().poll(&mut Context::from_waker(Waker::noop())).is_ready());
    }

    #[test]
    fn shutdown_wakes_all_registered_waiters() {
        use std::future::Future;
        use std::task::{Context, Waker};

        let requested = AtomicBool::new(false);
        let notify = Notify::new();
        let mut first = Box::pin(wait_for_shutdown(&requested, &notify));
        let mut second = Box::pin(wait_for_shutdown(&requested, &notify));
        let mut cx = Context::from_waker(Waker::noop());
        assert!(first.as_mut().poll(&mut cx).is_pending());
        assert!(second.as_mut().poll(&mut cx).is_pending());
        requested.store(true, Ordering::Release);
        notify.notify_waiters();
        assert!(first.as_mut().poll(&mut cx).is_ready());
        assert!(second.as_mut().poll(&mut cx).is_ready());
    }

    #[test]
    fn recorder_bitrate_matches_cpp_settings_range() {
        assert_eq!(validate_recorder_bitrate(2_500).expect("minimum"), 2_500);
        assert_eq!(validate_recorder_bitrate(40_000).expect("maximum"), 40_000);
        assert!(validate_recorder_bitrate(2_499).is_err());
        assert!(validate_recorder_bitrate(40_001).is_err());
    }

    #[test]
    fn manual_game_rules_cover_all_platforms_without_filesystem_access() {
        for platform in ["windows", "linux", "macos"] {
            assert_eq!(validate_manual_game_entry(Path::new("missing.exe"), platform, false, false, false)
                .expect_err("missing path").to_string(), "That executable does not exist.");
        }
        assert!(validate_manual_game_entry(Path::new("game.EXE"), "windows", true, false, false).is_ok());
        assert_eq!(validate_manual_game_entry(Path::new("game.txt"), "windows", true, false, true)
            .expect_err("extension").to_string(), "Please select the game's .exe file.");
        assert!(validate_manual_game_entry(Path::new("game.APP"), "macos", false, true, false).is_ok());
        assert!(validate_manual_game_entry(Path::new("game"), "macos", true, false, false).is_ok());
        assert!(validate_manual_game_entry(Path::new("folder"), "macos", false, true, false).is_err());
        assert_eq!(validate_manual_game_entry(Path::new("game"), "linux", true, false, false)
            .expect_err("execute permission").to_string(), "Please select an executable file.");
        assert!(validate_manual_game_entry(Path::new("game"), "linux", true, false, true).is_ok());
    }

    #[cfg(not(miri))]
    #[test]
    fn manual_game_validation_rejects_missing_paths_like_cpp_picker() {
        let missing = std::env::temp_dir().join("vodlink-missing-manual-game-entry");
        assert!(validate_manual_game_path(&missing).is_err());
    }

    #[cfg(all(not(miri), target_os = "windows"))]
    #[test]
    fn manual_game_validation_requires_exe_on_windows() {
        let root = std::env::temp_dir();
        let good = root.join("vodlink-manual-game.exe");
        let bad = root.join("vodlink-manual-game.txt");
        std::fs::write(&good, b"fixture").expect("write exe fixture");
        std::fs::write(&bad, b"fixture").expect("write txt fixture");
        assert!(validate_manual_game_path(&good).is_ok());
        assert_eq!(
            validate_manual_game_path(&bad).expect_err("reject non-exe").to_string(),
            "Please select the game's .exe file."
        );
        let _ = std::fs::remove_file(good);
        let _ = std::fs::remove_file(bad);
    }

    #[cfg(all(not(miri), target_os = "linux"))]
    #[test]
    fn manual_game_validation_requires_executable_bit_on_linux() {
        use std::os::unix::fs::PermissionsExt;
        let path = std::env::temp_dir().join(format!(
            "vodlink-manual-game-{}",
            std::process::id()
        ));
        std::fs::write(&path, b"#!/bin/sh\n").expect("write fixture");
        std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o644))
            .expect("make fixture non-executable");
        assert_eq!(
            validate_manual_game_path(&path)
                .expect_err("reject non-executable")
                .to_string(),
            "Please select an executable file."
        );
        std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o755))
            .expect("make fixture executable");
        assert!(validate_manual_game_path(&path).is_ok());
        let _ = std::fs::remove_file(path);
    }

    #[cfg(all(not(miri), target_os = "macos"))]
    #[test]
    fn manual_game_validation_accepts_app_bundles_on_macos() {
        let path = std::env::temp_dir().join(format!(
            "vodlink-manual-game-{}.app",
            std::process::id()
        ));
        std::fs::create_dir_all(&path).expect("create app bundle fixture");
        assert!(validate_manual_game_path(&path).is_ok());
        let _ = std::fs::remove_dir_all(path);
    }
}
