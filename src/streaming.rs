use crate::models::{AudioCaptureSource, CaptureMode, RecorderSettings};
use anyhow::{Context, Result, bail};
use libobs_wrapper::capabilities::{OutputCapabilities, OutputCompatibilityRequest};
use libobs_wrapper::context::ObsContext;
use libobs_wrapper::data::output::{ObsOutputRef, ObsOutputTrait};
use libobs_wrapper::data::video::ObsVideoInfoBuilder;
use libobs_wrapper::data::ObsDataSetters;
use libobs_wrapper::scenes::SceneItemTrait;
use libobs_wrapper::utils::StartupInfo;
use std::path::Path;
use std::sync::mpsc;

#[derive(Clone)]
pub(crate) struct StreamerHandle {
    sender: mpsc::Sender<StreamerCommand>,
}

enum StreamerCommand {
    Start {
        server: String,
        stream_key: String,
        capture_mode: CaptureMode,
        audio_source: AudioCaptureSource,
        process_hints: Vec<String>,
        microphone: bool,
        settings: RecorderSettings,
        reply: mpsc::Sender<Result<()>>,
    },
    Stop { reply: mpsc::Sender<Result<()>> },
}

impl StreamerHandle {
    pub(crate) fn spawn() -> Result<Self> {
        let (sender, receiver) = mpsc::channel::<StreamerCommand>();
        std::thread::Builder::new()
            .name("vodlink-obs".to_owned())
            .spawn(move || {
                let mut streamer = RtmpStreamer::default();
                while let Ok(command) = receiver.recv() {
                    match command {
                        StreamerCommand::Start { server, stream_key, capture_mode, audio_source, process_hints, microphone, settings, reply } => {
                            let result = streamer.start(&server, &stream_key, capture_mode, audio_source, &process_hints, microphone, &settings);
                            let _ = reply.send(result);
                        }
                        StreamerCommand::Stop { reply } => {
                            let _ = reply.send(streamer.stop());
                        }
                    }
                }
            })
            .context("Could not start the OBS worker thread")?;
        Ok(Self { sender })
    }

    pub(crate) fn start(
        &self,
        server: String,
        stream_key: String,
        capture_mode: CaptureMode,
        audio_source: AudioCaptureSource,
        process_hints: Vec<String>,
        microphone: bool,
        settings: RecorderSettings,
    ) -> Result<()> {
        let (reply, receive) = mpsc::channel();
        self.sender.send(StreamerCommand::Start { server, stream_key, capture_mode, audio_source, process_hints, microphone, settings, reply })
            .context("OBS worker thread is not available")?;
        receive.recv().context("OBS worker stopped before replying")?
    }

    pub(crate) fn stop(&self) -> Result<()> {
        let (reply, receive) = mpsc::channel();
        self.sender.send(StreamerCommand::Stop { reply })
            .context("OBS worker thread is not available")?;
        receive.recv().context("OBS worker stopped before replying")?
    }
}

#[derive(Default)]
pub(crate) struct RtmpStreamer {
    context: Option<ObsContext>,
    output: Option<ObsOutputRef>,
}

impl Drop for RtmpStreamer {
    fn drop(&mut self) {
        let _ = self.stop();
    }
}

impl RtmpStreamer {
    pub(crate) fn is_streaming(&self) -> bool {
        self.output.is_some()
    }

    pub(crate) fn start(
        &mut self,
        server: &str,
        stream_key: &str,
        capture_mode: CaptureMode,
        audio_source: AudioCaptureSource,
        process_hints: &[String],
        microphone: bool,
        settings: &RecorderSettings,
    ) -> Result<()> {
        if self.is_streaming() {
            bail!("VodLink is already streaming");
        }
        if !matches!(url::Url::parse(server), Ok(url) if matches!(url.scheme(), "rtmp" | "rtmps")) {
            bail!("YouTube returned an invalid RTMP ingest address");
        }
        if stream_key.trim().is_empty() {
            bail!("YouTube returned an empty RTMP stream key");
        }

        let mut context = create_context(settings)?;
        let mut scene = context.scene("VodLink", Some(0)).context("Could not create the OBS scene")?;
        add_video_source(&mut context, &mut scene, capture_mode, audio_source, process_hints)?;
        add_audio_sources(&mut context, &mut scene, audio_source, microphone, process_hints)?;

        let output = create_rtmp_output(&context, server, stream_key, settings)?;
        output.start().context("OBS could not start the YouTube RTMP output")?;
        self.context = Some(context);
        self.output = Some(output);
        Ok(())
    }

    pub(crate) fn stop(&mut self) -> Result<()> {
        if let Some(output) = self.output.take() {
            output.stop().context("OBS could not stop the RTMP output cleanly")?;
        }
        self.context.take();
        Ok(())
    }
}

