//! Desktop entries: the applications installed in the XDG data directories,
//! the launchers placed on the user's desktop, their menu categories, and how
//! an entry is launched.

use std::collections::HashSet;
use std::ffi::CString;
use std::os::unix::ffi::OsStrExt;
use std::path::{Path, PathBuf};

use crate::config::{self, Config};

/// Desktop entry categories folded into the menu's categories.
const CATEGORY_MAP: [(&str, &str); 16] = [
    ("AudioVideo", "Multimedia"),
    ("Audio", "Multimedia"),
    ("Video", "Multimedia"),
    ("Development", "Development"),
    ("Education", "Education"),
    ("Game", "Games"),
    ("Graphics", "Graphics"),
    ("Network", "Internet"),
    ("WebBrowser", "Internet"),
    ("Office", "Office"),
    ("Science", "Science"),
    ("Settings", "Settings"),
    ("System", "System"),
    ("Utility", "Utilities"),
    ("TerminalEmulator", "System"),
    ("FileManager", "System"),
];

/// The category of entries no mapping claims.
pub const MISC: &str = "Other";

/// The translated name of a menu category.
///
/// # Arguments
/// * `name` - The category as `CATEGORY_MAP` names it.
pub fn category_name(name: &str) -> &'static str {
    use crate::i18n::tr;
    match name {
        "Multimedia" => tr("Multimedia"),
        "Development" => tr("Development"),
        "Education" => tr("Education"),
        "Games" => tr("Games"),
        "Graphics" => tr("Graphics"),
        "Internet" => tr("Internet"),
        "Office" => tr("Office"),
        "Science" => tr("Science"),
        "Settings" => tr("Settings"),
        "System" => tr("System"),
        "Utilities" => tr("Utilities"),
        _ => tr("Other"),
    }
}

/// Terminal emulators tried for entries that need one, in order.
const TERMINALS: [&str; 10] = [
    "x-terminal-emulator",
    "foot",
    "xterm",
    "st",
    "stterm",
    "lxterminal",
    "xfce4-terminal",
    "konsole",
    "gnome-terminal",
    "weston-terminal",
];

/// Launches the desktop keeps in the recent list.
const RECENT_LIMIT: usize = 12;

/// One launchable desktop entry.
#[derive(Clone, Debug)]
pub struct Entry {
    /// The desktop file ID, its file name within the applications directory.
    pub id: String,
    /// Localized display name.
    pub name: String,
    /// Localized comment, shown as a tooltip.
    pub comment: Option<String>,
    /// Localized generic name, searched along with the name.
    pub generic_name: Option<String>,
    /// Keywords, searched along with the name.
    pub keywords: Vec<String>,
    /// The Exec line, unexpanded.
    pub exec: String,
    /// Icon name or path.
    pub icon: Option<String>,
    /// The desktop file.
    pub path: PathBuf,
    /// Whether the program runs in a terminal.
    pub terminal: bool,
    /// Working directory for the program.
    pub working_dir: Option<String>,
    /// Menu category the entry is listed under.
    pub category: String,
    /// The window class or application ID the entry's windows carry, when
    /// it differs from the desktop file ID.
    pub wm_class: Option<String>,
}

impl Entry {
    /// Whether the entry matches a search, case insensitively, on its name,
    /// generic name, keywords or executable.
    ///
    /// # Arguments
    /// * `needle` - The lowercase search text.
    pub fn matches(&self, needle: &str) -> bool {
        if needle.is_empty() {
            return true;
        }
        self.name.to_lowercase().contains(needle)
            || self
                .generic_name
                .as_ref()
                .is_some_and(|g| g.to_lowercase().contains(needle))
            || self.keywords.iter().any(|k| k.to_lowercase().contains(needle))
            || self.exec.split_whitespace().next().is_some_and(|e| {
                Path::new(e)
                    .file_name()
                    .and_then(|f| f.to_str())
                    .is_some_and(|f| f.to_lowercase().contains(needle))
            })
    }

    /// How well the entry matches a search, lower ranking first: a name that
    /// starts with the text, then a word of the name, then anything else.
    ///
    /// # Arguments
    /// * `needle` - The lowercase search text.
    pub fn rank(&self, needle: &str) -> u8 {
        let name = self.name.to_lowercase();
        if name.starts_with(needle) {
            0
        } else if name.split_whitespace().any(|w| w.starts_with(needle)) {
            1
        } else if name.contains(needle) {
            2
        } else {
            3
        }
    }
}

/// A menu category and the entries it lists, sorted by name.
#[derive(Clone, Debug, Default)]
pub struct Category {
    /// Display name.
    pub name: String,
    /// Indexes into the application list.
    pub entries: Vec<usize>,
}

