fn recommended_recorder_bitrate(width: u32, height: u32, fps: u32, encoder: &str) -> u32 {
    let h264 = default_h264_bitrate(width, height, fps);
    let efficient = {
        let normalized = encoder.trim().to_lowercase();
        normalized.contains("av1") || normalized.contains("hevc") || normalized.contains("265")
    };
    if !efficient {
        return h264;
    }

    let pixels = u64::from(width) * u64::from(height);
    let high = fps >= 50;
    let (av_min, av_max) = if pixels > 2560_u64 * 1440 {
        if high { (10_000, 40_000) } else { (8_000, 35_000) }
    } else if pixels > 1920_u64 * 1080 {
        if high { (6_000, 30_000) } else { (5_000, 25_000) }
    } else if pixels > 1280_u64 * 720 {
        if high { (4_000, 10_000) } else { (3_000, 8_000) }
    } else {
        (3_000, 8_000)
    };
    h264.clamp(av_min, av_max)
}

fn initial_recorder_bitrate(
    saved: Option<&str>,
    width: u32,
    height: u32,
    fps: u32,
    encoder: &str,
) -> (u32, bool) {
    if let Some(value) = saved
        && let Ok(parsed) = value.trim().parse::<i32>()
    {
        return (parsed.clamp(2500, 40_000) as u32, false);
    }

    (
        recommended_recorder_bitrate(width, height, fps, encoder).clamp(2500, 40_000),
        true,
    )
}

impl AppController {
    async fn fresh_tokens(&self) -> Result<AuthTokens> {
        let current = self.tokens.read().await.clone();
        if current.is_signed_in() && !current.needs_refresh() {
            return Ok(current);
        }
        if current.refresh_token().is_empty() {
            return Ok(current);
        }
        self.force_refresh().await
    }

    async fn force_refresh(&self) -> Result<AuthTokens> {
        let refresh = self.tokens.read().await.refresh_token().to_owned();
        if refresh.is_empty() {
            bail!("Google sign-in expired and no refresh token is available");
        }
        let refreshed = self.auth.refresh(&refresh).await?;
        self.store_tokens(refreshed.clone()).await?;
        Ok(refreshed)
    }

    async fn store_tokens(&self, tokens: AuthTokens) -> Result<()> {
        if !tokens.refresh_token().is_empty() {
            self.repository
                .set_setting(REFRESH_TOKEN_SETTING, tokens.refresh_token())?;
        }
        *self.tokens.write().await = tokens;
        Ok(())
    }

    async fn adopt_account(&self, profile: &AccountProfile) -> Result<()> {
        let email = profile.email.trim().to_lowercase();
        if email.is_empty() {
            return Ok(());
        }
        let previous = self
            .repository
            .setting(ACCOUNT_EMAIL_SETTING)?
            .unwrap_or_default()
            .to_lowercase();
        if !previous.is_empty() && previous != email {
            self.repository.clear_account_data()?;
            self.set_message(
                "Switched Google account — cleared the previous account's friend data and kept cached VODs",
            )
            .await;
        }
        self.repository.set_setting(ACCOUNT_EMAIL_SETTING, &email)?;
        Ok(())
    }

    async fn apply_profile(&self, profile: &AccountProfile) {
        let mut status = self.status.write().await;
        status.signed_in_email = profile.email.clone();
        status.signed_in_name = profile.display_name.clone();
        status.signed_in_picture = profile.picture_url.clone();
        status.error.clear();
    }