fn create_context(settings: &RecorderSettings) -> Result<ObsContext> {
    let video = ObsVideoInfoBuilder::new()
        .base_width(settings.width)
        .base_height(settings.height)
        .output_width(settings.width)
        .output_height(settings.height)
        .fps_num(settings.fps)
        .fps_den(1)
        .build();
    StartupInfo::new()
        .set_video_info(video)
        .start()
        .context("Could not initialize VodLink's private OBS runtime")
}

fn create_rtmp_output(
    context: &ObsContext,
    server: &str,
    stream_key: &str,
    settings: &RecorderSettings,
) -> Result<ObsOutputRef> {
    let codec = normalized_codec(&settings.encoder);
    let capabilities = context.capabilities().context("Could not inspect OBS capabilities")?;
    let request = OutputCompatibilityRequest::new()
        .protocol("RTMP")
        .video_codec(codec)
        .audio_codec("aac")
        .prefer_hardware_video(true)
        .require_output_capabilities(
            OutputCapabilities::ENCODED
                | OutputCapabilities::VIDEO
                | OutputCapabilities::AUDIO
                | OutputCapabilities::SERVICE,
        );
    let plan = capabilities
        .best_output_plan(&request)
        .map_err(|report| anyhow::anyhow!("No compatible hardware RTMP graph: {}", report.summary()))?;
    let video_type = plan.video_encoder().context("No compatible video encoder was found")?;
    let audio_type = plan.audio_encoder().context("No compatible AAC encoder was found")?;

    let mut video_settings = video_type.default_settings_mut()?;
    video_settings
        .set_string("rate_control", "CBR")?
        .set_string("rc", "cbr")?
        .set_int("bitrate", i64::from(settings.bitrate_kbps))?
        .set_int("keyint_sec", 2)?
        .set_int("gop_size", i64::from(settings.fps.saturating_mul(2)))?
        .set_int("bf", 2)?
        .set_int("bframes", 2)?
        .set_bool("repeat_headers", true)?;
    let video_encoder = context.create_video_encoder(
        video_type,
        "VodLink Video",
        Some(video_settings),
    )?;

    let mut audio_settings = audio_type.default_settings_mut()?;
    audio_settings.set_int("bitrate", 128)?;
    let audio_encoder = context.create_audio_encoder(
        audio_type,
        "VodLink Audio",
        Some(audio_settings),
        0,
    )?;

    let service_type = context
        .service_type("rtmp_custom")?
        .context("OBS rtmp-services plugin is not available")?;
    let mut service_settings = service_type.default_settings_mut()?;
    service_settings
        .set_string("server", server)?
        .set_string("key", stream_key)?;
    let service = context.create_service(
        &service_type,
        "VodLink YouTube",
        Some(service_settings),
    )?;

    let output_settings = plan.output().default_settings_mut()?;
    Ok(context
        .output_pipeline(plan.output(), "VodLink RTMP", Some(output_settings))
        .video_encoder(video_encoder)
        .audio_encoder(0, audio_encoder)
        .service(service)
        .build()?
        .into_output())
}

fn normalized_codec(preference: &str) -> &'static str {
    let lower = preference.to_ascii_lowercase();
    if lower.contains("av1") {
        "av1"
    } else if lower.contains("hevc") || lower.contains("265") {
        "hevc"
    } else {
        "h264"
    }
}

fn add_video_source(
    context: &mut ObsContext,
    scene: &mut libobs_wrapper::scenes::ObsSceneRef,
    capture_mode: CaptureMode,
    audio_source: AudioCaptureSource,
    process_hints: &[String],
) -> Result<()> {
    #[cfg(target_os = "windows")]
    {
        use libobs_simple::sources::windows::{WindowCaptureSourceBuilder, WindowSearchMode};

        if capture_mode == CaptureMode::GameWindow {
            let windows = WindowCaptureSourceBuilder::get_windows(WindowSearchMode::ExcludeMinimized)
                .context("Could not enumerate capturable game windows")?;
            let window = windows
                .iter()
                .find(|candidate| window_matches(&candidate.full_exe, process_hints))
                .context("The detected game is running but its capturable window is not ready yet")?;
            let source_type = context
                .source_type("window_capture")?
                .context("OBS window_capture is unavailable")?;
            let mut source_settings = source_type.default_settings_mut()?;
            source_settings
                .set_string("window", window.obs_id.as_str())?
                .set_int("priority", 2)?
                .set_int("method", 2)?
                .set_bool("cursor", true)?
                .set_bool("capture_cursor", true)?
                .set_bool("capture_audio", audio_source == AudioCaptureSource::GameOnly)?;
            let item = scene.add_discovered_source(
                &source_type,
                "VodLink Game Window",
                Some(source_settings),
            )?;
            item.fit_source_to_screen()?;
            return Ok(());
        }
    }

    let candidates: &[&str] = if cfg!(target_os = "windows") {
        &["monitor_capture"]
    } else if cfg!(target_os = "macos") {
        &["screen_capture", "display_capture"]
    } else {
        &["pipewire-desktop-capture-source", "xshm_input"]
    };
    add_first_available_source(context, scene, candidates, "VodLink Desktop", true)
        .context("No supported desktop capture source is available")
}

