use crate::models::{Vod, VodClip};
use anyhow::{Context, Result, bail};
use chrono::{DateTime, Duration as ChronoDuration, Local, Utc};
use regex::Regex;
use reqwest::{Client, Method, StatusCode};
use serde_json::{Value, json};
use std::collections::HashMap;
use std::time::Duration;
use url::Url;

const API_BASE: &str = "https://www.googleapis.com/youtube/v3";
const MARKER_START: &str = "[VodLink]";
const MARKER_END: &str = "[/VodLink]";
const DEFAULT_DESCRIPTION: &str = "Automatically captured by VodLink";

#[derive(Clone, Debug)]
pub(crate) struct PreparedBroadcast {
    pub(crate) broadcast_id: String,
    pub(crate) stream_id: String,
    pub(crate) youtube_id: String,
    pub(crate) rtmp_server: String,
    pub(crate) stream_key: String,
}

#[derive(Clone, Debug)]
pub(crate) struct BroadcastSettings {
    pub(crate) width: u32,
    pub(crate) height: u32,
    pub(crate) fps: u32,
    pub(crate) game: String,
}

#[derive(Clone, Debug)]
pub(crate) struct SyncedVod {
    pub(crate) vod: Vod,
    pub(crate) clips: Vec<VodClip>,
}

#[derive(Clone)]
pub(crate) struct YouTubeLiveClient {
    client: Client,
}

include!("client_1.rs");
include!("client_2.rs");
include!("client_3.rs");
include!("client_4.rs");
include!("helpers.rs");
