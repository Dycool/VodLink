use crate::auth::{AuthTokens, GoogleAuth};
use crate::cloud::SessionClient;
use crate::config::Config;
use crate::games::{DetectedGame, GameCatalog, GameDetector, add_manual_game};
use crate::models::{
    AccountProfile, AppStatus, AudioCaptureSource, CaptureMode, RecorderSettings, StreamState, Vod,
    VodClip,
};
use crate::paths::AppPaths;
use crate::repository::VodRepository;
use crate::streaming::{StreamRequest, StreamerHandle};
use crate::youtube::{BroadcastSettings, PreparedBroadcast, YouTubeLiveClient, default_h264_bitrate};
use anyhow::{Context, Result, bail};
use chrono::{DateTime, Utc};
use serde::{Deserialize, Serialize};
use std::path::Path;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex as StdMutex};
use std::time::Duration;
use tokio::sync::{Mutex, Notify, RwLock};

const SHARE_SETTING: &str = "share_vods";
const AUTO_RECORD_SETTING: &str = "auto_record";
const MICROPHONE_SETTING: &str = "microphone_enabled";
const NOTIFICATIONS_SETTING: &str = "notifications";
const LAUNCH_AT_STARTUP_SETTING: &str = "launch_at_startup";
const TRAY_CLOSE_TIP_SHOWN_SETTING: &str = "tray_close_tip_shown";
const PRIVACY_SETTING: &str = "privacy_mode";
const LAST_GAME_SETTING: &str = "last_game";
const REFRESH_TOKEN_SETTING: &str = "oauth_refresh_token";
const ACCOUNT_EMAIL_SETTING: &str = "account_email";
const ENCODER_SETTING: &str = "recorder_encoder";
const BITRATE_SETTING: &str = "recorder_bitrate_kbps";
const RESOLUTION_SETTING: &str = "recorder_resolution";
const FPS_SETTING: &str = "recorder_fps";
const YOUTUBE_SYNC_SETTING: &str = "youtube_library_last_sync_ms";
const INGEST_DRAIN: Duration = Duration::from_secs(8);
const SCAN_INTERVAL: Duration = Duration::from_secs(2);

#[derive(Clone, Debug, Serialize)]
pub(crate) struct Snapshot {
    status: AppStatus,
    vods: Vec<Vod>,
    games: Vec<String>,
    friends: Vec<AccountProfile>,
    recorder: RecorderSettings,
    resolution_options: Vec<String>,
    encoder_choices: Vec<String>,
    worker_configured: bool,
    auth_configured: bool,
    stored_credentials: bool,
    startup_supported: bool,
}

#[derive(Clone, Debug, Default, Deserialize)]
pub(crate) struct SettingsUpdate {
    pub(crate) auto_record: Option<bool>,
    pub(crate) share_vods: Option<bool>,
    pub(crate) microphone: Option<bool>,
    pub(crate) notifications: Option<bool>,
    pub(crate) launch_at_startup: Option<bool>,
    pub(crate) privacy_mode: Option<String>,
    pub(crate) encoder: Option<String>,
    pub(crate) bitrate_kbps: Option<u32>,
    pub(crate) resolution: Option<String>,
    pub(crate) fps: Option<u32>,
}

#[derive(Default)]
struct StreamRuntime {
    state: StreamState,
    pending_game: String,
    cancel_requested: bool,
    active: Option<ActiveStream>,
}

struct ActiveStream {
    game: String,
    prepared: PreparedBroadcast,
    started_at: DateTime<Utc>,
    share_announced: bool,
}

pub(crate) struct AppController {
    config: Config,
    paths: AppPaths,
    repository: VodRepository,
    auth: GoogleAuth,
    session: SessionClient,
    youtube: YouTubeLiveClient,
    streamer: StreamerHandle,
    detector: StdMutex<GameDetector>,
    tokens: RwLock<AuthTokens>,
    status: RwLock<AppStatus>,
    stream: Mutex<StreamRuntime>,
    explicitly_signed_out: AtomicBool,
    shutdown_requested: AtomicBool,
    shutdown_notify: Notify,
}

#[cfg(feature = "desktop")]
impl AppController {
    pub(crate) async fn tray_state(&self) -> (bool, bool, bool, String) {
        let (auto_record, share_vods, tooltip) = {
            let status = self.status.read().await;
            (
                status.auto_record,
                status.share_vods,
                status.message.clone(),
            )
        };
        let recording = self.stream.lock().await.state != StreamState::Idle;
        (auto_record, share_vods, recording, tooltip)
    }

    pub(crate) fn tray_close_tip_needed(&self) -> Result<bool> {
        Ok(read_bool(&self.repository, NOTIFICATIONS_SETTING, true)?
            && !read_bool(&self.repository, TRAY_CLOSE_TIP_SHOWN_SETTING, false)?)
    }

    pub(crate) fn mark_tray_close_tip_shown(&self) -> Result<()> {
        self.repository
            .set_setting(TRAY_CLOSE_TIP_SHOWN_SETTING, "1")
    }
}

include!("controller_1.rs");
include!("controller_2.rs");
include!("controller_3.rs");
include!("controller_4.rs");
include!("controller_reset.rs");
include!("helpers.rs");