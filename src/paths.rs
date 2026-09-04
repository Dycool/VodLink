use anyhow::{Context, Result};
use directories::BaseDirs;
use std::path::{Path, PathBuf};

#[derive(Clone, Debug)]
pub(crate) struct AppPaths {
    root: PathBuf,
}

impl AppPaths {
    pub(crate) fn discover() -> Result<Self> {
        let base = BaseDirs::new().context("Could not determine the user profile directory")?;
        #[cfg(target_os = "windows")]
        let root = std::env::var_os("LOCALAPPDATA")
            .map(PathBuf::from)
            .unwrap_or_else(|| base.data_local_dir().to_path_buf())
            .join("VodLink");
        #[cfg(target_os = "macos")]
        let root = base.home_dir().join("Library").join("Application Support").join("VodLink");
        #[cfg(all(not(target_os = "windows"), not(target_os = "macos")))]
        let root = base.home_dir().join(".local").join("share").join("VodLink");

        std::fs::create_dir_all(&root).with_context(|| format!("Could not create {}", root.display()))?;
        Ok(Self { root })
    }

    pub(crate) fn root(&self) -> &Path {
        &self.root
    }

    pub(crate) fn database(&self) -> PathBuf {
        self.root.join("vodlink.db")
    }
}