fn add_audio_sources(
    context: &mut ObsContext,
    scene: &mut libobs_wrapper::scenes::ObsSceneRef,
    source: AudioCaptureSource,
    microphone: bool,
    process_hints: &[String],
) -> Result<()> {
    if source == AudioCaptureSource::System {
        let candidates: &[&str] = if cfg!(target_os = "windows") {
            &["wasapi_output_capture"]
        } else if cfg!(target_os = "macos") {
            &["screen_capture_audio_capture"]
        } else {
            &["pulse_output_capture"]
        };
        add_first_available_source(context, scene, candidates, "VodLink System Audio", false)
            .context("System audio capture is unavailable")?;
    } else if cfg!(target_os = "windows") {
        let selector = obs_process_selector(process_hints);
        if selector.is_empty() {
            bail!("Game-only audio needs a concrete executable name");
        }
        let source_type = context
            .source_type("wasapi_process_output_capture")?
            .context("OBS application-audio capture is unavailable")?;
        let mut settings = source_type.default_settings_mut()?;
        settings
            .set_string("window", selector)?
            .set_int("priority", 2)?;
        scene.add_discovered_source(&source_type, "VodLink Game Audio", Some(settings))?;
    }

    if microphone {
        let candidates: &[&str] = if cfg!(target_os = "windows") {
            &["wasapi_input_capture"]
        } else if cfg!(target_os = "macos") {
            &["coreaudio_input_capture", "coreaudio_input_capture_v2"]
        } else {
            &["pulse_input_capture"]
        };
        add_first_available_source(context, scene, candidates, "VodLink Microphone", false)
            .context("Microphone capture is unavailable")?;
    }
    Ok(())
}

fn add_first_available_source(
    context: &mut ObsContext,
    scene: &mut libobs_wrapper::scenes::ObsSceneRef,
    candidates: &[&str],
    name: &str,
    fit: bool,
) -> Result<()> {
    for source_id in candidates {
        if let Some(source_type) = context.source_type(source_id)? {
            let item = scene.add_discovered_source(&source_type, name, None)?;
            if fit {
                item.fit_source_to_screen()?;
            }
            return Ok(());
        }
    }
    bail!("None of the required OBS source plugins are available: {}", candidates.join(", "))
}

#[cfg(target_os = "windows")]
fn window_matches(full_exe: &str, hints: &[String]) -> bool {
    let full = full_exe.replace('\\', "/").to_ascii_lowercase();
    let file = Path::new(full_exe)
        .file_name()
        .map(|name| name.to_string_lossy().to_ascii_lowercase())
        .unwrap_or_default();
    hints.iter().any(|hint| {
        let hint = hint.replace('\\', "/").to_ascii_lowercase();
        let hint_file = Path::new(&hint)
            .file_name()
            .map(|name| name.to_string_lossy().to_ascii_lowercase())
            .unwrap_or_default();
        full == hint || file == hint_file || file == ensure_exe(&hint_file)
    })
}

#[cfg(target_os = "windows")]
fn ensure_exe(value: &str) -> String {
    if value.ends_with(".exe") { value.to_owned() } else { format!("{value}.exe") }
}

fn obs_process_selector(hints: &[String]) -> String {
    let executable = hints.iter().find_map(|hint| {
        let file = Path::new(hint).file_name()?.to_string_lossy().to_ascii_lowercase();
        if file.is_empty() { None } else if file.ends_with(".exe") { Some(file) } else { Some(format!("{file}.exe")) }
    });
    executable
        .map(|value| format!("VodLink:VodLink:{}", value.replace('#', "#22").replace(':', "#3A")))
        .unwrap_or_default()
}

#[cfg(test)]
mod tests {
    use super::normalized_codec;

    #[test]
    fn codec_preferences_match_legacy_settings() {
        assert_eq!(normalized_codec("H.264"), "h264");
        assert_eq!(normalized_codec("HEVC / H.265"), "hevc");
        assert_eq!(normalized_codec("AV1"), "av1");
    }
}
