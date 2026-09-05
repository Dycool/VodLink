use anyhow::{Context, Result, bail};
use directories::BaseDirs;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

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

    pub(crate) fn schedule_reset_after_exit(&self) -> Result<()> {
        let targets = self.reset_targets()?;
        if targets.is_empty() {
            bail!("Could not resolve VodLink local data paths.");
        }
        let pid = std::process::id();

        #[cfg(target_os = "windows")]
        {
            use std::os::windows::process::CommandExt;
            const CREATE_NO_WINDOW: u32 = 0x0800_0000;

            let quoted = targets
                .iter()
                .map(|path| powershell_single_quote(&path.to_string_lossy().replace('/', "\\")))
                .collect::<Vec<_>>()
                .join(",");
            let script = format!(
                "$ErrorActionPreference='SilentlyContinue'; $pidToWait={pid}; \
while (Get-Process -Id $pidToWait -ErrorAction SilentlyContinue) {{ Start-Sleep -Milliseconds 250 }}; \
Start-Sleep -Milliseconds 500; $targets=@({quoted}); \
foreach ($p in $targets) {{ if (Test-Path -LiteralPath $p) {{ Remove-Item -LiteralPath $p -Recurse -Force -ErrorAction SilentlyContinue }} }}"
            );
            Command::new("powershell.exe")
                .args([
                    "-NoProfile",
                    "-ExecutionPolicy",
                    "Bypass",
                    "-WindowStyle",
                    "Hidden",
                    "-Command",
                    &script,
                ])
                .stdin(Stdio::null())
                .stdout(Stdio::null())
                .stderr(Stdio::null())
                .creation_flags(CREATE_NO_WINDOW)
                .spawn()
                .with_context(|| {
                    format!(
                        "Could not start the VodLink reset helper. Close VodLink and delete {} manually.",
                        self.root.display()
                    )
                })?;
            Ok(())
        }

        #[cfg(not(target_os = "windows"))]
        {
            let mut command = Command::new("/bin/sh");
            command.args([
                "-c",
                "pid=\"$1\"; shift; while kill -0 \"$pid\" 2>/dev/null; do sleep 0.25; done; sleep 0.5; rm -rf -- \"$@\"",
                "vodlink-reset",
                &pid.to_string(),
            ]);
            command.args(targets.iter());
            command
                .stdin(Stdio::null())
                .stdout(Stdio::null())
                .stderr(Stdio::null())
                .spawn()
                .with_context(|| {
                    format!(
                        "Could not start the VodLink reset helper. Close VodLink and delete {} manually.",
                        self.root.display()
                    )
                })?;
            Ok(())
        }
    }

    fn reset_targets(&self) -> Result<Vec<PathBuf>> {
        let base = BaseDirs::new().context("Could not determine the user profile directory")?;
        let mut targets = Vec::new();

        #[cfg(target_os = "windows")]
        for name in [
            "cache",
            "obs-runtime",
            "obs-private",
            "vodlink.db",
            "vodlink.db-wal",
            "vodlink.db-shm",
        ] {
            add_vodlink_target(&mut targets, self.root.join(name));
        }

        #[cfg(not(target_os = "windows"))]
        {
            add_vodlink_target(&mut targets, self.root.clone());
            add_vodlink_target(&mut targets, self.root.join("cache"));
        }

        #[cfg(target_os = "windows")]
        {
            let generic_data = std::env::var_os("LOCALAPPDATA")
                .map(PathBuf::from)
                .unwrap_or_else(|| base.data_local_dir().to_path_buf());
            add_vodlink_target(&mut targets, generic_data.join("VodLink").join("VodLink"));
            add_vodlink_target(&mut targets, generic_data.join("VodLink").join("VodLink").join("cache"));
        }
        #[cfg(target_os = "macos")]
        {
            add_vodlink_target(
                &mut targets,
                base.home_dir()
                    .join("Library")
                    .join("Application Support")
                    .join("VodLink")
                    .join("VodLink"),
            );
            add_vodlink_target(
                &mut targets,
                base.home_dir().join("Library").join("Caches").join("VodLink").join("VodLink"),
            );
        }
        #[cfg(all(not(target_os = "windows"), not(target_os = "macos")))]
        {
            add_vodlink_target(
                &mut targets,
                base.home_dir().join(".local").join("share").join("VodLink").join("VodLink"),
            );
            add_vodlink_target(
                &mut targets,
                base.home_dir().join(".cache").join("VodLink").join("VodLink"),
            );
        }

        targets.sort_by_key(|path| std::cmp::Reverse(path.as_os_str().len()));
        Ok(targets)
    }
}

fn add_vodlink_target(targets: &mut Vec<PathBuf>, path: PathBuf) {
    let owned = path
        .components()
        .any(|component| component.as_os_str().to_string_lossy() == "VodLink");
    if owned && !targets.contains(&path) {
        targets.push(path);
    }
}

#[cfg(target_os = "windows")]
fn powershell_single_quote(value: &str) -> String {
    format!("'{}'", value.replace('\'', "''"))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    #[cfg_attr(miri, ignore = "directories uses unsupported host account lookup under Miri")]
    fn reset_targets_are_scoped_to_vodlink() {
        let paths = AppPaths::discover().expect("paths");
        for target in paths.reset_targets().expect("reset targets") {
            assert!(target.components().any(|component| component.as_os_str() == "VodLink"));
        }
    }

    #[cfg(target_os = "windows")]
    #[test]
    fn powershell_quote_doubles_single_quotes() {
        assert_eq!(powershell_single_quote("C:\\a'b"), "'C:\\a''b'");
    }
}
