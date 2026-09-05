use anyhow::{Context, Result, bail};
#[cfg(any(target_os = "macos", target_os = "linux"))]
use std::io::Write;
#[cfg(any(target_os = "macos", target_os = "linux"))]
use std::path::Path;
use std::path::PathBuf;

#[cfg(target_os = "windows")]
const WINDOWS_RUN_VALUE: &str = "VodLink";

pub(crate) fn supported() -> bool {
    cfg!(any(target_os = "windows", target_os = "macos", target_os = "linux"))
}

pub(crate) fn enabled() -> bool {
    enabled_inner().unwrap_or(false)
}

pub(crate) fn set_enabled(enabled: bool) -> Result<()> {
    if !supported() {
        bail!("Launch on startup is not supported on this platform.");
    }
    set_enabled_inner(enabled)
}

fn current_executable() -> Result<PathBuf> {
    std::env::current_exe().context("VodLink executable path is empty.")
}

#[cfg(any(target_os = "windows", test))]
fn windows_startup_command(executable: &str) -> String {
    format!("\"{}\" --minimized", executable.replace('/', "\\"))
}

#[cfg(target_os = "windows")]
fn enabled_inner() -> Result<bool> {
    use winreg::enums::{HKEY_CURRENT_USER, KEY_READ};
    use winreg::RegKey;

    let hkcu = RegKey::predef(HKEY_CURRENT_USER);
    let run = match hkcu.open_subkey_with_flags(
        "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        KEY_READ,
    ) {
        Ok(key) => key,
        Err(_) => return Ok(false),
    };
    let value: String = match run.get_value(WINDOWS_RUN_VALUE) {
        Ok(value) => value,
        Err(_) => return Ok(false),
    };
    let exe = current_executable()?.to_string_lossy().replace('/', "\\");
    let value_lower = value.to_lowercase();
    Ok(value_lower.contains(&exe.to_lowercase()) && value_lower.contains("--minimized"))
}

#[cfg(target_os = "windows")]
fn set_enabled_inner(enabled: bool) -> Result<()> {
    use winreg::enums::{HKEY_CURRENT_USER, KEY_SET_VALUE};
    use winreg::RegKey;

    let hkcu = RegKey::predef(HKEY_CURRENT_USER);
    let (run, _) = hkcu
        .create_subkey_with_flags(
            "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            KEY_SET_VALUE,
        )
        .map_err(|_| anyhow::anyhow!("Windows rejected the startup registry change."))?;
    if enabled {
        let command = windows_startup_command(&current_executable()?.to_string_lossy());
        run.set_value(WINDOWS_RUN_VALUE, &command)
            .map_err(|_| anyhow::anyhow!("Windows rejected the startup registry change."))?;
    } else {
        match run.delete_value(WINDOWS_RUN_VALUE) {
            Ok(()) => {}
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
            Err(_) => bail!("Windows rejected the startup registry change."),
        }
    }
    Ok(())
}

#[cfg(target_os = "macos")]
fn enabled_inner() -> Result<bool> {
    let path = mac_launch_agent_path()?;
    let content = match std::fs::read_to_string(path) {
        Ok(content) => content,
        Err(_) => return Ok(false),
    };
    let exe = current_executable()?.to_string_lossy().into_owned();
    Ok(content.contains(&exe) && content.contains("--minimized"))
}

#[cfg(target_os = "macos")]
fn set_enabled_inner(enabled: bool) -> Result<()> {
    let path = mac_launch_agent_path()?;
    if !enabled {
        let _ = std::fs::remove_file(&path);
        return Ok(());
    }
    let parent = path
        .parent()
        .context("Could not create the macOS LaunchAgents directory.")?;
    std::fs::create_dir_all(parent)
        .map_err(|_| anyhow::anyhow!("Could not create the macOS LaunchAgents directory."))?;
    write_startup_file(
        &path,
        &mac_launch_agent_content(&current_executable()?.to_string_lossy()),
        "Could not write the macOS startup item.",
    )
}

#[cfg(target_os = "linux")]
fn enabled_inner() -> Result<bool> {
    let path = linux_autostart_path()?;
    let content = match std::fs::read_to_string(path) {
        Ok(content) => content,
        Err(_) => return Ok(false),
    };
    let exe = current_executable()?.to_string_lossy().into_owned();
    Ok(content.contains(&exe)
        && content.contains("--minimized")
        && !content.contains("Hidden=true"))
}

