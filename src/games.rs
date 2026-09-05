use crate::models::InstalledGame;
use crate::repository::VodRepository;
use anyhow::{Context, Result};
use regex::Regex;
#[cfg(target_os = "windows")]
use serde_json::Value;
use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};
#[cfg(target_os = "windows")]
use std::process::Command;
#[cfg(not(target_os = "linux"))]
use sysinfo::{ProcessesToUpdate, System};

const FALLBACK_GAMES: &[(&str, &str)] = &[
    ("cs2.exe", "Counter-Strike 2"),
    ("valorant-win64-shipping.exe", "VALORANT"),
    ("valorant.exe", "VALORANT"),
    ("league of legends.exe", "League of Legends"),
    ("overwatch.exe", "Overwatch 2"),
    ("fortniteclient-win64-shipping.exe", "Fortnite"),
    ("dota2.exe", "Dota 2"),
    ("rocketleague.exe", "Rocket League"),
    ("r5apex.exe", "Apex Legends"),
    ("warframe.x64.exe", "Warframe"),
    ("destiny2.exe", "Destiny 2"),
    ("wow.exe", "World of Warcraft"),
    ("hearthstone.exe", "Hearthstone"),
    ("diablo iv.exe", "Diablo IV"),
    ("starcraft ii.exe", "StarCraft II"),
    ("sc2.exe", "StarCraft II"),
    ("tf2.exe", "Team Fortress 2"),
    ("portal2.exe", "Portal 2"),
    ("amongus.exe", "Among Us"),
    ("fallguys_client.exe", "Fall Guys"),
    ("balatro.exe", "Balatro"),
    ("vampire survivors.exe", "Vampire Survivors"),
];

const NATIVE_ALIASES: &[(&str, &str)] = &[
    ("cs2", "Counter-Strike 2"),
    ("dota2", "Dota 2"),
    ("portal2_linux", "Portal 2"),
    ("balatro", "Balatro"),
];

#[cfg(all(feature = "desktop", target_os = "windows"))]
const PROCESS_DENYLIST: &[&str] = &[
    "explorer.exe", "finder", "gnome-shell", "kwin_x11", "kwin_wayland",
    "chrome.exe", "msedge.exe", "firefox.exe", "safari", "brave.exe",
    "code.exe", "devenv.exe", "idea64.exe", "discord.exe", "slack.exe",
    "spotify.exe", "steam.exe", "epicgameslauncher.exe", "battle.net.exe",
    "riotclientservices.exe", "winword.exe", "excel.exe", "powerpnt.exe",
    "vlc.exe", "zoom.exe", "taskmgr.exe", "powershell.exe", "cmd.exe",
    "terminal.exe", "windowsterminal.exe", "obs64.exe", "vodlink.exe",
];

#[derive(Clone)]
pub(crate) struct GameCatalog {
    fallbacks: HashMap<String, String>,
    user: HashMap<String, String>,
    installed: Vec<InstalledGame>,
}

impl GameCatalog {
    pub(crate) fn load(repository: &VodRepository) -> Result<Self> {
        let mut fallbacks = HashMap::<String, String>::new();
        for (executable, game) in FALLBACK_GAMES {
            let executable = executable.to_lowercase();
            fallbacks.insert(executable.clone(), (*game).to_owned());
            if let Some(alias) = executable.strip_suffix(".exe")
                && !alias.is_empty()
            {
                fallbacks.entry(alias.to_owned()).or_insert_with(|| (*game).to_owned());
            }
        }
        for (executable, game) in NATIVE_ALIASES {
            fallbacks.insert((*executable).to_owned(), (*game).to_owned());
        }

        let user = repository
            .user_games()?
            .into_iter()
            .map(|(key, value)| (normalize_mapping_key(&key), value))
            .collect();
        let installed = discover_installed_games();
        Ok(Self { fallbacks, user, installed })
    }

    pub(crate) fn refresh_installed(&mut self) {
        self.installed = discover_installed_games();
    }