/// Everything loaded from the desktop files.
#[derive(Default)]
pub struct Apps {
    /// Applications from the data directories, in load order.
    pub entries: Vec<Entry>,
    /// Categories sorted by name with the miscellaneous one last.
    pub categories: Vec<Category>,
    /// Launchers in the user's Desktop directory, sorted by name.
    pub desktop: Vec<Entry>,
}

impl Apps {
    /// Scans the applications directories and the Desktop directory.
    ///
    /// # Arguments
    /// * `config` - The settings, for the terminal to validate entries against.
    pub fn load(config: &Config) -> Apps {
        let locales = locale_suffixes();
        let terminal_available = find_terminal(config).is_some();
        let mut apps = Apps::default();
        let mut seen: HashSet<String> = HashSet::new();
        for dir in applications_dirs() {
            scan_dir(&dir, &locales, terminal_available, &mut seen, &mut apps.entries);
        }
        apps.build_categories();
        apps.desktop = load_desktop_dir(&locales);
        apps
    }

    /// Groups the entries into sorted categories.
    fn build_categories(&mut self) {
        let mut categories: Vec<Category> = Vec::new();
        for (index, entry) in self.entries.iter().enumerate() {
            match categories.iter_mut().find(|c| c.name == entry.category) {
                Some(category) => category.entries.push(index),
                None => categories.push(Category {
                    name: entry.category.clone(),
                    entries: vec![index],
                }),
            }
        }
        for category in &mut categories {
            category.entries.sort_by(|a, b| {
                self.entries[*a]
                    .name
                    .to_lowercase()
                    .cmp(&self.entries[*b].name.to_lowercase())
            });
        }
        categories.sort_by(|a, b| match (a.name == MISC, b.name == MISC) {
            (true, false) => std::cmp::Ordering::Greater,
            (false, true) => std::cmp::Ordering::Less,
            _ => a.name.to_lowercase().cmp(&b.name.to_lowercase()),
        });
        self.categories = categories;
    }

    /// Every application index sorted by name.
    pub fn all_sorted(&self) -> Vec<usize> {
        let mut all: Vec<usize> = (0..self.entries.len()).collect();
        all.sort_by(|a, b| {
            self.entries[*a]
                .name
                .to_lowercase()
                .cmp(&self.entries[*b].name.to_lowercase())
        });
        all
    }

    /// Finds an application by desktop file ID.
    ///
    /// # Arguments
    /// * `id` - The desktop file ID.
    pub fn find(&self, id: &str) -> Option<usize> {
        self.entries.iter().position(|e| e.id == id)
    }

    /// Applications matching a search, best matches first.
    ///
    /// # Arguments
    /// * `query` - The search text as typed.
    pub fn search(&self, query: &str) -> Vec<usize> {
        let needle = query.trim().to_lowercase();
        if needle.is_empty() {
            return Vec::new();
        }
        let mut hits: Vec<(u8, usize)> = self
            .entries
            .iter()
            .enumerate()
            .filter(|(_, e)| e.matches(&needle))
            .map(|(i, e)| (e.rank(&needle), i))
            .collect();
        hits.sort_by(|a, b| {
            a.0.cmp(&b.0).then_with(|| {
                self.entries[a.1]
                    .name
                    .to_lowercase()
                    .cmp(&self.entries[b.1].name.to_lowercase())
            })
        });
        hits.into_iter().map(|(_, i)| i).collect()
    }
}

/// The icon of the application whose windows carry an application ID or
/// class: the entry naming it as its window class, the entry with that
/// desktop file ID, else the entry whose ID is the longest prefix of it, the
/// way a helper process such as `steamwebhelper` belongs to `steam`.
///
/// # Arguments
/// * `entries` - The loaded applications.
/// * `app_id` - The application ID or window class.
pub fn icon_for_app_id(entries: &[Entry], app_id: &str) -> Option<String> {
    let lower = app_id.to_lowercase();
    if let Some(icon) = entries
        .iter()
        .find(|e| e.wm_class.as_ref().is_some_and(|c| c.to_lowercase() == lower))
        .and_then(|e| e.icon.clone())
    {
        return Some(icon);
    }
    if let Some(icon) = entries
        .iter()
        .find(|e| e.id.to_lowercase() == format!("{lower}.desktop"))
        .and_then(|e| e.icon.clone())
    {
        return Some(icon);
    }
    entries
        .iter()
        .filter_map(|e| {
            let stem = e.id.strip_suffix(".desktop").unwrap_or(&e.id).to_lowercase();
            (stem.len() >= 4 && lower.starts_with(&stem) && e.icon.is_some()).then_some((stem.len(), e))
        })
        .max_by_key(|(len, _)| *len)
        .and_then(|(_, e)| e.icon.clone())
}

