use crate::models::{GameDefinition, InstalledGame};
use crate::repository::VodRepository;
use anyhow::{Context, Result};
use regex::Regex;
#[cfg(target_os = "windows")]
use serde_json::Value;
use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};
#[cfg(target_os = "windows")]
use std::process::Command;
use sysinfo::{ProcessesToUpdate, System};

const FALLBACK_GAMES: &[(&str, &[&str])] = &[
    ("Counter-Strike 2", &["cs2.exe", "cs2"]),
    ("VALORANT", &["valorant-win64-shipping.exe", "valorant.exe"]),
    ("League of Legends", &["league of legends.exe"]),
    ("Overwatch 2", &["overwatch.exe"]),
    ("Fortnite", &["fortniteclient-win64-shipping.exe"]),
    ("Dota 2", &["dota2.exe", "dota2"]),
    ("Rocket League", &["rocketleague.exe"]),
    ("Apex Legends", &["r5apex.exe"]),
    ("Warframe", &["warframe.x64.exe"]),
    ("Destiny 2", &["destiny2.exe"]),
    ("World of Warcraft", &["wow.exe"]),
    ("Hearthstone", &["hearthstone.exe"]),
    ("Diablo IV", &["diablo iv.exe"]),
    ("StarCraft II", &["starcraft ii.exe", "sc2.exe"]),
    ("Team Fortress 2", &["tf2.exe"]),
    ("Portal 2", &["portal2.exe", "portal2_linux"]),
    ("Among Us", &["amongus.exe"]),
    ("Fall Guys", &["fallguys_client.exe"]),
    ("Balatro", &["balatro.exe", "balatro"]),
    ("Vampire Survivors", &["vampire survivors.exe"]),
];

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
    definitions: Vec<GameDefinition>,
    installed: Vec<InstalledGame>,
}

impl GameCatalog {
    pub(crate) fn load(repository: &VodRepository) -> Result<Self> {
        let mut definitions = FALLBACK_GAMES.iter()
            .map(|(name, executables)| GameDefinition::new(*name, executables.iter().copied()))
            .collect::<Vec<_>>();
        for (exe, display) in repository.user_games()? {
            definitions.push(GameDefinition::new(display, [exe]));
        }
        definitions.sort_by_key(|definition| definition.name.to_lowercase());
        let installed = discover_installed_games();
        Ok(Self { definitions, installed })
    }

    pub(crate) fn refresh_installed(&mut self) {
        self.installed = discover_installed_games();
    }

    pub(crate) fn identify(&self, executable: &Path) -> Option<(String, Vec<String>)> {
        let file = executable.file_name()?.to_string_lossy().to_lowercase();
        for definition in &self.definitions {
            if definition.process_names.iter().any(|candidate| candidate == &file) {
                return Some((definition.name.clone(), definition.process_names.clone()));
            }
        }
        let normalized = normalize_dir(executable.parent().unwrap_or(executable));
        let mut best: Option<&InstalledGame> = None;
        for game in &self.installed {
            if normalized.starts_with(&game.install_dir)
                && best.is_none_or(|previous| game.install_dir.len() > previous.install_dir.len())
            {
                best = Some(game);
            }
        }
        best.map(|game| (game.name.clone(), vec![file]))
    }
}

#[derive(Clone, Debug)]
pub(crate) struct DetectedGame {
    pub(crate) name: String,
    pub(crate) executable: PathBuf,
    pub(crate) process_names: Vec<String>,
}

pub(crate) struct GameDetector {
    system: System,
    catalog: GameCatalog,
    baseline_complete: bool,
    active_games: HashMap<String, DetectedGame>,
    scan_count: u64,
}

