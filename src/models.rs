use chrono::{DateTime, Utc};
use serde::{Deserialize, Serialize};

#[derive(Clone, Debug, Default, Serialize, Deserialize, PartialEq, Eq)]
pub(crate) struct AccountProfile {
    pub(crate) email: String,
    pub(crate) display_name: String,
    pub(crate) picture_url: String,
}

impl AccountProfile {
    pub(crate) fn new(email: impl Into<String>, display_name: impl Into<String>, picture_url: impl Into<String>) -> Self {
        Self {
            email: email.into().trim().to_lowercase(),
            display_name: display_name.into().trim().to_owned(),
            picture_url: picture_url.into().trim().to_owned(),
        }
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub(crate) struct Vod {
    pub(crate) id: i64,
    pub(crate) game: String,
    pub(crate) youtube_id: String,
    pub(crate) stream_status: String,
    pub(crate) started_at: DateTime<Utc>,
    pub(crate) duration_ms: i64,
    pub(crate) account_email: String,
    pub(crate) owner_email: String,
    pub(crate) owner_name: String,
    pub(crate) owner_picture_url: String,
    pub(crate) title: String,
}

impl Vod {
    pub(crate) fn own(game: impl Into<String>, youtube_id: impl Into<String>, started_at: DateTime<Utc>) -> Self {
        Self {
            id: 0,
            game: game.into(),
            youtube_id: youtube_id.into(),
            stream_status: "processing".to_owned(),
            started_at,
            duration_ms: 0,
            account_email: String::new(),
            owner_email: String::new(),
            owner_name: String::new(),
            owner_picture_url: String::new(),
            title: String::new(),
        }
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub(crate) struct VodClip {
    pub(crate) id: i64,
    pub(crate) youtube_id: String,
    pub(crate) clip_id: String,
    pub(crate) clip_url: String,
    pub(crate) title: String,
    pub(crate) start_seconds: i32,
    pub(crate) end_seconds: i32,
    pub(crate) created_at: DateTime<Utc>,
}

impl VodClip {
    pub(crate) fn normalize(mut self) -> Self {
        self.start_seconds = self.start_seconds.max(0);
        self.end_seconds = self.end_seconds.max(self.start_seconds + 1).min(self.start_seconds + 120);
        if self.title.trim().is_empty() {
            self.title = format!("Clip {}s–{}s", self.start_seconds, self.end_seconds);
        }
        self
    }
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub(crate) struct GameDefinition {
    pub(crate) name: String,
    pub(crate) process_names: Vec<String>,
}

impl GameDefinition {
    pub(crate) fn new(name: impl Into<String>, process_names: impl IntoIterator<Item = impl Into<String>>) -> Self {
        let mut names = process_names
            .into_iter()
            .map(|p| p.into().trim().to_lowercase())
            .filter(|p| !p.is_empty())
            .collect::<Vec<_>>();
        names.sort();
        names.dedup();
        Self { name: name.into().trim().to_owned(), process_names: names }
    }
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub(crate) struct InstalledGame {
    pub(crate) name: String,
    pub(crate) install_dir: String,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub(crate) struct AppStatus {
    pub(crate) signed_in_email: String,
    pub(crate) signed_in_name: String,
    pub(crate) signed_in_picture: String,
    pub(crate) auto_record: bool,
    pub(crate) share_vods: bool,
    pub(crate) microphone: bool,
    pub(crate) notifications: bool,
    pub(crate) launch_at_startup: bool,
    pub(crate) privacy_mode: String,
    pub(crate) current_game: String,
    pub(crate) last_game: String,
    pub(crate) streaming: bool,
    pub(crate) message: String,
    pub(crate) error: String,
}

impl Default for AppStatus {
    fn default() -> Self {
        Self {
            signed_in_email: String::new(),
            signed_in_name: String::new(),
            signed_in_picture: String::new(),
            auto_record: false,
            share_vods: true,
            microphone: false,
            notifications: true,
            launch_at_startup: false,
            privacy_mode: "game_external_audio".to_owned(),
            current_game: String::new(),
            last_game: String::new(),
            streaming: false,
            message: "Watching for games".to_owned(),
            error: String::new(),
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) enum StreamState {
    #[default]
    Idle,
    Preparing,
    Streaming,
    Stopping,
}

#[derive(Clone, Copy, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub(crate) enum CaptureMode {
    GameWindow,
    FullDesktop,
}

#[derive(Clone, Copy, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub(crate) enum AudioCaptureSource {
    GameOnly,
    System,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub(crate) struct RecorderSettings {
    pub(crate) encoder: String,
    pub(crate) bitrate_kbps: u32,
    pub(crate) width: u32,
    pub(crate) height: u32,
    pub(crate) fps: u32,
}