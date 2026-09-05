#![forbid(unsafe_code)]

mod app;
mod auth;
mod cloud;
mod config;
#[cfg(feature = "desktop")]
mod desktop;
mod games;
mod models;
mod paths;
mod repository;
mod startup;
#[cfg(feature = "obs")]
mod streaming;
#[cfg(not(feature = "obs"))]
#[path = "streaming_stub.rs"]
mod streaming;
#[cfg(all(feature = "desktop", target_os = "windows"))]
mod updater;
mod web;
mod youtube;

#[cfg(not(feature = "desktop"))]
use anyhow::Context;
use anyhow::Result;
use tracing_subscriber::EnvFilter;

pub fn run() -> Result<()> {
    let filter =
        EnvFilter::try_from_default_env().unwrap_or_else(|_| EnvFilter::new("vodlink=info"));
    let _ = tracing_subscriber::fmt()
        .with_env_filter(filter)
        .try_init();

    let start_minimized = std::env::args().any(|argument| {
        argument.eq_ignore_ascii_case("--minimized")
            || argument.eq_ignore_ascii_case("--startup")
    });

    run_frontend(start_minimized)
}

#[cfg(feature = "desktop")]
fn run_frontend(start_minimized: bool) -> Result<()> {
    desktop::run(start_minimized)
}

#[cfg(not(feature = "desktop"))]
fn run_frontend(start_minimized: bool) -> Result<()> {
    let runtime = tokio::runtime::Builder::new_multi_thread()
        .enable_all()
        .build()
        .context("Could not initialize the VodLink async runtime")?;
    runtime.block_on(async move {
        let controller = app::AppController::new().await?;
        let restore_controller = controller.clone();
        std::mem::drop(tokio::spawn(async move {
            restore_controller.restore_stored_credentials().await;
        }));
        web::serve(controller, start_minimized).await
    })
}