    pub(crate) fn identify(&self, executable: &str, full_path: &Path) -> Option<String> {
        let executable = executable.trim().to_lowercase();
        let executable_no_exe = strip_app_suffix(&executable);
        let full_path = normalize_path(full_path);

        if let Some(game) = self.user.get(&full_path) {
            return Some(game.clone());
        }
        if !full_path.is_empty() {
            for (key, game) in &self.user {
                let Some(slash) = key.rfind('/') else { continue };
                if slash < 4 {
                    continue;
                }
                let install_dir = &key[..slash];
                if full_path != *key && full_path.starts_with(&format!("{install_dir}/")) {
                    return Some(game.clone());
                }
            }
        }
        if let Some(game) = self.user.get(&executable) {
            return Some(game.clone());
        }
        if executable_no_exe != executable
            && let Some(game) = self.user.get(&executable_no_exe)
        {
            return Some(game.clone());
        }

        if !full_path.is_empty() {
            for game in &self.installed {
                if full_path == game.install_dir
                    || full_path.starts_with(&format!("{}/", game.install_dir))
                {
                    return Some(game.name.clone());
                }
            }
        }

        self.fallbacks
            .get(&executable)
            .or_else(|| self.fallbacks.get(&executable_no_exe))
            .cloned()
    }
}

#[derive(Clone, Debug)]
pub(crate) struct DetectedGame {
    pub(crate) name: String,
    pub(crate) executable: PathBuf,
    pub(crate) process_names: Vec<String>,
}

#[derive(Clone, Debug, PartialEq, Eq, Hash)]
struct ProcessCandidate {
    name: String,
    path: PathBuf,
}

pub(crate) struct GameDetector {
    #[cfg(not(target_os = "linux"))]
    system: System,
    catalog: GameCatalog,
    baseline_complete: bool,
    active_games: HashMap<String, DetectedGame>,
    scan_count: u64,
}

impl GameDetector {
    pub(crate) fn new(catalog: GameCatalog) -> Self {
        Self {
            #[cfg(not(target_os = "linux"))]
            system: System::new(),
            catalog,
            baseline_complete: false,
            active_games: HashMap::new(),
            scan_count: 0,
        }
    }

    pub(crate) fn scan(&mut self) -> (Vec<DetectedGame>, Vec<String>) {
        self.scan_count = self.scan_count.saturating_add(1);
        if self.scan_count.is_multiple_of(150) {
            self.catalog.refresh_installed();
        }

        let candidates = self.scan_processes();
        let mut now = HashMap::<String, DetectedGame>::new();
        for process in candidates {
            if process.name.is_empty() {
                continue;
            }
            let Some(game_name) = self.catalog.identify(&process.name, &process.path) else {
                continue;
            };
            let entry = now.entry(game_name.clone()).or_insert_with(|| DetectedGame {
                name: game_name,
                executable: process.path.clone(),
                process_names: Vec::new(),
            });
            if !entry.process_names.contains(&process.name) {
                entry.process_names.push(process.name);
            }
            if entry.executable.as_os_str().is_empty() && !process.path.as_os_str().is_empty() {
                entry.executable = process.path;
            }
        }

        if !self.baseline_complete {
            self.active_games = now;
            self.baseline_complete = true;
            return (Vec::new(), Vec::new());
        }

        let started = now
            .iter()
            .filter(|(name, _)| !self.active_games.contains_key(*name))
            .map(|(_, game)| game.clone())
            .collect::<Vec<_>>();
        let stopped = self
            .active_games
            .keys()
            .filter(|name| !now.contains_key(*name))
            .cloned()
            .collect::<Vec<_>>();
        self.active_games = now;
        (started, stopped)
    }