    fn recorder_settings(&self) -> Result<RecorderSettings> {
        let encoder = normalized_encoder(
            self.repository
                .setting(ENCODER_SETTING)?
                .as_deref()
                .unwrap_or("H.264"),
        )?;
        let native = native_recorder_resolution();
        let resolution = self
            .repository
            .setting(RESOLUTION_SETTING)?
            .unwrap_or_else(|| format!("{}x{}", native.0, native.1));
        let (width, height) = parse_resolution(&resolution).unwrap_or(native);
        let fps = self
            .repository
            .setting(FPS_SETTING)?
            .and_then(|value| value.parse::<u32>().ok())
            .filter(|value| matches!(value, 30 | 60))
            .unwrap_or(60);
        let saved_bitrate = self.repository.setting(BITRATE_SETTING)?;
        let (bitrate_kbps, persist_default_bitrate) = initial_recorder_bitrate(
            saved_bitrate.as_deref(),
            width,
            height,
            fps,
            &encoder,
        );
        if persist_default_bitrate {
            self.repository
                .set_setting(BITRATE_SETTING, &bitrate_kbps.to_string())?;
        }
        Ok(RecorderSettings {
            encoder,
            bitrate_kbps,
            width,
            height,
            fps,
        })
    }

    async fn reset_stream_state(&self) {
        {
            let mut stream = self.stream.lock().await;
            stream.state = StreamState::Idle;
            stream.pending_game.clear();
            stream.cancel_requested = false;
            stream.active = None;
        }
        let mut status = self.status.write().await;
        status.current_game.clear();
        status.streaming = false;
        if status.error.is_empty() {
            status.message = "Watching for games".to_owned();
        }
    }

    async fn set_message(&self, message: impl Into<String>) {
        let mut status = self.status.write().await;
        status.message = message.into();
        status.error.clear();
    }

    async fn set_error(&self, error: impl Into<String>) {
        self.status.write().await.error = error.into();
    }
}

#[cfg(test)]
mod controller_4_tests {
    use super::*;

    #[test]
    fn recorder_recommendation_matches_cpp_youtube_ladder() {
        assert_eq!(recommended_recorder_bitrate(1920, 1080, 60, "H.264"), 12_000);
        assert_eq!(recommended_recorder_bitrate(1920, 1080, 60, "HEVC"), 10_000);
        assert_eq!(recommended_recorder_bitrate(1920, 1080, 30, "AV1"), 8_000);
        assert_eq!(recommended_recorder_bitrate(2560, 1440, 60, "HEVC"), 24_000);
        assert_eq!(recommended_recorder_bitrate(3840, 2160, 60, "AV1"), 35_000);
        assert_eq!(recommended_recorder_bitrate(1280, 720, 30, "HEVC"), 4_000);
    }

    #[test]
    fn recorder_recommendation_uses_cpp_pixel_tiers_for_ultrawide_modes() {
        assert_eq!(recommended_recorder_bitrate(3440, 1440, 60, "H.264"), 35_000);
        assert_eq!(recommended_recorder_bitrate(2560, 1080, 60, "H.264"), 24_000);
    }

    #[test]
    fn saved_numeric_bitrate_is_clamped_like_cpp_spinbox() {
        assert_eq!(initial_recorder_bitrate(Some("2499"), 1920, 1080, 60, "H.264"), (2500, false));
        assert_eq!(initial_recorder_bitrate(Some("40001"), 1920, 1080, 60, "H.264"), (40_000, false));
        assert_eq!(initial_recorder_bitrate(Some(" -5 "), 1920, 1080, 60, "H.264"), (2500, false));
        assert_eq!(initial_recorder_bitrate(Some("12000"), 1920, 1080, 60, "H.264"), (12_000, false));
    }

    #[test]
    fn missing_or_non_numeric_bitrate_uses_and_persists_cpp_default() {
        assert_eq!(initial_recorder_bitrate(None, 1920, 1080, 60, "H.264"), (12_000, true));
        assert_eq!(initial_recorder_bitrate(Some(""), 1920, 1080, 60, "HEVC"), (10_000, true));
        assert_eq!(initial_recorder_bitrate(Some("garbage"), 3840, 2160, 60, "AV1"), (35_000, true));
        assert_eq!(initial_recorder_bitrate(Some("9999999999"), 1920, 1080, 60, "H.264"), (12_000, true));
    }
}