/// The applications directories in precedence order: the user's data home,
/// then each system data directory.
pub fn applications_dirs() -> Vec<PathBuf> {
    let mut dirs = vec![config::data_home().join("applications")];
    for dir in config::data_dirs() {
        dirs.push(dir.join("applications"));
    }
    dirs
}

/// The user's Desktop directory.
pub fn desktop_dir() -> PathBuf {
    let user_dirs = config::home().join(".config/user-dirs.dirs");
    if let Ok(text) = std::fs::read_to_string(user_dirs) {
        for line in text.lines() {
            if let Some(value) = line.trim().strip_prefix("XDG_DESKTOP_DIR=") {
                let value = value.trim_matches('"');
                let path = value.replace("$HOME", &config::home().to_string_lossy());
                return PathBuf::from(path);
            }
        }
    }
    config::home().join("Desktop")
}

/// Locale suffixes for localized keys, most specific first: `ll_CC@mod`,
/// `ll_CC`, `ll@mod`, `ll`.
fn locale_suffixes() -> Vec<String> {
    let raw = ["LC_ALL", "LC_MESSAGES", "LANG"]
        .iter()
        .find_map(|var| std::env::var(var).ok().filter(|v| !v.is_empty()))
        .unwrap_or_default();
    let raw = raw.trim();
    if raw.is_empty() || raw == "C" || raw == "POSIX" {
        return Vec::new();
    }
    let (without_modifier, modifier) = match raw.split_once('@') {
        Some((base, m)) => (base, Some(m)),
        None => (raw, None),
    };
    let without_encoding = without_modifier
        .split_once('.')
        .map(|(b, _)| b)
        .unwrap_or(without_modifier);
    let (lang, country) = match without_encoding.split_once('_') {
        Some((l, c)) => (l, Some(c)),
        None => (without_encoding, None),
    };
    let mut suffixes = Vec::new();
    if let (Some(c), Some(m)) = (country, modifier) {
        suffixes.push(format!("{lang}_{c}@{m}"));
    }
    if let Some(c) = country {
        suffixes.push(format!("{lang}_{c}"));
    }
    if let Some(m) = modifier {
        suffixes.push(format!("{lang}@{m}"));
    }
    suffixes.push(lang.to_string());
    suffixes
}

/// The keys of one `[Desktop Entry]` group.
#[derive(Default)]
struct Group {
    keys: Vec<(String, String)>,
}

impl Group {
    /// The value of a plain key.
    fn get(&self, key: &str) -> Option<&str> {
        self.keys.iter().find(|(k, _)| k == key).map(|(_, v)| v.as_str())
    }

    /// The best localized value of a key, falling back to the plain one.
    fn localized(&self, key: &str, locales: &[String]) -> Option<&str> {
        for suffix in locales {
            let wanted = format!("{key}[{suffix}]");
            if let Some(value) = self.get(&wanted) {
                return Some(value);
            }
        }
        self.get(key)
    }

    /// Whether a boolean key is set to true.
    fn flag(&self, key: &str) -> bool {
        self.get(key).is_some_and(|v| v.trim() == "true")
    }
}

/// Reads the `[Desktop Entry]` group of a file.  Lines longer than the limit
/// are kept whole; other groups are ignored.
fn read_entry_group(path: &Path) -> Option<Group> {
    let text = std::fs::read_to_string(path).ok()?;
    let mut group = Group::default();
    let mut in_entry = false;
    for line in text.lines() {
        let line = line.trim_end_matches('\r');
        if line.starts_with('[') {
            in_entry = line.trim() == "[Desktop Entry]";
            continue;
        }
        if !in_entry || line.trim().is_empty() || line.starts_with('#') {
            continue;
        }
        if let Some((key, value)) = line.split_once('=') {
            group
                .keys
                .push((key.trim().to_string(), value.trim().to_string()));
        }
    }
    if group.keys.is_empty() { None } else { Some(group) }
}

