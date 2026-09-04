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
        _ => bail!("Unknown privacy mode: {value}"),
    }
}

fn normalized_encoder(value: &str) -> Result<String> {
    let lower = value.trim().to_lowercase();
    if lower.contains("av1") {
        Ok("AV1".to_owned())
    } else if lower.contains("hevc") || lower.contains("265") {
        Ok("HEVC".to_owned())
    } else if lower.contains("264") || lower == "h.264" || lower == "h264" {
        Ok("H.264".to_owned())
    } else {
        bail!("Recorder encoder must be H.264, HEVC, or AV1")
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
    if width < 640
        || height < 360
        || width > 7680
        || height > 4320
        || width % 2 != 0
        || height % 2 != 0
    {
        bail!("Resolution must be an even size between 640x360 and 7680x4320");
    }
    Ok((width, height))
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
    fn invalid_resolution_fails_closed() {
        assert!(parse_resolution("1921x1080").is_err());
        assert!(parse_resolution("320x200").is_err());
        assert_eq!(
            parse_resolution("3440x1440").expect("ultrawide"),
            (3440, 1440)
        );
    }
}