    #[cfg(feature = "desktop")]
    pub(crate) fn running_process_names(&mut self) -> Vec<String> {
        #[cfg(target_os = "windows")]
        {
            return vodlink_native_parity::windowed_process_names()
                .into_iter()
                .filter(|name| !is_denylisted(name))
                .collect();
        }
        #[cfg(not(target_os = "windows"))]
        {
            let mut names = self
                .scan_processes()
                .into_iter()
                .map(|process| process.name)
                .filter(|name| !name.is_empty())
                .collect::<Vec<_>>();
            names.sort();
            names.dedup();
            names
        }
    }

    #[cfg(not(target_os = "linux"))]
    fn scan_processes(&mut self) -> Vec<ProcessCandidate> {
        self.system.refresh_processes(ProcessesToUpdate::All, true);
        self.system
            .processes()
            .values()
            .filter_map(|process| {
                let executable = process.exe()?;
                let name = executable
                    .file_name()
                    .map(|value| value.to_string_lossy().trim().to_lowercase())
                    .unwrap_or_default();
                Some(ProcessCandidate {
                    name,
                    path: executable.to_path_buf(),
                })
            })
            .collect()
    }

    #[cfg(target_os = "linux")]
    fn scan_processes(&mut self) -> Vec<ProcessCandidate> {
        scan_linux_processes()
    }
}

#[cfg(all(target_os = "linux", miri))]
fn scan_linux_processes() -> Vec<ProcessCandidate> {
    Vec::new()
}

#[cfg(all(target_os = "linux", not(miri)))]
fn scan_linux_processes() -> Vec<ProcessCandidate> {
    let mut result = Vec::new();
    let mut seen = HashSet::<String>::new();
    let Ok(entries) = std::fs::read_dir("/proc") else {
        return result;
    };

    for entry in entries.flatten() {
        let pid = entry.file_name().to_string_lossy().into_owned();
        if pid.parse::<u32>().is_err() {
            continue;
        }
        let base = entry.path();
        let comm = read_text_file(&base.join("comm")).to_lowercase();
        let exe_path = std::fs::read_link(base.join("exe")).unwrap_or_default();
        let cwd_path = std::fs::read_link(base.join("cwd")).unwrap_or_default();
        let cmdline = read_cmdline(&base.join("cmdline"));

        add_process_candidate(&mut result, &mut seen, &comm, &exe_path);
        for argument in cmdline {
            let path = PathBuf::from(&argument);
            if path.is_absolute() && path.exists() {
                let name = path
                    .file_name()
                    .map(|value| value.to_string_lossy().into_owned())
                    .unwrap_or_default();
                add_process_candidate(&mut result, &mut seen, &name, &path);
            }
        }
        if looks_like_game_path(&cwd_path) {
            add_process_candidate(&mut result, &mut seen, &comm, &cwd_path);
        }
    }
    result
}

#[cfg(all(target_os = "linux", not(miri)))]
fn read_text_file(path: &Path) -> String {
    std::fs::read(path)
        .ok()
        .map(|data| String::from_utf8_lossy(&data).trim().to_owned())
        .unwrap_or_default()
}

#[cfg(all(target_os = "linux", not(miri)))]
fn read_cmdline(path: &Path) -> Vec<String> {
    let Ok(data) = std::fs::read(path) else {
        return Vec::new();
    };
    data.split(|byte| *byte == 0)
        .filter_map(|part| {
            let value = String::from_utf8_lossy(part).trim().to_owned();
            (!value.is_empty()).then_some(value)
        })
        .collect()
}

#[cfg(all(target_os = "linux", not(miri)))]
fn looks_like_game_path(path: &Path) -> bool {
    let normalized = normalize_path(path);
    normalized.contains("/steamapps/common/")
        || normalized.contains("/gog games/")
        || normalized.contains("/heroic/")
        || normalized.contains("/lutris/")
}