/// Builds an entry from a parsed group, or `None` when it is not a visible
/// application.
///
/// Entries are left out when they are not applications, set NoDisplay or
/// Hidden, name a TryExec that is not installed, run a program that exists but
/// may not be executed (how the hardened images disable terminals), or need a
/// terminal when none can run.  OnlyShowIn and NotShowIn are deliberately
/// ignored, since the panel is not a desktop environment of its own.
fn build_entry(
    path: &Path,
    group: &Group,
    locales: &[String],
    terminal_available: bool,
    strict: bool,
) -> Option<Entry> {
    if strict {
        if group.get("Type").is_some_and(|t| t != "Application") {
            return None;
        }
        if group.flag("NoDisplay") || group.flag("Hidden") {
            return None;
        }
        if let Some(try_exec) = group.get("TryExec").filter(|v| !v.is_empty())
            && resolve_program(try_exec).is_none()
        {
            return None;
        }
    } else if group.get("Type").is_some_and(|t| t != "Application") {
        return None;
    }
    let name = group.localized("Name", locales)?.to_string();
    let exec = group.get("Exec")?.to_string();
    if name.is_empty() || exec.trim().is_empty() {
        return None;
    }
    let terminal = group.flag("Terminal");
    if strict {
        let program = exec_argv(&exec, None, "", path).into_iter().next()?;
        if program_disabled(&program) {
            return None;
        }
        if terminal && !terminal_available {
            return None;
        }
    }
    let keywords = group
        .localized("Keywords", locales)
        .map(|k| {
            k.split(';')
                .map(|s| s.trim().to_string())
                .filter(|s| !s.is_empty())
                .collect()
        })
        .unwrap_or_default();
    let category = group
        .get("Categories")
        .and_then(|cats| {
            let listed: Vec<&str> = cats.split(';').map(|c| c.trim()).collect();
            CATEGORY_MAP
                .iter()
                .find(|(from, _)| listed.contains(from))
                .map(|(_, to)| to.to_string())
        })
        .unwrap_or_else(|| MISC.to_string());
    Some(Entry {
        id: path
            .file_name()
            .map(|f| f.to_string_lossy().to_string())
            .unwrap_or_default(),
        name,
        comment: group
            .localized("Comment", locales)
            .filter(|c| !c.is_empty())
            .map(str::to_string),
        generic_name: group
            .localized("GenericName", locales)
            .filter(|c| !c.is_empty())
            .map(str::to_string),
        keywords,
        exec,
        icon: group.get("Icon").filter(|i| !i.is_empty()).map(str::to_string),
        path: path.to_path_buf(),
        terminal,
        working_dir: group.get("Path").filter(|p| !p.is_empty()).map(str::to_string),
        category,
        wm_class: group
            .get("StartupWMClass")
            .filter(|c| !c.is_empty())
            .map(str::to_string),
    })
}

/// Scans one applications directory.  A desktop file ID already claimed by a
/// directory of higher precedence is skipped, so a file in the user's data
/// directory replaces or hides the system file of the same name.
fn scan_dir(
    dir: &Path,
    locales: &[String],
    terminal_available: bool,
    seen: &mut HashSet<String>,
    out: &mut Vec<Entry>,
) {
    let Ok(read) = std::fs::read_dir(dir) else {
        return;
    };
    let mut files: Vec<PathBuf> = read
        .flatten()
        .map(|e| e.path())
        .filter(|p| p.extension().is_some_and(|e| e == "desktop") && p.is_file())
        .collect();
    files.sort();
    for path in files {
        let id = path
            .file_name()
            .map(|f| f.to_string_lossy().to_string())
            .unwrap_or_default();
        if !seen.insert(id) {
            continue;
        }
        let Some(group) = read_entry_group(&path) else {
            continue;
        };
        if let Some(entry) = build_entry(&path, &group, locales, terminal_available, true) {
            out.push(entry);
        }
    }
}

/// Reads the launchers on the user's desktop; these are shown as placed, so
/// only the entry type is checked.
fn load_desktop_dir(locales: &[String]) -> Vec<Entry> {
    let dir = desktop_dir();
    let Ok(read) = std::fs::read_dir(&dir) else {
        return Vec::new();
    };
    let mut entries = Vec::new();
    for path in read.flatten().map(|e| e.path()) {
        if path.extension().is_none_or(|e| e != "desktop") || !path.is_file() {
            continue;
        }
        let Some(group) = read_entry_group(&path) else {
            continue;
        };
        if let Some(entry) = build_entry(&path, &group, locales, true, false) {
            entries.push(entry);
        }
    }
    entries.sort_by(|a, b| a.name.to_lowercase().cmp(&b.name.to_lowercase()));
    entries
}

/// The directories searched for programs: PATH plus the user's local bin,
/// which login shells add but the session may not have.
fn search_path() -> Vec<PathBuf> {
    let raw = std::env::var("PATH").ok().filter(|p| !p.is_empty());
    let raw = raw.unwrap_or_else(|| "/usr/local/bin:/usr/bin:/bin".to_string());
    let mut dirs: Vec<PathBuf> = raw
        .split(':')
        .filter(|d| !d.is_empty())
        .map(PathBuf::from)
        .collect();
    let local = config::home().join(".local/bin");
    if !dirs.contains(&local) {
        dirs.push(local);
    }
    dirs
}

/// Whether a file exists and may be executed by this user.
fn is_executable(path: &Path) -> bool {
    let Ok(c_path) = CString::new(path.as_os_str().as_bytes()) else {
        return false;
    };
    unsafe { libc::access(c_path.as_ptr(), libc::X_OK) == 0 }
}

