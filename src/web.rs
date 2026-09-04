use crate::app::{AppController, SettingsUpdate};
use crate::models::VodClip;
use anyhow::{Context, Result};
use axum::extract::{Path, State};
use axum::http::{StatusCode, header};
use axum::response::{Html, IntoResponse, Response};
use axum::routing::{delete, get, post};
use axum::{Json, Router};
use serde::{Deserialize, Serialize};
use std::sync::Arc;

const INDEX_HTML: &str = include_str!("../resources/web/index.html");
const APP_JS: &str = include_str!("../resources/web/app.js");
const STYLES_CSS: &str = include_str!("../resources/web/styles.css");

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

pub(crate) async fn serve(controller: Arc<AppController>, start_minimized: bool) -> Result<()> {
    let router = Router::new()
        .route("/", get(index))
        .route("/app.js", get(javascript))
        .route("/styles.css", get(styles))
        .route("/api/ping", get(ping))
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
        .with_state(controller.clone());

    let address = "127.0.0.1:43861";
    let listener = match tokio::net::TcpListener::bind(address).await {
        Ok(listener) => listener,
        Err(error) if error.kind() == std::io::ErrorKind::AddrInUse => {
            let url = format!("http://{address}/api/ping");
            let existing = reqwest::Client::new()
                .get(url)
                .timeout(std::time::Duration::from_secs(2))
                .send()
                .await
                .is_ok_and(|response| response.status().is_success());
            if existing {
                if !start_minimized {
                    let _ = webbrowser::open(&format!("http://{address}/"));
                }
                return Ok(());
            }
            return Err(error).context("VodLink UI port is already occupied by another application");
        }
        Err(error) => return Err(error).context("Could not bind the VodLink local UI server"),
    };

    if !start_minimized {
        webbrowser::open(&format!("http://{address}/"))
            .context("Could not open the VodLink interface in the default browser")?;
    }

    let monitor = tokio::spawn(controller.clone().run_monitor());
    let shutdown_controller = controller.clone();
    let server = axum::serve(listener, router).with_graceful_shutdown(async move {
        tokio::select! {
            _ = shutdown_controller.wait_shutdown() => {}
            _ = tokio::signal::ctrl_c() => {
                let _ = shutdown_controller.request_shutdown().await;
            }
        }
    });
    server.await.context("VodLink local UI server failed")?;
    monitor.abort();
    Ok(())
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

async fn snapshot(State(controller): State<Arc<AppController>>) -> Result<impl IntoResponse, ApiError> {
    Ok(Json(controller.snapshot().await?))
}

async fn sign_in(State(controller): State<Arc<AppController>>) -> Result<impl IntoResponse, ApiError> {
    controller.sign_in().await?;
    Ok(ApiMessage::ok("Signed in"))
}

async fn sign_out(State(controller): State<Arc<AppController>>) -> Result<impl IntoResponse, ApiError> {
    controller.sign_out().await?;
    Ok(ApiMessage::ok("Signed out"))
}

async fn sync_library(State(controller): State<Arc<AppController>>) -> Result<impl IntoResponse, ApiError> {
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
    controller.add_manual_game(&request.executable, &request.name).await?;
    Ok(ApiMessage::ok("Game added"))
}

async fn stop_recording(State(controller): State<Arc<AppController>>) -> Result<impl IntoResponse, ApiError> {
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
    Ok(Json(controller.import_clip(&request.youtube_id, &request.url).await?))
}

async fn data_root(State(controller): State<Arc<AppController>>) -> Json<ApiMessage> {
    ApiMessage::ok(controller.data_root().display().to_string())
}

async fn shutdown(State(controller): State<Arc<AppController>>) -> Result<impl IntoResponse, ApiError> {
    controller.request_shutdown().await?;
    Ok(ApiMessage::ok("VodLink is shutting down"))
}