#[cfg(target_os = "linux")]
fn set_enabled_inner(enabled: bool) -> Result<()> {
    let path = linux_autostart_path()?;
    if !enabled {
        let _ = std::fs::remove_file(&path);
        return Ok(());
    }
    let parent = path
        .parent()
        .context("Could not create the Linux autostart directory.")?;
    std::fs::create_dir_all(parent)
        .map_err(|_| anyhow::anyhow!("Could not create the Linux autostart directory."))?;
    write_startup_file(
        &path,
        &linux_autostart_content(&current_executable()?.to_string_lossy()),
        "Could not write the Linux autostart entry.",
    )
}

#[cfg(not(any(target_os = "windows", target_os = "macos", target_os = "linux")))]
fn enabled_inner() -> Result<bool> {
    Ok(false)
}

#[cfg(not(any(target_os = "windows", target_os = "macos", target_os = "linux")))]
fn set_enabled_inner(_enabled: bool) -> Result<()> {
    bail!("Launch on startup is not supported on this platform.")
}

#[cfg(target_os = "macos")]
fn mac_launch_agent_path() -> Result<PathBuf> {
    let base = directories::BaseDirs::new().context("Could not locate the user's home directory")?;
    Ok(base
        .home_dir()
        .join("Library")
        .join("LaunchAgents")
        .join("app.vodlink.VodLink.plist"))
}

#[cfg(target_os = "linux")]
fn linux_autostart_path() -> Result<PathBuf> {
    let base = directories::BaseDirs::new().context("Could not locate the user's config directory")?;
    Ok(base.config_dir().join("autostart").join("vodlink.desktop"))
}

#[cfg(any(target_os = "macos", target_os = "linux"))]
fn write_startup_file(path: &Path, content: &str, open_error: &str) -> Result<()> {
    let mut file = std::fs::File::create(path).map_err(|_| anyhow::anyhow!(open_error.to_owned()))?;
    // C++ checks that QFile opens, but deliberately does not inspect QTextStream's
    // eventual write status. Preserve that externally observable failure behavior.
    let _ = file.write_all(content.as_bytes());
    Ok(())
}

#[cfg(any(target_os = "macos", test))]
fn xml_escape(value: &str) -> String {
    value
        .replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
        .replace('"', "&quot;")
        .replace('\'', "&apos;")
}

#[cfg(any(target_os = "macos", test))]
fn mac_launch_agent_content(executable: &str) -> String {
    let exe = xml_escape(executable);
    format!(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n\
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n\
<plist version=\"1.0\">\n\
<dict>\n\
  <key>Label</key><string>app.vodlink.VodLink</string>\n\
  <key>ProgramArguments</key>\n\
  <array>\n\
    <string>{exe}</string>\n\
    <string>--minimized</string>\n\
  </array>\n\
  <key>RunAtLoad</key><true/>\n\
</dict>\n\
</plist>\n"
    )
}

#[cfg(any(target_os = "linux", test))]
fn desktop_quote(value: &str) -> String {
    format!("\"{}\"", value.replace('\\', "\\\\").replace('"', "\\\""))
}

#[cfg(any(target_os = "linux", test))]
fn linux_autostart_content(executable: &str) -> String {
    let exe = desktop_quote(executable);
    format!(
        "[Desktop Entry]\n\
Type=Application\n\
Name=VodLink\n\
Comment=Start VodLink minimized\n\
Exec={exe} --minimized\n\
Icon=vodlink\n\
Terminal=false\n\
X-GNOME-Autostart-enabled=true\n"
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn windows_command_matches_cpp_exactly() {
        assert_eq!(
            windows_startup_command("C:/Program Files/VodLink/VodLink.exe"),
            "\"C:\\Program Files\\VodLink\\VodLink.exe\" --minimized"
        );
    }

    #[test]
    fn mac_plist_matches_cpp_exactly() {
        assert_eq!(
            mac_launch_agent_content("/Applications/Vod&Link.app/VodLink"),
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n\
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n\
<plist version=\"1.0\">\n\
<dict>\n\
  <key>Label</key><string>app.vodlink.VodLink</string>\n\
  <key>ProgramArguments</key>\n\
  <array>\n\
    <string>/Applications/Vod&amp;Link.app/VodLink</string>\n\
    <string>--minimized</string>\n\
  </array>\n\
  <key>RunAtLoad</key><true/>\n\
</dict>\n\
</plist>\n"
        );
    }

    #[test]
    fn linux_desktop_entry_matches_cpp_exactly() {
        assert_eq!(
            linux_autostart_content("/opt/Vod Link/Vod\"Link"),
            "[Desktop Entry]\n\
Type=Application\n\
Name=VodLink\n\
Comment=Start VodLink minimized\n\
Exec=\"/opt/Vod Link/Vod\\\"Link\" --minimized\n\
Icon=vodlink\n\
Terminal=false\n\
X-GNOME-Autostart-enabled=true\n"
        );
    }
}