/// Finds a program as an absolute path or on the search path.
///
/// # Arguments
/// * `program` - A path or a bare program name.
///
/// # Returns
/// The executable's path, or `None` when none can be run.
pub fn resolve_program(program: &str) -> Option<PathBuf> {
    let program = program.trim();
    if program.is_empty() {
        return None;
    }
    if program.contains('/') {
        let path = PathBuf::from(program);
        return is_executable(&path).then_some(path);
    }
    search_path()
        .into_iter()
        .map(|dir| dir.join(program))
        .find(|candidate| is_executable(candidate))
}

/// Whether a program is installed but may not be run, or is an absolute path
/// that does not exist.  A bare name found nowhere is not disabled, since it
/// may be provided by a wrapper the session cannot see.
fn program_disabled(program: &str) -> bool {
    if program.contains('/') {
        return !is_executable(Path::new(program));
    }
    let mut present = false;
    for dir in search_path() {
        let candidate = dir.join(program);
        if candidate.is_file() {
            present = true;
            if is_executable(&candidate) {
                return false;
            }
        }
    }
    present
}

/// The terminal command to wrap entries in, as argv without the program to
/// run.  Under Wayland foot is tried before the distribution's alternatives
/// link, which points at an X terminal; foot cannot run without Wayland, so
/// elsewhere the link keeps its say.
///
/// # Arguments
/// * `config` - The settings, whose `terminal` takes precedence.
pub fn find_terminal(config: &Config) -> Option<Vec<String>> {
    let configured = config
        .terminal
        .clone()
        .or_else(|| std::env::var("TERMINAL").ok())
        .filter(|t| !t.is_empty());
    if let Some(command) = configured {
        let mut argv = split_exec(&command);
        if let Some(program) = argv.first()
            && resolve_program(program).is_some()
        {
            if argv.len() == 1 {
                argv.push("-e".to_string());
            }
            return Some(argv);
        }
    }
    let wayland = std::env::var_os("WAYLAND_DISPLAY").is_some_and(|d| !d.is_empty());
    let preferred = if wayland { Some("foot") } else { None };
    preferred
        .into_iter()
        .chain(TERMINALS.iter().copied())
        .find(|t| resolve_program(t).is_some())
        .map(|t| vec![t.to_string(), "-e".to_string()])
}

/// Splits an Exec value into arguments following the desktop entry quoting
/// rules: spaces separate, double quotes group, and inside quotes a backslash
/// escapes the next character.
///
/// # Arguments
/// * `exec` - The Exec value.
pub fn split_exec(exec: &str) -> Vec<String> {
    let mut args = Vec::new();
    let mut current = String::new();
    let mut in_quotes = false;
    let mut has_arg = false;
    let mut chars = exec.chars().peekable();
    while let Some(c) = chars.next() {
        match c {
            '"' => {
                in_quotes = !in_quotes;
                has_arg = true;
            }
            '\\' if in_quotes => {
                if let Some(next) = chars.next() {
                    current.push(next);
                }
            }
            ' ' | '\t' if !in_quotes => {
                if has_arg || !current.is_empty() {
                    args.push(std::mem::take(&mut current));
                    has_arg = false;
                }
            }
            _ => current.push(c),
        }
    }
    if has_arg || !current.is_empty() {
        args.push(current);
    }
    args
}

/// Expands the field codes of an Exec value.  File and URL codes expand to
/// nothing since the panel launches without arguments, `%i` to the icon
/// option, `%c` to the name and `%k` to the desktop file path.
///
/// # Arguments
/// * `exec` - The Exec value.
/// * `icon` - The entry's icon, for `%i`.
/// * `name` - The entry's name, for `%c`.
/// * `path` - The desktop file, for `%k`.
pub fn exec_argv(exec: &str, icon: Option<&str>, name: &str, path: &Path) -> Vec<String> {
    exec_argv_files(exec, icon, name, path, &[])
}

