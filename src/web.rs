use crate::app::{AppController, SettingsUpdate};
use crate::models::VodClip;
use anyhow::{Context, Result};
use axum::extract::{Path, State};
use axum::http::{StatusCode, header};
use axum::response::{Html, IntoResponse, Response};
use axum::routing::{delete, get, post};
use axum::{Json, Router};
use serde::{Deserialize, Serialize};
use std::sync::{Arc, OnceLock};

const INDEX_HTML: &str = include_str!("../resources/web/index.html");
const APP_JS: &str = include_str!("../resources/web/app.js");
const STYLES_CSS: &str = include_str!("../resources/web/styles.css");
pub(crate) const UI_ADDRESS: &str = "127.0.0.1:43861";

type UiHandler = Arc<dyn Fn() + Send + Sync + 'static>;
static SHOW_WINDOW_HANDLER: OnceLock<UiHandler> = OnceLock::new();
static EXIT_HANDLER: OnceLock<UiHandler> = OnceLock::new();

#[derive(Debug)]
struct ApiError(anyhow::Error);

impl From<anyhow::Error> for ApiError {
    fn from(error: anyhow::Error) -> Self {
        Self(error)
    }
}

impl IntoResponse for ApiError {
    fn into_response(self) -> Response {
        (
            StatusCode::BAD_REQUEST,
            Json(ApiMessage {
                ok: false,
                message: self.0.to_string(),
            }),
        )
            .into_response()
    }
}

#[derive(Serialize)]
struct ApiMessage {
    ok: bool,
    message: String,
}

impl ApiMessage {
    fn ok(message: impl Into<String>) -> Json<Self> {
        Json(Self {
            ok: true,
            message: message.into(),
        })
    }
}

#[derive(Deserialize)]
struct FriendRequest {
    email: String,
}

#[derive(Deserialize)]
struct GameRequest {
    executable: String,
    name: String,
}

#[derive(Deserialize)]
struct ClipRequest {
    youtube_id: String,
    url: String,
}

pub(crate) fn ui_url() -> String {
    format!("http://{UI_ADDRESS}/")
}

pub(crate) fn register_show_window_handler(handler: UiHandler) -> Result<()> {
    SHOW_WINDOW_HANDLER
        .set(handler)
        .map_err(|_| anyhow::anyhow!("VodLink window handler was already registered"))
}

pub(crate) fn register_exit_handler(handler: UiHandler) -> Result<()> {
    EXIT_HANDLER
        .set(handler)
        .map_err(|_| anyhow::anyhow!("VodLink exit handler was already registered"))
}

pub(crate) fn bind_ui() -> std::io::Result<std::net::TcpListener> {
    let listener = std::net::TcpListener::bind(UI_ADDRESS)?;
    listener.set_nonblocking(true)?;
    Ok(listener)
}

pub(crate) async fn existing_instance(show: bool) -> bool {
    let client = reqwest::Client::new();
    let ping = client
        .get(format!("http://{UI_ADDRESS}/api/ping"))
        .timeout(std::time::Duration::from_secs(2))
        .send()
        .await;

    let Ok(response) = ping else {
        return false;
    };
    if !response.status().is_success() {
        return false;
    }

    if show {
        let _ = client
            .post(format!("http://{UI_ADDRESS}/api/window/show"))
            .timeout(std::time::Duration::from_secs(2))
            .send()
            .await;
    }
    true
}

fn router(controller: Arc<AppController>) -> Router {
    Router::new()
        .route("/", get(index))
        .route("/app.js", get(javascript))
        .route("/styles.css", get(styles))
        .route("/api/ping", get(ping))
        .route("/api/window/show", post(show_window))
        .route("/api/snapshot", get(snapshot))
        .route("/api/sign-in", post(sign_in))
        .route("/api/sign-out", post(sign_out))
        .route("/api/sync", post(sync_library))
        .route("/api/settings", post(update_settings))
        .route("/api/friends", post(add_friend))
        .route("/api/friends/{email}", delete(remove_friend))
        .route("/api/games", post(add_game))
        .route("/api/record/stop", post(stop_recording))
        .route("/api/vods/{youtube_id}", delete(delete_vod))
        .route("/api/friend-vods/{youtube_id}", delete(remove_friend_vod))
        .route("/api/clips/{youtube_id}", get(clips_for_vod))
        .route("/api/clips/import", post(import_clip))
        .route("/api/data-root", get(data_root))
        .route("/api/shutdown", post(shutdown))
        .with_state(controller)
}

pub(crate) async fn serve_bound(
    controller: Arc<AppController>,
    listener: std::net::TcpListener,
) -> Result<()> {
    let listener = tokio::net::TcpListener::from_std(listener)
        .context("Could not attach VodLink UI listener to the async runtime")?;
    let shutdown_controller = controller.clone();
    axum::serve(listener, router(controller))
        .with_graceful_shutdown(async move {
            shutdown_controller.wait_shutdown().await;
        })
        .await
        .context("VodLink local UI server failed")
}

