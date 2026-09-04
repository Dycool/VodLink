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
        let resolution = self
            .repository
            .setting(RESOLUTION_SETTING)?
            .unwrap_or_else(|| "1920x1080".to_owned());
        let (width, height) = parse_resolution(&resolution).unwrap_or((1920, 1080));
        let fps = self
            .repository
            .setting(FPS_SETTING)?
            .and_then(|value| value.parse::<u32>().ok())
            .filter(|value| matches!(value, 30 | 60))
            .unwrap_or(60);
        let bitrate_kbps = self
            .repository
            .setting(BITRATE_SETTING)?
            .and_then(|value| value.parse::<u32>().ok())
            .filter(|value| *value > 0)
            .unwrap_or_else(|| default_h264_bitrate(width, height, fps));
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