/// Expands the field codes of an Exec value with files to open: `%f` and
/// `%u` take the first file, `%F` and `%U` every file, as paths or `file://`
/// URIs; the rest expand as `exec_argv` does.  When the Exec line has no file
/// code the files are appended.
///
/// # Arguments
/// * `exec` - The Exec value.
/// * `icon` - The entry's icon, for `%i`.
/// * `name` - The entry's name, for `%c`.
/// * `path` - The desktop file, for `%k`.
/// * `files` - The files to open.
pub fn exec_argv_files(
    exec: &str,
    icon: Option<&str>,
    name: &str,
    path: &Path,
    files: &[PathBuf],
) -> Vec<String> {
    let mut argv = Vec::new();
    let mut placed = files.is_empty();
    for arg in split_exec(exec) {
        if arg == "%i" {
            if let Some(icon) = icon {
                argv.push("--icon".to_string());
                argv.push(icon.to_string());
            }
            continue;
        }
        if matches!(arg.as_str(), "%f" | "%F" | "%u" | "%U") {
            if files.is_empty() {
                continue;
            }
            placed = true;
            let uri = arg == "%u" || arg == "%U";
            let count = if arg == "%f" || arg == "%u" {
                1
            } else {
                files.len()
            };
            for file in files.iter().take(count) {
                argv.push(if uri {
                    crate::files::file_uri(file)
                } else {
                    file.to_string_lossy().to_string()
                });
            }
            continue;
        }
        let mut expanded = String::new();
        let mut chars = arg.chars();
        while let Some(c) = chars.next() {
            if c != '%' {
                expanded.push(c);
                continue;
            }
            match chars.next() {
                Some('%') => expanded.push('%'),
                Some('c') => expanded.push_str(name),
                Some('k') => expanded.push_str(&path.to_string_lossy()),
                Some(_) | None => {}
            }
        }
        let was_only_code = arg.len() == 2 && arg.starts_with('%') && expanded.is_empty();
        if was_only_code {
            continue;
        }
        if !expanded.is_empty() || !arg.contains('%') {
            argv.push(expanded);
        }
    }
    if !placed && !argv.is_empty() {
        argv.extend(files.iter().map(|f| f.to_string_lossy().to_string()));
    }
    argv
}

/// Reads one desktop entry by its ID from the applications directories,
/// whether or not it is shown in menus.
///
/// # Arguments
/// * `id` - The desktop file ID.
pub fn load_entry(id: &str) -> Option<Entry> {
    let locales = locale_suffixes();
    applications_dirs()
        .into_iter()
        .map(|dir| dir.join(id))
        .find(|path| path.is_file())
        .and_then(|path| {
            let group = read_entry_group(&path)?;
            build_entry(&path, &group, &locales, true, false)
        })
}

/// Launches an entry on files, the way `launch` does.
///
/// # Arguments
/// * `entry` - The entry to launch.
/// * `config` - The settings, for the terminal command.
/// * `files` - The files to open.
pub fn launch_with(entry: &Entry, config: &Config, files: &[PathBuf]) {
    let mut argv = exec_argv_files(
        &entry.exec,
        entry.icon.as_deref(),
        &entry.name,
        &entry.path,
        files,
    );
    if argv.is_empty() {
        return;
    }
    if entry.terminal
        && let Some(mut terminal) = find_terminal(config)
    {
        terminal.append(&mut argv);
        argv = terminal;
    }
    spawn_detached(&argv, entry.working_dir.as_deref());
}

/// The desktop file ID of the built-in file manager.
pub const FILES_ID: &str = "selkies-desktop-files.desktop";

/// Writes the file manager's desktop entry into the user's applications
/// directory when it is missing, so the menu lists it and `xdg-open` hands
/// folders to it, and makes it the folder handler when none is set.
pub fn ensure_files_entry() {
    let dir = config::data_home().join("applications");
    let path = dir.join(FILES_ID);
    if !path.is_file() && std::fs::create_dir_all(&dir).is_ok() {
        let mut text = String::from("[Desktop Entry]\nType=Application\nName=Files\n");
        for (code, name) in crate::i18n::all("Files") {
            text.push_str(&format!("Name[{code}]={name}\n"));
        }
        text.push_str("Comment=Browse and manage your files\n");
        for (code, comment) in crate::i18n::all("Browse and manage your files") {
            text.push_str(&format!("Comment[{code}]={comment}\n"));
        }
        text.push_str(
            "Exec=selkies-desktop --files %f\nIcon=system-file-manager\nTerminal=false\n\
             Categories=System;FileManager;\nMimeType=inode/directory;\nStartupNotify=false\n",
        );
        let _ = std::fs::write(&path, text);
    }
    let mimeapps = crate::files::mime::user_mimeapps();
    let text = std::fs::read_to_string(&mimeapps).unwrap_or_default();
    if !default_set(&text, "inode/directory")
        && let Some(parent) = mimeapps.parent()
        && std::fs::create_dir_all(parent).is_ok()
    {
        let _ = std::fs::write(&mimeapps, add_default(&text, "inode/directory", FILES_ID));
    }
}

/// Whether a `mimeapps.list` names a default application for a type.
fn default_set(text: &str, mime: &str) -> bool {
    let mut in_defaults = false;
    for line in text.lines() {
        let line = line.trim();
        if line.starts_with('[') {
            in_defaults = line == "[Default Applications]";
            continue;
        }
        if in_defaults
            && let Some((key, value)) = line.split_once('=')
            && key.trim() == mime
            && !value.trim().trim_end_matches(';').is_empty()
        {
            return true;
        }
    }
    false
}