pub(crate) async fn serve(controller: Arc<AppController>, start_minimized: bool) -> Result<()> {
    let listener = match bind_ui() {
        Ok(listener) => listener,
        Err(error) if error.kind() == std::io::ErrorKind::AddrInUse => {
            if existing_instance(!start_minimized).await {
                if !start_minimized {
                    let _ = webbrowser::open(&ui_url());
                }
                return Ok(());
            }
            return Err(error).context("VodLink UI port is already occupied by another application");
        }
        Err(error) => return Err(error).context("Could not bind the VodLink local UI server"),
    };

    if !start_minimized {
        webbrowser::open(&ui_url())
            .context("Could not open the VodLink interface in the default browser")?;
    }

    let monitor = tokio::spawn(controller.clone().run_monitor());
    let result = serve_bound(controller, listener).await;
    monitor.abort();
    result
}

async fn index() -> Html<&'static str> {
    Html(INDEX_HTML)
}

async fn javascript() -> impl IntoResponse {
    ([(header::CONTENT_TYPE, "text/javascript; charset=utf-8")], APP_JS)
}

async fn styles() -> impl IntoResponse {
    ([(header::CONTENT_TYPE, "text/css; charset=utf-8")], STYLES_CSS)
}

async fn ping() -> Json<ApiMessage> {
    ApiMessage::ok("VodLink")
}

async fn show_window() -> Json<ApiMessage> {
    if let Some(handler) = SHOW_WINDOW_HANDLER.get() {
        handler();
    }
    ApiMessage::ok("VodLink window requested")
}

async fn snapshot(
    State(controller): State<Arc<AppController>>,
) -> Result<impl IntoResponse, ApiError> {
    Ok(Json(controller.snapshot().await?))
}

async fn sign_in(
    State(controller): State<Arc<AppController>>,
) -> Result<impl IntoResponse, ApiError> {
    controller.sign_in().await?;
    Ok(ApiMessage::ok("Signed in"))
}

async fn sign_out(
    State(controller): State<Arc<AppController>>,
) -> Result<impl IntoResponse, ApiError> {
    controller.sign_out().await?;
    Ok(ApiMessage::ok("Signed out"))
}

async fn sync_library(
    State(controller): State<Arc<AppController>>,
) -> Result<impl IntoResponse, ApiError> {
    controller.sync_library().await?;
    Ok(ApiMessage::ok("Library synced"))
}

async fn update_settings(
    State(controller): State<Arc<AppController>>,
    Json(update): Json<SettingsUpdate>,
) -> Result<impl IntoResponse, ApiError> {
    controller.update_settings(update).await?;
    Ok(ApiMessage::ok("Settings saved"))
}

async fn add_friend(
    State(controller): State<Arc<AppController>>,
    Json(request): Json<FriendRequest>,
) -> Result<impl IntoResponse, ApiError> {
    controller.add_friend(&request.email).await?;
    Ok(ApiMessage::ok("Friend added"))
}

async fn remove_friend(
    State(controller): State<Arc<AppController>>,
    Path(email): Path<String>,
) -> Result<impl IntoResponse, ApiError> {
    controller.remove_friend(&email).await?;
    Ok(ApiMessage::ok("Friend removed"))
}

async fn add_game(
    State(controller): State<Arc<AppController>>,
    Json(request): Json<GameRequest>,
) -> Result<impl IntoResponse, ApiError> {
    controller
        .add_manual_game(&request.executable, &request.name)
        .await?;
    Ok(ApiMessage::ok("Game added"))
}

async fn stop_recording(
    State(controller): State<Arc<AppController>>,
) -> Result<impl IntoResponse, ApiError> {
    controller.stop_recording().await?;
    Ok(ApiMessage::ok("Recording stopped"))
}

async fn delete_vod(
    State(controller): State<Arc<AppController>>,
    Path(youtube_id): Path<String>,
) -> Result<impl IntoResponse, ApiError> {
    controller.delete_vod(&youtube_id).await?;
    Ok(ApiMessage::ok("VOD deleted"))
}

async fn remove_friend_vod(
    State(controller): State<Arc<AppController>>,
    Path(youtube_id): Path<String>,
) -> Result<impl IntoResponse, ApiError> {
    controller.remove_friend_vod(&youtube_id).await?;
    Ok(ApiMessage::ok("Friend VOD removed"))
}

async fn clips_for_vod(
    State(controller): State<Arc<AppController>>,
    Path(youtube_id): Path<String>,
) -> Result<Json<Vec<VodClip>>, ApiError> {
    Ok(Json(controller.clips_for_vod(&youtube_id)?))
}

async fn import_clip(
    State(controller): State<Arc<AppController>>,
    Json(request): Json<ClipRequest>,
) -> Result<Json<VodClip>, ApiError> {
    Ok(Json(
        controller
            .import_clip(&request.youtube_id, &request.url)
            .await?,
    ))
}

async fn data_root(State(controller): State<Arc<AppController>>) -> Json<ApiMessage> {
    ApiMessage::ok(controller.data_root().display().to_string())
}

async fn shutdown(
    State(controller): State<Arc<AppController>>,
) -> Result<impl IntoResponse, ApiError> {
    controller.request_shutdown().await?;
    if let Some(handler) = EXIT_HANDLER.get() {
        handler();
    }
    Ok(ApiMessage::ok("VodLink is shutting down"))
}
