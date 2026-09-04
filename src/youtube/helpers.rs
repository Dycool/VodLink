fn require_string(value: &Value, field: &str, message: &str) -> Result<String> {
    value
        .get(field)
        .and_then(Value::as_str)
        .filter(|text| !text.is_empty())
        .map(ToOwned::to_owned)
        .with_context(|| message.to_owned())
}

fn api_error_message(value: &Value) -> String {
    value
        .pointer("/error/message")
        .and_then(Value::as_str)
        .or_else(|| value.get("message").and_then(Value::as_str))
        .unwrap_or("Unknown YouTube API error")
        .to_owned()
}

fn is_quota_error(value: &Value) -> bool {
    value
        .pointer("/error/errors")
        .and_then(Value::as_array)
        .is_some_and(|errors| {
            errors.iter().any(|error| {
                matches!(
                    error.get("reason").and_then(Value::as_str),
                    Some("quotaExceeded" | "rateLimitExceeded" | "userRateLimitExceeded")
                )
            })
        })
}

pub(crate) fn youtube_quality_tier(width: u32, height: u32) -> &'static str {
    let pixels = u64::from(width) * u64::from(height);
    if pixels <= 640 * 360 {
        "360p"
    } else if pixels <= 854 * 480 {
        "480p"
    } else if pixels <= 1280 * 720 {
        "720p"
    } else if pixels <= 1920 * 1080 {
        "1080p"
    } else if pixels <= 2560 * 1440 {
        "1440p"
    } else {
        "2160p"
    }
}

pub(crate) fn default_h264_bitrate(width: u32, height: u32, fps: u32) -> u32 {
    let tier = youtube_quality_tier(width, height);
    let high = fps > 30;
    match tier {
        "2160p" => {
            if high { 35_000 } else { 30_000 }
        }
        "1440p" => {
            if high { 24_000 } else { 15_000 }
        }
        "1080p" => {
            if high { 12_000 } else { 10_000 }
        }
        "720p" => {
            if high { 6_000 } else { 4_000 }
        }
        _ => 4_000,
    }
}

fn parse_library_item(item: &Value, account_email: &str) -> Option<Vod> {
    let youtube_id = item.get("id")?.as_str()?.to_owned();
    let snippet = item.get("snippet")?;
    let description = snippet
        .get("description")
        .and_then(Value::as_str)
        .unwrap_or_default();
    let metadata = parse_metadata(description);
    if metadata.is_none() && !description.contains(DEFAULT_DESCRIPTION) {
        return None;
    }
    let title = snippet
        .get("title")
        .and_then(Value::as_str)
        .unwrap_or_default()
        .to_owned();
    let game = metadata
        .as_ref()
        .and_then(|value| value.get("game"))
        .and_then(Value::as_str)
        .map(ToOwned::to_owned)
        .or_else(|| title.split(" — ").next().map(str::to_owned))
        .unwrap_or_else(|| "Unknown game".to_owned());
    let started_at = metadata
        .as_ref()
        .and_then(|value| value.get("startedAt"))
        .and_then(Value::as_str)
        .and_then(|raw| DateTime::parse_from_rfc3339(raw).ok())
        .map(|time| time.with_timezone(&Utc))
        .or_else(|| {
            snippet
                .get("publishedAt")
                .and_then(Value::as_str)
                .and_then(|raw| DateTime::parse_from_rfc3339(raw).ok())
                .map(|time| time.with_timezone(&Utc))
        })
        .unwrap_or_else(Utc::now);
    let duration_ms = metadata
        .as_ref()
        .and_then(|value| value.get("durationMs"))
        .and_then(Value::as_i64)
        .unwrap_or_else(|| {
            item.pointer("/contentDetails/duration")
                .and_then(Value::as_str)
                .map(parse_iso8601_duration_ms)
                .unwrap_or(0)
        });
    let upload_status = item
        .pointer("/status/uploadStatus")
        .and_then(Value::as_str)
        .unwrap_or_default();
    let processing = item
        .pointer("/processingDetails/processingStatus")
        .and_then(Value::as_str)
        .unwrap_or_default();
    let stream_status = if matches!(upload_status, "failed" | "rejected" | "deleted")
        || processing == "failed"
    {
        "failed"
    } else if processing == "processing" && duration_ms == 0 {
        "processing"
    } else {
        "processed"
    };
    Some(Vod {
        id: 0,
        game,
        youtube_id,
        stream_status: stream_status.to_owned(),
        started_at,
        duration_ms,
        account_email: account_email.trim().to_lowercase(),
        owner_email: String::new(),
        owner_name: String::new(),
        owner_picture_url: String::new(),
        title,
    })
}