impl GameDetector {
    pub(crate) fn new(catalog: GameCatalog) -> Self {
        Self {
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
        self.system.refresh_processes(ProcessesToUpdate::All, true);
        let mut now = HashMap::<String, DetectedGame>::new();

        for process in self.system.processes().values() {
            let Some(executable) = process.exe() else { continue };
            let file = executable.file_name()
                .map(|name| name.to_string_lossy().to_lowercase())
                .unwrap_or_default();
            if file.is_empty() || PROCESS_DENYLIST.iter().any(|deny| *deny == file) {
                continue;
            }
            if let Some((name, process_names)) = self.catalog.identify(executable) {
                now.entry(name.clone()).or_insert_with(|| DetectedGame {
                    name,
                    executable: executable.to_path_buf(),
                    process_names,
                });
            }
        }

        if !self.baseline_complete {
            self.active_games = now;
            self.baseline_complete = true;
            return (Vec::new(), Vec::new());
        }

        let started = now.iter()
            .filter(|(name, _)| !self.active_games.contains_key(*name))
            .map(|(_, game)| game.clone())
            .collect::<Vec<_>>();
        let stopped = self.active_games.keys()
            .filter(|name| !now.contains_key(*name))
            .cloned()
            .collect::<Vec<_>>();
        self.active_games = now;
        (started, stopped)
    }
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
        if let Some(program_files) = std::env::var_os("PROGRAMFILES(X86)") {
            roots.push(PathBuf::from(program_files).join("Steam"));
        }
    }
    #[cfg(target_os = "macos")]
    {
        if let Some(home) = directories::BaseDirs::new().map(|base| base.home_dir().to_path_buf()) {
            roots.push(home.join("Library").join("Application Support").join("Steam"));
        }
    }
    #[cfg(all(not(target_os = "windows"), not(target_os = "macos")))]
    {
        if let Some(home) = directories::BaseDirs::new().map(|base| base.home_dir().to_path_buf()) {
            roots.extend([
                home.join(".steam/root"),
                home.join(".steam/steam"),
                home.join(".local/share/Steam"),
                home.join(".var/app/com.valvesoftware.Steam/.steam/steam"),
            ]);
        }
    }

    roots.retain(|root| root.exists());
    let path_re = Regex::new(r#""path"\s*"([^"]+)""#).expect("static Steam path regex");
    let mut library_roots = HashSet::<PathBuf>::new();
    for root in roots {
        library_roots.insert(root.clone());
        let folders = root.join("steamapps").join("libraryfolders.vdf");
        if let Ok(text) = std::fs::read_to_string(folders) {
            for cap in path_re.captures_iter(&text) {
                if let Some(path) = cap.get(1) {
                    library_roots.insert(PathBuf::from(path.as_str().replace("\\\\", "\\")));
                }
            }
        }
    }

    let field_re = Regex::new(r#""(appid|name|installdir)"\s*"([^"]*)""#)
        .expect("static Steam manifest regex");
    for root in library_roots {
        let steamapps = root.join("steamapps");
        let Ok(entries) = std::fs::read_dir(&steamapps) else { continue };
        for entry in entries.flatten() {
            let file_name = entry.file_name().to_string_lossy().into_owned();
            if !file_name.starts_with("appmanifest_") || !file_name.ends_with(".acf") {
                continue;
            }
            let Ok(text) = std::fs::read_to_string(entry.path()) else { continue };
            let mut fields = HashMap::<String, String>::new();
            for capture in field_re.captures_iter(&text) {
                fields.insert(capture[1].to_owned(), capture[2].to_owned());
            }
            let (Some(name), Some(install_dir)) = (fields.get("name"), fields.get("installdir")) else {
                continue;
            };
            let full = steamapps.join("common").join(install_dir);
            if full.exists() {
                out.push(InstalledGame {
                    name: name.clone(),
                    install_dir: normalize_dir(&full),
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
        if !name.is_empty() && !install.is_empty() && Path::new(install).exists() {
            out.push(InstalledGame {
                name: name.to_owned(),
                install_dir: normalize_dir(Path::new(install)),
            });
        }
    }
}

fn normalize_dir(path: &Path) -> String {
    path.to_string_lossy()
        .replace('\\', "/")
        .trim_end_matches('/')
        .to_lowercase()
}

pub(crate) fn add_manual_game(repository: &VodRepository, executable: &Path, display_name: &str) -> Result<()> {
    let file = executable.file_name()
        .context("Selected executable has no file name")?
        .to_string_lossy()
        .to_lowercase();
    repository.set_user_game(&file, display_name)
}