#[cfg(all(target_os = "linux", not(miri)))]
fn add_process_candidate(
    out: &mut Vec<ProcessCandidate>,
    seen: &mut HashSet<String>,
    name: &str,
    path: &Path,
) {
    let lower_path = normalize_path(path);
    let mut lower_name = name.trim().to_lowercase();
    if lower_name.is_empty() && !lower_path.is_empty() {
        lower_name = Path::new(&lower_path)
            .file_name()
            .map(|value| value.to_string_lossy().to_lowercase())
            .unwrap_or_default();
    }
    if lower_name.is_empty() && lower_path.is_empty() {
        return;
    }
    let key = format!("{lower_name}|{lower_path}");
    if seen.insert(key) {
        out.push(ProcessCandidate {
            name: lower_name,
            path: PathBuf::from(lower_path),
        });
    }
}

#[cfg(all(feature = "desktop", target_os = "windows"))]
fn is_denylisted(name: &str) -> bool {
    let lower = name.trim().to_lowercase();
    PROCESS_DENYLIST.contains(&lower.as_str())
}

fn discover_installed_games() -> Vec<InstalledGame> {
    let mut games = Vec::new();
    discover_steam(&mut games);
    #[cfg(target_os = "windows")]
    discover_epic(&mut games);
    games.sort_by_key(|game| game.name.to_lowercase());
    games.dedup_by(|a, b| a.install_dir == b.install_dir);
    games
}