/// A `mimeapps.list` with a default added for a type, the group created when
/// the file lacks it.
fn add_default(text: &str, mime: &str, id: &str) -> String {
    let line = format!("{mime}={id};");
    let mut out = String::new();
    let mut inserted = false;
    let mut in_defaults = false;
    for existing in text.lines() {
        let trimmed = existing.trim();
        if trimmed.starts_with('[') {
            if in_defaults && !inserted {
                out.push_str(&line);
                out.push('\n');
                inserted = true;
            }
            in_defaults = trimmed == "[Default Applications]";
        }
        if in_defaults && !inserted && trimmed.split_once('=').is_some_and(|(k, _)| k.trim() == mime) {
            out.push_str(&line);
            out.push('\n');
            inserted = true;
            continue;
        }
        out.push_str(existing);
        out.push('\n');
    }
    if !inserted {
        if !in_defaults {
            if !out.is_empty() && !out.ends_with("\n\n") {
                out.push('\n');
            }
            out.push_str("[Default Applications]\n");
        }
        out.push_str(&line);
        out.push('\n');
    }
    out
}

/// Launches an entry fully detached from the panel process.  The program is
/// run directly from its expanded Exec line, in a terminal when it asks for
/// one, with its standard streams on `/dev/null`.
///
/// # Arguments
/// * `entry` - The entry to launch.
/// * `config` - The settings, for the terminal command and the recent list.
pub fn launch(entry: &Entry, config: &Config) {
    let mut argv = exec_argv(&entry.exec, entry.icon.as_deref(), &entry.name, &entry.path);
    if argv.is_empty() {
        return;
    }
    if entry.terminal
        && let Some(mut terminal) = find_terminal(config)
    {
        terminal.append(&mut argv);
        argv = terminal;
    }
    if config.track_recent {
        record_recent(&entry.id);
    }
    spawn_detached(&argv, entry.working_dir.as_deref());
}

/// Forks twice so the program is reparented to init and never becomes a
/// zombie of the panel.
///
/// # Arguments
/// * `argv` - Program and arguments.
/// * `working_dir` - Directory to start in, if any.
pub fn spawn_detached(argv: &[String], working_dir: Option<&str>) {
    let c_args: Vec<CString> = argv
        .iter()
        .filter_map(|a| CString::new(a.as_str()).ok())
        .collect();
    if c_args.is_empty() {
        return;
    }
    let mut ptrs: Vec<*const libc::c_char> = c_args.iter().map(|a| a.as_ptr()).collect();
    ptrs.push(std::ptr::null());
    let dir = working_dir.and_then(|d| CString::new(d).ok());
    let null = CString::new("/dev/null").expect("static string");
    unsafe {
        let pid = libc::fork();
        if pid != 0 {
            if pid > 0 {
                let mut status = 0;
                libc::waitpid(pid, &mut status, 0);
            }
            return;
        }
        libc::setsid();
        if libc::fork() != 0 {
            libc::_exit(0);
        }
        let fd = libc::open(null.as_ptr(), libc::O_RDWR);
        if fd >= 0 {
            libc::dup2(fd, 0);
            libc::dup2(fd, 1);
            libc::dup2(fd, 2);
            if fd > 2 {
                libc::close(fd);
            }
        }
        if let Some(dir) = dir {
            libc::chdir(dir.as_ptr());
        }
        libc::execvp(ptrs[0], ptrs.as_ptr());
        libc::_exit(127);
    }
}

/// The file the recent launches are kept in.
fn recent_file() -> PathBuf {
    config::state_dir().join("recent")
}

/// Desktop file IDs launched most recently, newest first.
pub fn recent() -> Vec<String> {
    std::fs::read_to_string(recent_file())
        .map(|text| {
            text.lines()
                .map(|l| l.trim().to_string())
                .filter(|l| !l.is_empty())
                .collect()
        })
        .unwrap_or_default()
}

