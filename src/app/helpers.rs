fn read_bool(repository: &VodRepository, key: &str, fallback: bool) -> Result<bool> {
    Ok(repository.setting(key)?.map_or(fallback, |value| {
        matches!(value.as_str(), "1" | "true" | "yes" | "on")
    }))
}

fn bool_text(value: bool) -> &'static str {
    if value { "1" } else { "0" }
}

fn normalized_privacy(value: &str) -> Result<String> {
    let normalized = value.trim().to_lowercase();
    match normalized.as_str() {
        "game_only" | "game_external_audio" | "desktop" | "full_desktop" => Ok(normalized),
        _ => Ok("game_external_audio".to_owned()),
    }
}

fn normalized_encoder(value: &str) -> Result<String> {
    let lower = value.trim().to_lowercase();
    if lower.contains("av1") {
        Ok("AV1".to_owned())
    } else if lower.contains("hevc") || lower.contains("h265") || lower.contains("265") {
        Ok("HEVC".to_owned())
    } else {
        Ok("H.264".to_owned())
    }
}

fn parse_resolution(value: &str) -> Result<(u32, u32)> {
    let normalized = value.trim().to_lowercase();
    let (width, height) = normalized
        .split_once('x')
        .context("Resolution must use WIDTHxHEIGHT format")?;
    let width = width
        .trim()
        .parse::<u32>()
        .context("Invalid recorder width")?;
    let height = height
        .trim()
        .parse::<u32>()
        .context("Invalid recorder height")?;
    if width < 640 || height < 360 || width % 2 != 0 || height % 2 != 0 {
        bail!("Resolution must be an even size of at least 640x360");
    }
    Ok((width, height))
}

fn native_recorder_resolution() -> (u32, u32) {
    #[cfg(feature = "desktop")]
    {
        if let Ok(displays) = display_info::DisplayInfo::all() {
            let display = displays
                .iter()
                .find(|display| display.is_primary)
                .or_else(|| displays.first());
            if let Some(display) = display {
                let width = display.width & !1;
                let height = display.height & !1;
                if width >= 640 && height >= 360 {
                    return (width, height);
                }
            }
        }
    }
    (1920, 1080)
}

fn resolution_options_from_sizes(
    native: (u32, u32),
    mut display_sizes: Vec<(u32, u32)>,
    saved: &str,
) -> Vec<String> {
    display_sizes.extend([
        (3840, 2160),
        (3440, 1440),
        (2560, 1440),
        (2560, 1080),
        (1920, 1080),
        (1600, 900),
        (1366, 768),
        (1280, 720),
    ]);
    display_sizes.sort_by(|a, b| b.0.cmp(&a.0).then_with(|| b.1.cmp(&a.1)));

    let native_text = format!("{}x{}", native.0, native.1);
    let mut result = vec![native_text];
    for (width, height) in display_sizes {
        let text = format!("{width}x{height}");
        if !result.contains(&text) {
            result.push(text);
        }
    }
    let saved = saved.trim();
    if !saved.is_empty() && !result.iter().any(|entry| entry == saved) {
        result.insert(0, saved.to_owned());
    }
    if result.is_empty() {
        result.push("1920x1080".to_owned());
    }
    result
}

fn available_resolutions(saved: &str) -> Vec<String> {
    let native = native_recorder_resolution();
    let mut display_sizes = Vec::<(u32, u32)>::new();

    #[cfg(feature = "desktop")]
    if let Ok(displays) = display_info::DisplayInfo::all() {
        for display in displays {
            // Match the C++ settings list exactly: connected monitor modes are
            // presented as reported. Only native_recorder_resolution() rounds
            // the primary recorder default to encoder-compatible even values.
            if display.width >= 640 && display.height >= 480 {
                display_sizes.push((display.width, display.height));
            }
        }
    }

    resolution_options_from_sizes(native, display_sizes, saved)
}

fn available_encoder_choices() -> Vec<String> {
    let mut choices = vec!["H.264".to_owned(), "HEVC".to_owned()];
    if probable_hardware_av1_encoder_available() {
        choices.push("AV1".to_owned());
    }
    choices
}

