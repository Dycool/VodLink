use crate::models::{AudioCaptureSource, CaptureMode, RecorderSettings};
use anyhow::{Result, bail};

pub(crate) struct StreamRequest {
    server: String,
    stream_key: String,
    capture_mode: CaptureMode,
    audio_source: AudioCaptureSource,
    process_hints: Vec<String>,
    microphone: bool,
    settings: RecorderSettings,
}

impl StreamRequest {
    pub(crate) fn new(
        server: String,
        stream_key: String,
        capture_mode: CaptureMode,
        audio_source: AudioCaptureSource,
        process_hints: Vec<String>,
        microphone: bool,
        settings: RecorderSettings,
    ) -> Self {
        Self {
            server,
            stream_key,
            capture_mode,
            audio_source,
            process_hints,
            microphone,
            settings,
        }
    }
}

#[derive(Clone, Default)]
pub(crate) struct StreamerHandle;

impl StreamerHandle {
    pub(crate) fn spawn() -> Result<Self> {
        Ok(Self)
    }

    pub(crate) fn start(&self, request: StreamRequest) -> Result<()> {
        let StreamRequest {
            server,
            stream_key,
            capture_mode,
            audio_source,
            process_hints,
            microphone,
            settings,
        } = request;
        drop((
            server,
            stream_key,
            capture_mode,
            audio_source,
            process_hints,
            microphone,
            settings,
        ));
        bail!("OBS streaming support is disabled in this build")
    }

    pub(crate) fn stop(&self) -> Result<()> {
        Ok(())
    }
}