fn discover_steam(out: &mut Vec<InstalledGame>) {
    let mut roots = Vec::<PathBuf>::new();
    #[cfg(target_os = "windows")]
    {
        if let Ok(output) = Command::new("reg")
            .args(["query", r"HKCU\Software\Valve\Steam", "/v", "SteamPath"])
            .output()
            && output.status.success()
        {
            let text = String::from_utf8_lossy(&output.stdout);
            for line in text.lines() {
                if line.contains("SteamPath")
                    && let Some(path) = line.split_whitespace().last()
                {
                    roots.push(PathBuf::from(path));
                }
            }
        }
    }
    #[cfg(target_os = "macos")]
    if let Some(home) = directories::BaseDirs::new().map(|base| base.home_dir().to_path_buf()) {
        roots.push(home.join("Library").join("Application Support").join("Steam"));
    }
    #[cfg(all(not(target_os = "windows"), not(target_os = "macos")))]
    if let Some(home) = directories::BaseDirs::new().map(|base| base.home_dir().to_path_buf()) {
        roots.extend([
            home.join(".steam/root"),
            home.join(".steam/steam"),
            home.join(".local/share/Steam"),
            home.join(".var/app/com.valvesoftware.Steam/.local/share/Steam"),
        ]);
    }

    let mut library_roots = Vec::<PathBuf>::new();
    let path_re = Regex::new(r#""path"\s*"([^"]+)""#).expect("static Steam path regex");
    let mut seen_roots = HashSet::<String>::new();
    for raw_root in roots {
        if !raw_root.exists() {
            continue;
        }
        let root = raw_root.canonicalize().unwrap_or(raw_root);
        let normalized = normalize_path(&root);
        if !seen_roots.insert(normalized) {
            continue;
        }
        library_roots.push(root.clone());
        let folders = root.join("steamapps").join("libraryfolders.vdf");
        if let Ok(text) = std::fs::read_to_string(folders) {
            for capture in path_re.captures_iter(&text) {
                if let Some(path) = capture.get(1) {
                    let decoded = path
                        .as_str()
                        .replace("\\\"", "\"")
                        .replace("\\\\", "\\");
                    library_roots.push(PathBuf::from(decoded));
                }
            }
        }
    }

    let name_re = Regex::new(r#""name"\s*"([^"]+)""#).expect("static name regex");
    let dir_re = Regex::new(r#""installdir"\s*"([^"]+)""#).expect("static dir regex");
    let mut seen_libraries = HashSet::<String>::new();
    for library in library_roots {
        if !seen_libraries.insert(normalize_path(&library)) {
            continue;
        }
        let steamapps = library.join("steamapps");
        let Ok(entries) = std::fs::read_dir(&steamapps) else { continue };
        for entry in entries.flatten() {
            let file_name = entry.file_name().to_string_lossy().into_owned();
            if !file_name.starts_with("appmanifest_") || !file_name.ends_with(".acf") {
                continue;
            }
            let Ok(text) = std::fs::read_to_string(entry.path()) else { continue };
            let Some(name) = name_re.captures(&text).and_then(|capture| capture.get(1)) else { continue };
            let Some(install_dir) = dir_re.captures(&text).and_then(|capture| capture.get(1)) else { continue };
            let full = steamapps.join("common").join(install_dir.as_str());
            if full.exists() {
                out.push(InstalledGame {
                    name: name.as_str().to_owned(),
                    install_dir: normalize_path(&full),
                });
            }
        }
    }
}

#[cfg(target_os = "windows")]
fn discover_epic(out: &mut Vec<InstalledGame>) {
    let Some(program_data) = std::env::var_os("PROGRAMDATA") else { return };
    let manifests = PathBuf::from(program_data)
        .join("Epic")
        .join("EpicGamesLauncher")
        .join("Data")
        .join("Manifests");
    let Ok(entries) = std::fs::read_dir(manifests) else { return };
    for entry in entries.flatten() {
        if entry.path().extension().and_then(|ext| ext.to_str()) != Some("item") {
            continue;
        }
        let Ok(text) = std::fs::read_to_string(entry.path()) else { continue };
        let Ok(value) = serde_json::from_str::<Value>(&text) else { continue };
        let name = value.get("DisplayName").and_then(Value::as_str).unwrap_or("").trim();
        let install = value.get("InstallLocation").and_then(Value::as_str).unwrap_or("").trim();
        if !name.is_empty() && !install.is_empty() {
            out.push(InstalledGame {
                name: name.to_owned(),
                install_dir: normalize_path(Path::new(install)),
            });
        }
    }
}

fn strip_app_suffix(value: &str) -> String {
    value
        .strip_suffix(".exe")
        .or_else(|| value.strip_suffix(".app"))
        .unwrap_or(value)
        .to_owned()
}

fn normalize_mapping_key(value: &str) -> String {
    value.trim().replace('\\', "/").to_lowercase()
}

fn normalize_path(path: &Path) -> String {
    let mut value = path.to_string_lossy().replace('\\', "/").to_lowercase();
    while value.contains("//") {
        value = value.replace("//", "/");
    }
    while value.ends_with('/') {
        value.pop();
    }
    value
}

pub(crate) fn add_manual_game(
    repository: &VodRepository,
    executable: &Path,
    display_name: &str,
) -> Result<()> {
    let display_name = display_name.trim();
    let raw = executable.to_string_lossy();
    if raw.trim().is_empty() || display_name.is_empty() {
        return Ok(());
    }

    let file_name = executable
        .file_name()
        .context("Selected executable has no file name")?
        .to_string_lossy()
        .trim()
        .to_lowercase();
    let full_path = executable
        .canonicalize()
        .unwrap_or_else(|_| executable.to_path_buf());
    let full_path = normalize_path(&full_path);
    let file_name_no_suffix = strip_app_suffix(&file_name);

    for key in [full_path, file_name, file_name_no_suffix] {
        if !key.trim().is_empty() {
            repository.set_user_game(&key, display_name)?;
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fallback_aliases_strip_exe_like_cpp() {
        let mut fallbacks = HashMap::new();
        for (executable, game) in FALLBACK_GAMES {
            fallbacks.insert((*executable).to_owned(), (*game).to_owned());
            if let Some(alias) = executable.strip_suffix(".exe") {
                fallbacks.insert(alias.to_owned(), (*game).to_owned());
            }
        }
        assert_eq!(
            fallbacks.get("overwatch").map(String::as_str),
            Some("Overwatch 2")
        );
    }

    #[test]
    fn mapping_keys_normalize_windows_separators() {
        assert_eq!(normalize_mapping_key(" C:\\Games\\Foo.EXE "), "c:/games/foo.exe");
    }
}