fn parse_library_clips(item: &Value, youtube_id: &str) -> Vec<VodClip> {
    let description = item
        .pointer("/snippet/description")
        .and_then(Value::as_str)
        .unwrap_or_default();
    let Some(metadata) = parse_metadata(description) else {
        return Vec::new();
    };
    let Some(clips) = metadata.get("clips").and_then(Value::as_array) else {
        return Vec::new();
    };
    clips
        .iter()
        .filter_map(|value| {
            let clip_id = value
                .get("clipId")
                .and_then(Value::as_str)
                .unwrap_or_default()
                .to_owned();
            let clip_url = value
                .get("clipUrl")
                .and_then(Value::as_str)
                .map(ToOwned::to_owned)
                .unwrap_or_else(|| {
                    if clip_id.is_empty() {
                        String::new()
                    } else {
                        format!("https://www.youtube.com/clip/{clip_id}")
                    }
                });
            if clip_id.is_empty() && clip_url.is_empty() {
                return None;
            }
            let title = value
                .get("title")
                .and_then(Value::as_str)
                .unwrap_or_default()
                .to_owned();
            let start_seconds = value
                .get("startSeconds")
                .and_then(Value::as_i64)
                .unwrap_or(0) as i32;
            let end_seconds = value
                .get("endSeconds")
                .and_then(Value::as_i64)
                .unwrap_or(i64::from(start_seconds + 1)) as i32;
            let created_at = value
                .get("createdAt")
                .and_then(Value::as_str)
                .and_then(|raw| DateTime::parse_from_rfc3339(raw).ok())
                .map(|time| time.with_timezone(&Utc))
                .unwrap_or_else(Utc::now);
            Some(
                VodClip {
                    id: 0,
                    youtube_id: youtube_id.to_owned(),
                    clip_id,
                    clip_url,
                    title,
                    start_seconds,
                    end_seconds,
                    created_at,
                }
                .normalize(),
            )
        })
        .collect()
}

fn replace_metadata_block(existing: &str, metadata: &Value) -> Result<String> {
    let serialized = serde_json::to_string(metadata)?;
    let block = format!("{MARKER_START}\n{serialized}\n{MARKER_END}");
    if let (Some(start), Some(end)) = (existing.find(MARKER_START), existing.find(MARKER_END)) {
        let end = end + MARKER_END.len();
        let before = existing[..start].trim_end();
        let after = existing[end..].trim_start();
        let mut pieces = Vec::new();
        if !before.is_empty() {
            pieces.push(before.to_owned());
        }
        pieces.push(block);
        if !after.is_empty() {
            pieces.push(after.to_owned());
        }
        Ok(pieces.join("\n\n"))
    } else {
        let base = if existing.trim().is_empty() {
            DEFAULT_DESCRIPTION
        } else {
            existing.trim()
        };
        Ok(format!("{base}\n\n{block}"))
    }
}

fn parse_metadata(description: &str) -> Option<Value> {
    let start = description.find(MARKER_START)? + MARKER_START.len();
    let end = description[start..].find(MARKER_END)? + start;
    serde_json::from_str(description[start..end].trim()).ok()
}

fn parse_iso8601_duration_ms(raw: &str) -> i64 {
    let regex = Regex::new(
        r"^P(?:(\d+)D)?(?:T(?:(\d+)H)?(?:(\d+)M)?(?:(\d+(?:\.\d+)?)S)?)?$",
    )
    .expect("static duration regex");
    let Some(capture) = regex.captures(raw) else {
        return 0;
    };
    let days = capture
        .get(1)
        .and_then(|value| value.as_str().parse::<f64>().ok())
        .unwrap_or(0.0);
    let hours = capture
        .get(2)
        .and_then(|value| value.as_str().parse::<f64>().ok())
        .unwrap_or(0.0);
    let minutes = capture
        .get(3)
        .and_then(|value| value.as_str().parse::<f64>().ok())
        .unwrap_or(0.0);
    let seconds = capture
        .get(4)
        .and_then(|value| value.as_str().parse::<f64>().ok())
        .unwrap_or(0.0);
    ((days * 86_400.0 + hours * 3600.0 + minutes * 60.0 + seconds) * 1000.0).round()
        as i64
}

fn capture_first(text: &str, patterns: &[&str]) -> Option<String> {
    patterns.iter().find_map(|pattern| {
        Regex::new(pattern)
            .ok()?
            .captures(text)?
            .get(1)
            .map(|value| value.as_str().to_owned())
    })
}

fn html_unescape(value: &str) -> String {
    let entities: HashMap<&str, &str> = [
        ("&amp;", "&"),
        ("&quot;", "\""),
        ("&#39;", "'"),
        ("&lt;", "<"),
        ("&gt;", ">"),
    ]
    .into_iter()
    .collect();
    entities
        .into_iter()
        .fold(value.to_owned(), |text, (from, to)| text.replace(from, to))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rounds_up_quality_tiers() {
        assert_eq!(youtube_quality_tier(1920, 1080), "1080p");
        assert_eq!(youtube_quality_tier(2560, 1080), "1440p");
        assert_eq!(youtube_quality_tier(3840, 2160), "2160p");
    }

    #[test]
    fn duration_parser_handles_days_and_hours() {
        assert_eq!(parse_iso8601_duration_ms("PT1H2M3.5S"), 3_723_500);
        assert_eq!(parse_iso8601_duration_ms("P1DT2H3M4S"), 93_784_000);
        assert_eq!(parse_iso8601_duration_ms("P0D"), 0);
    }

    #[test]
    fn metadata_round_trip() {
        let metadata = json!({"game":"Portal 2","durationMs":123});
        let description =
            replace_metadata_block(DEFAULT_DESCRIPTION, &metadata).expect("metadata");
        assert_eq!(
            parse_metadata(&description).and_then(|value| value.get("game").cloned()),
            Some(json!("Portal 2"))
        );
    }
}