/// Moves an ID to the front of the recent list and trims it.
fn record_recent(id: &str) {
    let mut ids = recent();
    ids.retain(|existing| existing != id);
    ids.insert(0, id.to_string());
    ids.truncate(RECENT_LIMIT);
    let dir = config::state_dir();
    if std::fs::create_dir_all(&dir).is_ok() {
        let _ = std::fs::write(recent_file(), ids.join("\n") + "\n");
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn splits_quoted_exec() {
        let argv = split_exec(r#"/bin/sh -c "$HOME/.local/bin/proot-apps run x %U" extra"#);
        assert_eq!(
            argv,
            vec!["/bin/sh", "-c", "$HOME/.local/bin/proot-apps run x %U", "extra"]
        );
        assert_eq!(
            split_exec(r#"prog "a \"quoted\" arg""#),
            vec!["prog", "a \"quoted\" arg"]
        );
        assert_eq!(split_exec("prog  \"\""), vec!["prog", ""]);
    }

    #[test]
    fn expands_field_codes() {
        let path = Path::new("/usr/share/applications/app.desktop");
        let argv = exec_argv("app %U --name=%c %i tail %%", Some("icon"), "My App", path);
        assert_eq!(argv, vec!["app", "--name=My App", "--icon", "icon", "tail", "%"]);
        assert_eq!(exec_argv("%F", None, "", path), Vec::<String>::new());
        assert_eq!(
            exec_argv("app %k", None, "", path),
            vec!["app".to_string(), path.to_string_lossy().to_string()]
        );
    }

    #[test]
    fn category_mapping_prefers_listed_order() {
        let group = Group {
            keys: vec![
                ("Type".into(), "Application".into()),
                ("Name".into(), "Thing".into()),
                ("Name[de]".into(), "Ding".into()),
                ("Exec".into(), "/bin/true".into()),
                ("Categories".into(), "Qt;Network;WebBrowser;".into()),
            ],
        };
        let entry = build_entry(
            Path::new("/tmp/thing.desktop"),
            &group,
            &["de".to_string()],
            true,
            true,
        )
        .unwrap();
        assert_eq!(entry.category, "Internet");
        assert_eq!(entry.name, "Ding");
        assert_eq!(entry.id, "thing.desktop");
        assert!(entry.matches("din"));
        assert!(entry.matches("true"));
    }

    #[test]
    fn hidden_and_nodisplay_are_skipped() {
        let group = Group {
            keys: vec![
                ("Type".into(), "Application".into()),
                ("Name".into(), "Hidden".into()),
                ("Exec".into(), "/bin/true".into()),
                ("NoDisplay".into(), "true".into()),
            ],
        };
        assert!(build_entry(Path::new("/tmp/h.desktop"), &group, &[], true, true).is_none());
        assert!(build_entry(Path::new("/tmp/h.desktop"), &group, &[], true, false).is_some());
    }

    #[test]
    fn expands_file_codes() {
        let path = Path::new("/usr/share/applications/app.desktop");
        let files = vec![PathBuf::from("/tmp/a b.txt"), PathBuf::from("/tmp/c.txt")];
        assert_eq!(
            exec_argv_files("app %F", None, "", path, &files),
            vec!["app", "/tmp/a b.txt", "/tmp/c.txt"]
        );
        assert_eq!(
            exec_argv_files("app %u", None, "", path, &files),
            vec!["app", "file:///tmp/a%20b.txt"]
        );
        assert_eq!(
            exec_argv_files("app --flag", None, "", path, &files[..1]),
            vec!["app", "--flag", "/tmp/a b.txt"]
        );
    }

    #[test]
    fn adds_mimeapps_default() {
        assert!(!default_set("", "inode/directory"));
        let text = add_default("", "inode/directory", FILES_ID);
        assert_eq!(
            text,
            "[Default Applications]\ninode/directory=selkies-desktop-files.desktop;\n"
        );
        assert!(default_set(&text, "inode/directory"));
        let text = add_default(
            "[Added Associations]\nimage/png=x.desktop;\n\n[Default Applications]\nimage/png=x.desktop;\n",
            "inode/directory",
            FILES_ID,
        );
        assert!(text.contains("image/png=x.desktop;\ninode/directory=selkies-desktop-files.desktop;\n"));
        let kept = add_default(
            "[Default Applications]\ninode/directory=\n",
            "inode/directory",
            FILES_ID,
        );
        assert!(default_set(&kept, "inode/directory"));
        assert_eq!(kept.matches("inode/directory").count(), 1);
    }

    #[test]
    fn app_id_icons_match_class_id_and_prefix() {
        let entry = |id: &str, icon: &str, class: Option<&str>| Entry {
            id: id.to_string(),
            name: id.to_string(),
            comment: None,
            generic_name: None,
            keywords: Vec::new(),
            exec: "/bin/true".to_string(),
            icon: Some(icon.to_string()),
            path: PathBuf::from("/tmp"),
            terminal: false,
            working_dir: None,
            category: MISC.to_string(),
            wm_class: class.map(str::to_string),
        };
        let entries = vec![
            entry("steam.desktop", "steam", None),
            entry("org.mozilla.firefox.desktop", "firefox", Some("Firefox")),
            entry("st.desktop", "st", None),
        ];
        assert_eq!(
            icon_for_app_id(&entries, "steamwebhelper").as_deref(),
            Some("steam")
        );
        assert_eq!(icon_for_app_id(&entries, "firefox").as_deref(), Some("firefox"));
        assert_eq!(icon_for_app_id(&entries, "steam").as_deref(), Some("steam"));
        assert_eq!(icon_for_app_id(&entries, "stalonetray"), None);
    }

    #[test]
    fn missing_absolute_program_is_disabled() {
        assert!(program_disabled("/nonexistent/program"));
        assert!(!program_disabled("/bin/sh"));
        assert!(!program_disabled("surely-not-installed-anywhere"));
    }
}
