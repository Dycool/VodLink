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
    let response = match client
        .get(LATEST_RELEASE_URL)
        .header(ACCEPT, "application/vnd.github+json")
        .header(USER_AGENT, "VodLink-Updater")
        .send()
        .await
    {
        Ok(response) => response,
        Err(_) => {
            tracing::warn!("Latest release check failed");
            return Ok(false);
        }
    };
    if !response.status().is_success() {
        tracing::warn!("Latest release check failed");
        return Ok(false);
    }
    let release: Value = match response.json().await {
        Ok(release) => release,
        Err(_) => return Ok(false),
    };
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

    let Some((download_url, digest)) = release_installer(&release) else {
        tracing::warn!("Latest release has no verifiable Windows installer");
        return Ok(false);
    };

    let response = match client
        .get(download_url)
        .header(USER_AGENT, "VodLink-Updater")
        .send()
        .await
    {
        Ok(response) => response,
        Err(_) => {
            tracing::warn!("Release installer download or digest verification failed");
            return Ok(false);
        }
    };
    if !response.status().is_success() {
        tracing::warn!("Release installer download or digest verification failed");
        return Ok(false);
    }
    let installer = match response.bytes().await {
        Ok(installer) => installer,
        Err(_) => {
            tracing::warn!("Release installer download or digest verification failed");
            return Ok(false);
        }
    };
    if !installer_digest_matches(&installer, digest) {
        tracing::warn!("Release installer download or digest verification failed");
        return Ok(false);
    }

    let installer_path = updater_path(latest_tag)?;
    if let Some(parent) = installer_path.parent()
        && std::fs::create_dir_all(parent).is_err()
    {
        return Ok(false);
    }
    if std::fs::write(&installer_path, &installer).is_err() {
        return Ok(false);
    }

    let mut command = Command::new(&installer_path);
    command.arg("--update-background");
    if start_minimized {
        command.arg("--launch-minimized");
    }
    if command
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
        .is_err()
    {
        return Ok(false);
    }

    tracing::info!(from = current_tag, to = latest_tag, "Handing off VodLink update");
    if let Err(error) = controller.request_shutdown().await {
        tracing::warn!(%error, "VodLink updater shutdown cleanup failed");
    }
    Ok(true)
}

fn release_installer(release: &Value) -> Option<(&str, &str)> {
    let asset = release
        .get("assets")
        .and_then(Value::as_array)?
        .iter()
        .find(|asset| asset.get("name").and_then(Value::as_str) == Some(INSTALLER_ASSET))?;
    let download_url = asset
        .get("browser_download_url")
        .and_then(Value::as_str)
        .unwrap_or_default();
    let digest = asset
        .get("digest")
        .and_then(Value::as_str)
        .unwrap_or_default();
    if download_url.is_empty() || !digest.starts_with("sha256:") {
        return None;
    }
    Some((download_url, digest))
}

fn installer_digest_matches(installer: &[u8], expected_digest: &str) -> bool {
    let actual_digest = format!("sha256:{:x}", Sha256::digest(installer));
    actual_digest.eq_ignore_ascii_case(expected_digest)
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
    use serde_json::json;

    #[test]
    fn updater_tag_is_sanitized_like_cpp() {
        let path = updater_path("v1/2:3").expect("path");
        assert!(path.to_string_lossy().contains("v1_2_3"));
    }

    #[test]
    fn updater_selects_only_the_cpp_windows_asset_contract() {
        let release = json!({
            "assets": [
                {
                    "name": "VodLink-Windows-arm64.exe",
                    "browser_download_url": "https://example.invalid/arm64.exe",
                    "digest": "sha256:deadbeef"
                },
                {
                    "name": "VodLink-Windows-x64.exe",
                    "browser_download_url": "https://example.invalid/x64.exe",
                    "digest": "sha256:cafebabe"
                }
            ]
        });
        assert_eq!(
            release_installer(&release),
            Some(("https://example.invalid/x64.exe", "sha256:cafebabe"))
        );
    }

    #[test]
    fn updater_rejects_missing_cpp_digest_prefix() {
        let release = json!({
            "assets": [{
                "name": "VodLink-Windows-x64.exe",
                "browser_download_url": "https://example.invalid/x64.exe",
                "digest": "deadbeef"
            }]
        });
        assert_eq!(release_installer(&release), None);
    }

    #[test]
    fn updater_digest_comparison_matches_cpp_case_insensitivity() {
        let expected = format!("sha256:{:X}", Sha256::digest(b"VodLink updater fixture"));
        assert!(installer_digest_matches(b"VodLink updater fixture", &expected));
        assert!(!installer_digest_matches(b"different bytes", &expected));
    }
}