fn probable_hardware_av1_encoder_available() -> bool {
    if std::env::var("VODLINK_FORCE_AV1_SETTINGS").ok().as_deref() == Some("1") {
        return true;
    }

    #[cfg(target_os = "macos")]
    {
        return false;
    }

    #[cfg(any(target_os = "windows", target_os = "linux"))]
    {
        let gpu = gpu_description_for_settings_probe();
        if gpu.is_empty() {
            return false;
        }
        gpu.contains("rtx 40")
            || gpu.contains("rtx 50")
            || gpu.contains("geforce rtx 40")
            || gpu.contains("geforce rtx 50")
            || gpu.contains("intel(r) arc")
            || gpu.contains("intel arc")
            || gpu.contains("radeon rx 7")
            || gpu.contains("radeon 780m")
            || gpu.contains("radeon 760m")
    }

    #[cfg(not(any(target_os = "windows", target_os = "linux", target_os = "macos")))]
    false
}

#[cfg(target_os = "windows")]
fn gpu_description_for_settings_probe() -> String {
    std::process::Command::new("wmic")
        .args(["path", "win32_VideoController", "get", "name"])
        .output()
        .ok()
        .filter(|output| output.status.success())
        .map(|output| String::from_utf8_lossy(&output.stdout).to_lowercase())
        .unwrap_or_default()
}

#[cfg(target_os = "linux")]
fn gpu_description_for_settings_probe() -> String {
    std::process::Command::new("sh")
        .args(["-c", "lspci 2>/dev/null | grep -Ei 'vga|3d|display' || true"])
        .output()
        .ok()
        .filter(|output| output.status.success())
        .map(|output| String::from_utf8_lossy(&output.stdout).to_lowercase())
        .unwrap_or_default()
}

fn capture_policy(privacy: &str) -> Result<(CaptureMode, AudioCaptureSource)> {
    match privacy {
        "game_only" => Ok((CaptureMode::GameWindow, AudioCaptureSource::GameOnly)),
        "game_external_audio" => Ok((CaptureMode::GameWindow, AudioCaptureSource::System)),
        "desktop" | "full_desktop" => {
            Ok((CaptureMode::FullDesktop, AudioCaptureSource::System))
        }
        _ => bail!("Unknown privacy mode: {privacy}"),
    }
}

fn auth_expired(error: &anyhow::Error) -> bool {
    error.to_string().contains("AUTH_EXPIRED")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn privacy_modes_preserve_legacy_capture_semantics() {
        assert_eq!(
            capture_policy("game_only").expect("game only"),
            (CaptureMode::GameWindow, AudioCaptureSource::GameOnly)
        );
        assert_eq!(
            capture_policy("game_external_audio").expect("external"),
            (CaptureMode::GameWindow, AudioCaptureSource::System)
        );
        assert_eq!(
            capture_policy("desktop").expect("desktop"),
            (CaptureMode::FullDesktop, AudioCaptureSource::System)
        );
    }

    #[test]
    fn privacy_normalization_matches_cpp_fallback_semantics() {
        assert_eq!(
            normalized_privacy(" GAME_ONLY ").expect("game only"),
            "game_only"
        );
        assert_eq!(
            normalized_privacy("full_desktop").expect("legacy desktop"),
            "full_desktop"
        );
        assert_eq!(
            normalized_privacy("").expect("empty fallback"),
            "game_external_audio"
        );
        assert_eq!(
            normalized_privacy("unknown-value").expect("unknown fallback"),
            "game_external_audio"
        );
    }

    #[test]
    fn resolution_validation_matches_cpp_even_minimum() {
        assert!(parse_resolution("1921x1080").is_err());
        assert!(parse_resolution("320x200").is_err());
        assert_eq!(
            parse_resolution("3440x1440").expect("ultrawide"),
            (3440, 1440)
        );
    }

    #[test]
    fn resolution_list_preserves_saved_choice() {
        let choices = resolution_options_from_sizes((1920, 1080), Vec::new(), "2222x1112");
        assert_eq!(choices.first().map(String::as_str), Some("2222x1112"));
        assert!(choices.iter().any(|value| value == "1920x1080"));
    }

    #[test]
    fn resolution_list_preserves_connected_monitor_dimensions() {
        let choices = resolution_options_from_sizes(
            (1920, 1080),
            vec![(1365, 767), (3440, 1440), (1365, 767)],
            "",
        );
        assert!(choices.iter().any(|value| value == "1365x767"));
        assert_eq!(
            choices.iter().filter(|value| value.as_str() == "1365x767").count(),
            1
        );
        assert_eq!(choices.first().map(String::as_str), Some("1920x1080"));
    }
}
