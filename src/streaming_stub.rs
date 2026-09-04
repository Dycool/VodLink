use crate::models::{AudioCaptureSource, CaptureMode, RecorderSettings};
use anyhow::{Result, bail};

#[derive(Clone, Default)]
pub(crate) struct StreamerHandle;

impl StreamerHandle {
    pub(crate) fn spawn() -> Result<Self> {
        Ok(Self)
    }

    pub(crate) fn start(
        &self,
        _server: String,
        _stream_key: String,
        _capture_mode: CaptureMode,
        _audio_source: AudioCaptureSource,
        _process_hints: Vec<String>,
        _microphone: bool,
        _settings: RecorderSettings,
    ) -> Result<()> {
        bail!("OBS streaming support is disabled in this build")
    }

    pub(crate) fn stop(&self) -> Result<()> {
        Ok(())
    }
}
