use crate::app::AppController;
use anyhow::{Context, Result};
use reqwest::header::{ACCEPT, USER_AGENT};
use serde_json::Value;
use sha2::{Digest, Sha256};
use std::path::PathBuf;
use std::process::{Command, Stdio};
use std::sync::Arc;
use std::time::Duration;

const LATEST_RELEASE_URL: &str = "https://api.github.com/repos/Dycool/VodLink/releases/latest";
const INSTALLER_ASSET: &str = "VodLink-Windows-x64.exe";

pub(crate) async fn check_and_launch(
    controller: Arc<AppController>,
    start_minimized: bool,
) -> Result<bool> {
    let current_tag = option_env!("VODLINK_RELEASE_TAG").unwrap_or("").trim();
    if current_tag.is_empty() {
        let commit = option_env!("VODLINK_BUILD_COMMIT").unwrap_or("dev");
        tracing::info!(commit, "Unreleased build; public release check disabled");
        return Ok(false);
    }

    tokio::time::sleep(Duration::from_secs(5)).await;

    let client = reqwest::Client::builder()
        .timeout(Duration::from_secs(30))
        .build()
        .context("Could not initialize the VodLink updater HTTP client")?;
    let response = client
        .get(LATEST_RELEASE_URL)
        .header(ACCEPT, "application/vnd.github+json")
        .header(USER_AGENT, "VodLink-Updater")
        .send()
        .await
        .context("Latest release check failed")?;
    if !response.status().is_success() {
        tracing::warn!(status = %response.status(), "Latest release check failed");
        return Ok(false);
    }
    let release: Value = response.json().await.context("Invalid GitHub release metadata")?;
    let latest_tag = release
        .get("tag_name")
        .and_then(Value::as_str)
        .unwrap_or_default()
        .trim();
    if latest_tag.is_empty() || latest_tag == current_tag {
        return Ok(false);
    }

    let (_, _, recording, _) = controller.tray_state().await;
    if recording {
        tracing::info!("Update deferred until the next launch because a stream is active");
        return Ok(false);
    }

    let asset = release
        .get("assets")
        .and_then(Value::as_array)
        .and_then(|assets| {
            assets.iter().find(|asset| {
                asset.get("name").and_then(Value::as_str) == Some(INSTALLER_ASSET)
            })
        });
    let Some(asset) = asset else {
        tracing::warn!("Latest release has no verifiable Windows installer");
        return Ok(false);
    };
    let download_url = asset
        .get("browser_download_url")
        .and_then(Value::as_str)
        .unwrap_or_default();
    let digest = asset
        .get("digest")
        .and_then(Value::as_str)
        .unwrap_or_default();
    let Some(expected_hash) = digest.strip_prefix("sha256:") else {
        tracing::warn!("Latest release has no verifiable Windows installer");
        return Ok(false);
    };
    if download_url.is_empty() || expected_hash.len() != 64 {
        tracing::warn!("Latest release has no verifiable Windows installer");
        return Ok(false);
    }

    let response = client
        .get(download_url)
        .header(USER_AGENT, "VodLink-Updater")
        .send()
        .await
        .context("Release installer download failed")?;
    if !response.status().is_success() {
        tracing::warn!(status = %response.status(), "Release installer download failed");
        return Ok(false);
    }
    let installer = response
        .bytes()
        .await
        .context("Could not read the release installer")?;
    let actual_hash = format!("{:x}", Sha256::digest(&installer));
    if !actual_hash.eq_ignore_ascii_case(expected_hash) {
        tracing::warn!("Release installer digest verification failed");
        return Ok(false);
    }

    let installer_path = updater_path(latest_tag)?;
    if let Some(parent) = installer_path.parent() {
        std::fs::create_dir_all(parent).context("Could not create the VodLink updater directory")?;
    }
    std::fs::write(&installer_path, &installer)
        .context("Could not save the verified VodLink release installer")?;

    let mut command = Command::new(&installer_path);
    command.arg("--update-background");
    if start_minimized {
        command.arg("--launch-minimized");
    }
    command
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
        .context("Could not launch the verified VodLink update")?;

    tracing::info!(from = current_tag, to = latest_tag, "Handing off VodLink update");
    controller.request_shutdown().await?;
    Ok(true)
}

fn updater_path(tag: &str) -> Result<PathBuf> {
    let safe_tag = tag
        .chars()
        .map(|character| {
            if character.is_ascii_alphanumeric() || matches!(character, '.' | '_' | '-') {
                character
            } else {
                '_'
            }
        })
        .collect::<String>();
    Ok(std::env::temp_dir()
        .join("VodLink-Updater")
        .join(safe_tag)
        .join(INSTALLER_ASSET))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn updater_tag_is_sanitized_like_cpp() {
        let path = updater_path("v1/2:3").expect("path");
        assert!(path.to_string_lossy().contains("v1_2_3"));
    }
}
