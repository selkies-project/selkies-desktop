//! The bar's right-click menu and the settings window it opens: a column of
//! pages on the left and the page's settings on the right, in captioned
//! cards with one setting per row, its name on the left and its control on
//! the right.  The Panel page holds the bar and the start menu, the Files
//! page the file manager, and the Desktop page the wallpaper of every
//! monitor, shared or one by one, and the colors of the interface.  Every
//! change is written to the config file at once, which the desktop then
//! reads back, so the file stays the one source of the settings.  Images
//! are chosen through the file manager in its picker form, and applications
//! for the quick launch and favorites lists from a list that opens in place.
//! A page taller than the window scrolls.

use tiny_skia::{Color, PathBuilder};

use crate::config::{
    self, BackgroundStyle, COLOR_KEYS, FILES_KEYS, PANEL_KEYS, WALLPAPER_KEYS, WallpaperMode,
};
use crate::desktop::{Desktop, Hover, Popup};
use crate::event::Button;
use crate::geom::Rect;
use crate::i18n::tr;
use crate::render::{Align, Canvas, parse_color};
use crate::ui::{self, popups};
use crate::xkb::Key;

/// Settings window width.
const WIDTH: i32 = 700;
/// Settings window height, grown when a page needs more and the screen has it.
const HEIGHT: i32 = 460;
/// Inner padding of the window.
const PAD: i32 = 16;
/// Height of one line of a setting row.
const ROW: i32 = 44;
/// Height of a control.
const CONTROL: i32 = 28;
/// Horizontal inset of a card's rows.
const CARD_PAD: i32 = 12;
/// Corner radius of a card.
const CARD_RADIUS: f32 = 10.0;
/// Height of a group's caption line.
const CAPTION: i32 = 28;
/// Space between a card and the next caption.
const GROUP_GAP: i32 = 12;
/// Width of the name column of a row.
const TITLE_WIDTH: i32 = 150;
/// Width of the page column.
const NAV_WIDTH: i32 = 136;
/// Height of a page row.
const NAV_ROW: i32 = 32;
/// Side of an icon chip.
const CHIP: i32 = 30;
/// Height of an application list row.
const LIST_ROW: i32 = 30;
/// Width of the right-click menu.
const MENU_WIDTH: i32 = 220;
/// Row height of the right-click menu.
const MENU_ROW: i32 = 30;
/// Space between controls flowed in a row.
const GAP: i32 = 4;
/// Width of a toggle switch.
const SWITCH_WIDTH: i32 = 42;
/// Height of a toggle switch.
const SWITCH_HEIGHT: i32 = 22;
/// Width of a slider with its value.
const SLIDER_WIDTH: i32 = 230;
/// Width of the value shown beside a slider.
const SLIDER_VALUE_WIDTH: i32 = 46;
/// Diameter of a slider's knob.
const KNOB: i32 = 16;
/// Width of a text field.
const FIELD_WIDTH: i32 = 200;
/// The least width a text field shrinks to for the buttons beside it.
const FIELD_MIN_WIDTH: i32 = 120;
/// Width of a color's hex field.
const COLOR_FIELD_WIDTH: i32 = 84;
/// Side of a color swatch.
const SWATCH: i32 = 22;

/// The sliders of the panel page: key, lowest, highest, step and unit.
const SLIDERS: [(&str, i32, i32, i32, &str); 3] = [
    (
        "panel_height",
        config::MIN_PANEL_HEIGHT,
        config::MAX_PANEL_HEIGHT,
        2,
        "px",
    ),
    ("panel_width", 20, 100, 5, "%"),
    ("panel_opacity", 0, 100, 5, "%"),
];

/// Preset background colors, true black first.
const SWATCHES: [&str; 8] = [
    "#000000", "#101418", "#1d222b", "#2b2b2b", "#1e2a3a", "#243b2f", "#3b2d4a", "#5a5a5a",
];
/// Preset accent colors, the default first.
const ACCENTS: [&str; 8] = [
    "#7967c5", "#9d8cf0", "#3d7dd6", "#2f9e6e", "#d5499a", "#e0803a", "#d64545", "#8a8f99",
];
/// Preset surface colors for the bar and the menus, the defaults first.
const SURFACES: [&str; 8] = [
    "#0a0e17", "#121826", "#000000", "#1d222b", "#2b2b2b", "#1e2a3a", "#3b2d4a", "#f2f3f5",
];
/// Preset text colors, the default first.
const TEXTS: [&str; 8] = [
    "#eef2f8", "#ffffff", "#d0d4da", "#b6c0d2", "#f5e6c8", "#c8e6ff", "#333333", "#111111",
];

/// The pages of the window.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Page {
    /// The bar and the start menu.
    Panel,
    /// The file manager.
    Files,
    /// The wallpaper and the colors.
    Desktop,
}

impl Page {
    /// Every page in order.
    const ALL: [Page; 3] = [Page::Panel, Page::Files, Page::Desktop];

    /// The page's name.
    fn name(self) -> &'static str {
        match self {
            Page::Panel => tr("Panel"),
            Page::Files => tr("Files"),
            Page::Desktop => tr("Desktop"),
        }
    }
}

/// The colors of the interface the user can set.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ThemeColor {
    /// Markers, selections and switches.
    Accent,
    /// The bar.
    Panel,
    /// Menus, popups and windows.
    Popup,
    /// Text.
    Text,
}

impl ThemeColor {
    /// Every color in the order shown.
    const ALL: [ThemeColor; 4] = [
        ThemeColor::Accent,
        ThemeColor::Panel,
        ThemeColor::Popup,
        ThemeColor::Text,
    ];

    /// The config key.
    fn key(self) -> &'static str {
        match self {
            ThemeColor::Accent => "color_accent",
            ThemeColor::Panel => "color_panel",
            ThemeColor::Popup => "color_popup",
            ThemeColor::Text => "color_text",
        }
    }

    /// The row's name.
    fn name(self) -> &'static str {
        match self {
            ThemeColor::Accent => tr("Accent"),
            ThemeColor::Panel => tr("Bar"),
            ThemeColor::Popup => tr("Menus"),
            ThemeColor::Text => tr("Text"),
        }
    }

    /// The preset swatches offered.
    fn presets(self) -> &'static [&'static str] {
        match self {
            ThemeColor::Accent => &ACCENTS,
            ThemeColor::Panel | ThemeColor::Popup => &SURFACES,
            ThemeColor::Text => &TEXTS,
        }
    }

    /// The color in use.
    fn current(self, theme: ui::Theme) -> Color {
        match self {
            ThemeColor::Accent => theme.accent,
            ThemeColor::Panel => theme.panel,
            ThemeColor::Popup => theme.popup,
            ThemeColor::Text => theme.text,
        }
    }
}

/// A text field of the settings window.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Field {
    /// The clock's strftime pattern.
    ClockFormat,
    /// The start button's icon name or image path.
    StartIcon,
    /// The wallpaper image path.
    Image,
    /// The background color.
    Color,
    /// One of the interface colors.
    Theme(ThemeColor),
}

/// What clicking a control does.
#[derive(Clone, Debug, PartialEq)]
enum Action {
    /// Write keys and values; an empty value removes the key.
    Set(Vec<(&'static str, String)>),
    /// Focus a text field.
    Focus(Field),
    /// Show the wallpaper settings of a monitor, or the shared ones.
    Monitor(Option<u32>),
    /// Open the file manager to choose a wallpaper image.
    Browse,
    /// Open the file manager to choose a start button icon.
    BrowseIcon,
    /// Open the settings window, from the bar's menu.
    OpenSettings,
    /// Read the applications again, from the bar's menu.
    Reload,
    /// Close the window.
    Close,
    /// Show a page.
    Page(Page),
    /// Open the application list for a list setting.
    Pick(&'static str),
    /// Return from the application list.
    Back,
    /// Add an application to a list setting, or remove it.
    Toggle(&'static str, String),
    /// Remove the page's settings from the file.
    Reset,
    /// Drag a slider for a key.
    Slide(&'static str),
}

/// How a control is drawn.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Kind {
    /// One of a row of choices.
    Segment,
    /// A whole row with a toggle switch at its right.
    Switch,
    /// A text field.
    Text,
    /// A flat button.
    Button,
    /// A color square.
    Swatch,
    /// The cross that closes the window.
    Close,
    /// A page in the left column.
    Nav,
    /// An application icon; a click removes it.
    Chip,
    /// A row of the application list.
    AppRow,
    /// A slider; the label is the value shown.
    Slider,
}

/// One clickable thing in the window.
#[derive(Clone, Debug)]
struct Control {
    rect: Rect,
    action: Action,
    label: String,
    active: bool,
    kind: Kind,
    color: Option<Color>,
    /// The application whose icon the control shows.
    app: Option<usize>,
    /// The area the control is confined to when it scrolls.
    clip: Option<Rect>,
}

/// A piece of text in the page: a group caption or a row's name.
struct Text {
    rect: Rect,
    text: String,
    caption: bool,
}

/// What the page area shows.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum View {
    /// A page of settings.
    Page(Page),
    /// The application list for a list setting.
    List(&'static str),
}

/// Everything laid out in the window.
#[derive(Default)]
struct Layout {
    controls: Vec<Control>,
    texts: Vec<Text>,
    /// Card backgrounds.
    cards: Vec<Rect>,
    /// Hairlines between rows.
    lines: Vec<Rect>,
    /// Height of the page's content before scrolling.
    content: i32,
}

/// The window's state between events.
pub struct SettingsState {
    /// The page shown.
    pub page: Page,
    /// The list setting whose application list is open, if any.
    pub picking: Option<&'static str>,
    /// The monitor whose wallpaper is shown, or `None` for the shared one.
    pub monitor: Option<u32>,
    /// The text field being typed into.
    pub focus: Option<Field>,
    /// Text of the focused field as typed so far.
    pub editing: String,
    /// Scroll offset of the page or the application list.
    pub scroll: f64,
    /// The slider being dragged, with its value so far.
    pub drag: Option<(&'static str, i32)>,
}

impl Default for SettingsState {
    fn default() -> SettingsState {
        SettingsState {
            page: Page::Panel,
            picking: None,
            monitor: None,
            focus: None,
            editing: String::new(),
            scroll: 0.0,
            drag: None,
        }
    }
}

impl SettingsState {
    /// Resets the window for opening.
    pub fn open(&mut self) {
        self.picking = None;
        self.monitor = None;
        self.focus = None;
        self.editing.clear();
        self.scroll = 0.0;
        self.drag = None;
    }

    /// Clears the state when the window closes.
    pub fn close(&mut self) {
        self.focus = None;
        self.picking = None;
        self.editing.clear();
        self.drag = None;
    }
}

/// The items of the bar's right-click menu.
fn panel_menu_items(d: &Desktop) -> Vec<(&'static str, Action)> {
    let position = if d.panel_top() { "bottom" } else { "top" };
    vec![
        (tr("Settings…"), Action::OpenSettings),
        (
            if d.panel_top() {
                tr("Move panel to bottom")
            } else {
                tr("Move panel to top")
            },
            Action::Set(vec![("panel_position", position.to_string())]),
        ),
        (tr("Reload applications"), Action::Reload),
    ]
}

/// The rectangle of the right-click menu anchored at a bar X.
fn panel_menu_rect(d: &Desktop, x: i32) -> Rect {
    let items = panel_menu_items(d).len() as i32;
    let anchor = Rect::new(x - 8, d.bar_rect().y, 16, d.bar_rect().h);
    popups::anchor_above(d, anchor, MENU_WIDTH, items * MENU_ROW + 12)
}

/// Draws the right-click menu; returns its rectangle.
///
/// # Arguments
/// * `d` - The desktop.
/// * `canvas` - The panel canvas.
/// * `x` - The bar X the menu was opened at.
pub fn draw_panel_menu(d: &mut Desktop, canvas: &mut Canvas<'_>, x: i32) -> Rect {
    let frame = panel_menu_rect(d, x);
    ui::draw_popup_frame(canvas, frame);
    for (row, (label, _)) in panel_menu_items(d).into_iter().enumerate() {
        let rect = Rect::new(
            frame.x + 6,
            frame.y + 6 + row as i32 * MENU_ROW,
            frame.w - 12,
            MENU_ROW,
        );
        if d.hover == Hover::PanelMenuItem(row) {
            canvas.fill_rounded(rect, 5.0, ui::hover_bg());
        }
        let label_rect = Rect::new(rect.x + 10, rect.y, rect.w - 20, rect.h);
        ui::label(
            canvas,
            &mut d.fonts,
            label,
            label_rect,
            ui::FONT_SIZE,
            false,
            Align::Left,
            ui::text(),
        );
    }
    frame
}

/// The right-click menu item under a point, or `None` outside the menu.
///
/// # Arguments
/// * `d` - The desktop.
/// * `x` - Pointer X.
/// * `y` - Pointer Y.
pub fn panel_menu_hit(d: &Desktop, x: f64, y: f64) -> Option<Hover> {
    let Popup::PanelMenu(at) = d.popup else {
        return None;
    };
    let frame = panel_menu_rect(d, at);
    if !frame.contains(x, y) {
        return None;
    }
    let row = ((y - frame.y as f64 - 6.0) / MENU_ROW as f64).floor();
    if row >= 0.0 && (row as usize) < panel_menu_items(d).len() {
        return Some(Hover::PanelMenuItem(row as usize));
    }
    Some(Hover::None)
}

/// Handles a click; returns whether it landed in the right-click menu.
///
/// # Arguments
/// * `d` - The desktop.
/// * `x` - Pointer X.
/// * `y` - Pointer Y.
/// * `button` - The button.
pub fn panel_menu_click(d: &mut Desktop, x: f64, y: f64, button: Button) -> bool {
    let Some(hover) = panel_menu_hit(d, x, y) else {
        return false;
    };
    if button != Button::Left {
        return true;
    }
    if let Hover::PanelMenuItem(row) = hover
        && let Some((_, action)) = panel_menu_items(d).get(row).cloned()
    {
        match action {
            Action::OpenSettings => {
                d.close_popup();
                d.open_popup(Popup::Settings);
            }
            Action::Set(changes) => {
                d.close_popup();
                d.write_config(&changes);
            }
            _ => {
                d.close_popup();
                d.reload();
            }
        }
    }
    true
}

/// The config key of a wallpaper setting for one monitor.  The handful of
/// distinct names are leaked once each so they can be static like the rest.
fn monitor_key(base: &'static str, monitor: u32) -> &'static str {
    use std::sync::{Mutex, OnceLock};
    type Known = Vec<(&'static str, u32, &'static str)>;
    static KEYS: OnceLock<Mutex<Known>> = OnceLock::new();
    let keys = KEYS.get_or_init(|| Mutex::new(Vec::new()));
    let mut keys = keys.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
    if let Some((_, _, key)) = keys.iter().find(|(b, m, _)| *b == base && *m == monitor) {
        return key;
    }
    let key: &'static str = Box::leak(format!("{base}_{monitor}").into_boxed_str());
    keys.push((base, monitor, key));
    key
}

/// The config key a wallpaper setting is written to for the monitor shown.
fn wallpaper_key(d: &Desktop, base: &'static str) -> &'static str {
    match d.settings.monitor {
        None => base,
        Some(n) => monitor_key(base, n),
    }
}

/// The settings window rectangle for a height.
fn frame_for(d: &Desktop, height: i32) -> Rect {
    let bar = d.bar_rect();
    let room = d
        .primary_background()
        .filter(|b| b.geometry.configured)
        .map_or(d.panel.height - bar.h, |b| {
            b.icon_area().h.min(d.panel.height - bar.h)
        });
    let height = height.min(room - ui::POPUP_MARGIN * 2).max(240);
    let width = WIDTH.min(d.panel.width - 2 * ui::POPUP_MARGIN);
    let x = ((d.panel.width - width) / 2).max(ui::POPUP_MARGIN);
    let y = if d.panel_top() {
        bar.bottom() + ui::POPUP_MARGIN
    } else {
        bar.y - ui::POPUP_MARGIN - height
    };
    Rect::new(x, y, width, height)
}

/// The settings window rectangle: tall enough for the tallest page when
/// the screen allows, so it keeps its size from page to page, scrolled
/// otherwise.
fn frame(d: &Desktop) -> Rect {
    let probe = frame_for(d, HEIGHT);
    let content = Page::ALL
        .iter()
        .map(|page| build(d, probe, 0.0, View::Page(*page)).content)
        .max()
        .unwrap_or(0);
    let needed = content + (probe.h - page_rect_in(probe).h);
    frame_for(d, needed.max(HEIGHT))
}

/// The page area to the right of the page column, for a frame: below the
/// title and above the footer with the Reset or Back button.
fn page_rect_in(frame: Rect) -> Rect {
    let x = frame.x + PAD + NAV_WIDTH + PAD;
    let y = frame.y + PAD + CONTROL + 10;
    let bottom = frame.bottom() - PAD - CONTROL - 10;
    Rect::new(x, y, frame.right() - PAD - x, bottom - y)
}

/// The wallpaper the page shows: the monitor's, or the shared one.
fn shown_wallpaper(d: &Desktop) -> config::Wallpaper {
    match d.settings.monitor {
        None => d.config.wallpaper.clone(),
        Some(n) => d.config.wallpaper_for(n),
    }
}

/// A color as `#rrggbb`.
fn hex(color: Color) -> String {
    let c = color.to_color_u8();
    format!("#{:02x}{:02x}{:02x}", c.red(), c.green(), c.blue())
}

/// The text a field shows: what is being typed when it has focus, the
/// setting otherwise.
fn field_text(d: &Desktop, field: Field) -> String {
    if d.settings.focus == Some(field) {
        return d.settings.editing.clone();
    }
    match field {
        Field::ClockFormat => d.config.clock_format.clone(),
        Field::StartIcon => d.config.start_icon.clone().unwrap_or_default(),
        Field::Image => shown_wallpaper(d)
            .image
            .as_ref()
            .map(|p| p.to_string_lossy().to_string())
            .unwrap_or_default(),
        Field::Color => hex(shown_wallpaper(d).color),
        Field::Theme(color) => hex(color.current(d.config.theme)),
    }
}

/// Width of a segment or button for its label; wide scripts count double.
fn chip_width(label: &str) -> i32 {
    let width: i32 = label
        .chars()
        .map(|c| {
            if c.is_ascii() {
                8
            } else if c as u32 >= 0x2E80 {
                14
            } else {
                9
            }
        })
        .sum();
    14 + width + 14
}

/// The keys a page's reset removes.
fn page_keys(d: &Desktop) -> Vec<&'static str> {
    match d.settings.page {
        Page::Panel => PANEL_KEYS.to_vec(),
        Page::Files => FILES_KEYS.to_vec(),
        Page::Desktop => WALLPAPER_KEYS
            .iter()
            .map(|k| wallpaper_key(d, k))
            .chain(COLOR_KEYS)
            .collect(),
    }
}

/// Places items of given widths in an area: on one line flush right when
/// they fit, else left to right over as many lines as needed.
///
/// # Arguments
/// * `x` - Left edge of the area.
/// * `width` - Width of the area.
/// * `widths` - The items' widths.
///
/// # Returns
/// Each item's X and line, and the number of lines.
fn flow(x: i32, width: i32, widths: &[i32]) -> (Vec<(i32, i32)>, i32) {
    let total: i32 = widths.iter().sum::<i32>() + GAP * (widths.len() as i32 - 1).max(0);
    if total <= width {
        let mut cursor = x + width - total;
        let placed = widths
            .iter()
            .map(|w| {
                let at = cursor;
                cursor += w + GAP;
                (at, 0)
            })
            .collect();
        return (placed, 1);
    }
    let mut placed = Vec::new();
    let mut cursor = x;
    let mut line = 0;
    for w in widths {
        if cursor + w > x + width && cursor > x {
            cursor = x;
            line += 1;
        }
        placed.push((cursor, line));
        cursor += w + GAP;
    }
    (placed, line + 1)
}

/// The rectangle of an item placed by `flow`, centered in its line.
fn placed(area: Rect, at: (i32, i32), w: i32, h: i32) -> Rect {
    Rect::new(at.0, area.y + at.1 * ROW + (ROW - h) / 2, w, h)
}

/// Builds a page: captioned cards of rows, each row a name and a control.
struct Groups<'a> {
    out: &'a mut Layout,
    clip: Rect,
    x: i32,
    width: i32,
    /// Where the next caption or row goes, scrolled.
    y: i32,
    /// Top of the open card, if one is open.
    card_top: Option<i32>,
    /// Rows in the open card.
    rows: usize,
    /// Whether anything has been added yet.
    started: bool,
}

impl Groups<'_> {
    /// Width of the control area of a row.
    fn area_width(&self) -> i32 {
        self.width - 2 * CARD_PAD - TITLE_WIDTH
    }

    /// Starts a captioned card.
    fn group(&mut self, caption: &str) {
        self.finish();
        if self.started {
            self.y += GROUP_GAP;
        }
        self.started = true;
        self.out.texts.push(Text {
            rect: Rect::new(self.x + CARD_PAD, self.y, self.width - 2 * CARD_PAD, CAPTION),
            text: caption.to_string(),
            caption: true,
        });
        self.y += CAPTION;
        self.card_top = Some(self.y);
        self.rows = 0;
    }

    /// Closes the open card.
    fn finish(&mut self) {
        if let Some(top) = self.card_top.take() {
            self.out
                .cards
                .push(Rect::new(self.x, top, self.width, self.y - top));
        }
    }

    /// Starts a row with a name; returns the area its control goes in.
    ///
    /// # Arguments
    /// * `title` - The row's name.
    /// * `lines` - Lines of `ROW` height the control takes.
    fn row(&mut self, title: &str, lines: i32) -> Rect {
        if self.card_top.is_none() {
            self.card_top = Some(self.y);
            self.rows = 0;
        }
        let inner_x = self.x + CARD_PAD;
        let inner_w = self.width - 2 * CARD_PAD;
        if self.rows > 0 {
            self.out.lines.push(Rect::new(inner_x, self.y, inner_w, 1));
        }
        self.rows += 1;
        let height = lines.max(1) * ROW;
        if !title.is_empty() {
            self.out.texts.push(Text {
                rect: Rect::new(inner_x, self.y, TITLE_WIDTH - 8, ROW),
                text: title.to_string(),
                caption: false,
            });
        }
        let area = Rect::new(inner_x + TITLE_WIDTH, self.y, inner_w - TITLE_WIDTH, height);
        self.y += height;
        area
    }

    /// Adds a control; returns it for its icon or color.
    fn push(&mut self, kind: Kind, rect: Rect, action: Action, label: &str, active: bool) -> &mut Control {
        self.out.controls.push(Control {
            rect,
            action,
            label: label.to_string(),
            active,
            kind,
            color: None,
            app: None,
            clip: Some(self.clip),
        });
        let last = self.out.controls.len() - 1;
        &mut self.out.controls[last]
    }

    /// Adds a row whose whole width toggles a boolean key.
    fn switch(&mut self, title: &str, key: &'static str, on: bool) {
        let area = self.row(title, 1);
        let rect = Rect::new(area.x - TITLE_WIDTH, area.y, area.w + TITLE_WIDTH, ROW);
        self.push(
            Kind::Switch,
            rect,
            Action::Set(vec![(key, (!on).to_string())]),
            title,
            on,
        );
    }

    /// Adds a row of choices.
    fn segments(&mut self, title: &str, options: &[(String, Action)], active: &str) {
        let widths: Vec<i32> = options.iter().map(|(l, _)| chip_width(l)).collect();
        let (_, lines) = flow(0, self.area_width(), &widths);
        let area = self.row(title, lines);
        let (at, _) = flow(area.x, area.w, &widths);
        for (((label, action), w), at) in options.iter().zip(widths).zip(at) {
            let rect = placed(area, at, w, CONTROL);
            self.push(Kind::Segment, rect, action.clone(), label, label == active);
        }
    }

    /// Adds a slider row for a numeric key.
    fn slider(&mut self, title: &str, key: &'static str, value: i32, unit: &str) {
        let area = self.row(title, 1);
        let w = SLIDER_WIDTH.min(area.w);
        let rect = Rect::new(area.right() - w, area.y + (ROW - CONTROL) / 2, w, CONTROL);
        self.push(
            Kind::Slider,
            rect,
            Action::Slide(key),
            &format!("{value}{unit}"),
            false,
        );
    }

    /// Adds a text field with flat buttons after it.  The field gives up
    /// width for the buttons down to a minimum; past that it takes the
    /// whole line and the buttons the next.
    fn field(&mut self, title: &str, field: Field, value: String, focused: bool, buttons: &[(&str, Action)]) {
        let area_w = self.area_width();
        let buttons_w: i32 = buttons.iter().map(|(l, _)| chip_width(l) + GAP).sum();
        let mut widths: Vec<i32> = vec![
            FIELD_WIDTH
                .min(area_w - buttons_w)
                .max(FIELD_MIN_WIDTH.min(area_w)),
        ];
        widths.extend(buttons.iter().map(|(l, _)| chip_width(l)));
        let together = flow(0, area_w, &widths).1 == 1;
        let area = self.row(title, if together { 1 } else { 2 });
        let (at, field_w) = if together {
            (flow(area.x, area.w, &widths).0, widths[0])
        } else {
            let mut at = vec![(area.x, 0)];
            at.extend(
                flow(area.x, area.w, &widths[1..])
                    .0
                    .into_iter()
                    .map(|(x, _)| (x, 1)),
            );
            (at, area.w)
        };
        let rect = placed(area, at[0], field_w, CONTROL);
        self.push(Kind::Text, rect, Action::Focus(field), &value, focused);
        for (((label, action), w), at) in buttons.iter().zip(&widths[1..]).zip(&at[1..]) {
            let rect = placed(area, *at, *w, CONTROL);
            self.push(Kind::Button, rect, action.clone(), label, false);
        }
    }

    /// Adds a row of preset swatches and a hex field for a color key.
    fn color(
        &mut self,
        title: &str,
        field: Field,
        key: &'static str,
        current: String,
        focused: bool,
        presets: &[&str],
    ) {
        let mut widths: Vec<i32> = presets.iter().map(|_| SWATCH).collect();
        widths.push(COLOR_FIELD_WIDTH);
        let (_, lines) = flow(0, self.area_width(), &widths);
        let area = self.row(title, lines);
        let (at, _) = flow(area.x, area.w, &widths);
        for (swatch, at) in presets.iter().zip(&at) {
            let rect = placed(area, *at, SWATCH, SWATCH);
            let active = !focused && current.eq_ignore_ascii_case(swatch);
            let color = parse_color(swatch);
            self.push(
                Kind::Swatch,
                rect,
                Action::Set(vec![(key, swatch.to_string())]),
                "",
                active,
            )
            .color = color;
        }
        let rect = placed(area, at[presets.len()], COLOR_FIELD_WIDTH, CONTROL);
        self.push(Kind::Text, rect, Action::Focus(field), &current, focused);
    }

    /// Adds a row of application chips for a list setting, with a button
    /// that opens the application list.
    fn chips(&mut self, title: &str, key: &'static str, ids: &[String], d: &Desktop) {
        let members: Vec<usize> = ids.iter().filter_map(|id| d.apps.find(id)).collect();
        let mut widths: Vec<i32> = members.iter().map(|_| CHIP).collect();
        widths.push(CONTROL);
        let (_, lines) = flow(0, self.area_width(), &widths);
        let area = self.row(title, lines);
        let (at, _) = flow(area.x, area.w, &widths);
        for (index, at) in members.iter().zip(&at) {
            let entry = &d.apps.entries[*index];
            let rect = placed(area, *at, CHIP, CHIP);
            self.push(
                Kind::Chip,
                rect,
                Action::Toggle(key, entry.id.clone()),
                &entry.name,
                false,
            )
            .app = Some(*index);
        }
        let rect = placed(area, at[members.len()], CONTROL, CONTROL);
        self.push(Kind::Button, rect, Action::Pick(key), "+", false);
    }

    /// Adds a row with one flat button at its right.
    fn button(&mut self, title: &str, label: &str, action: Action) {
        let area = self.row(title, 1);
        let w = chip_width(label).min(area.w);
        let rect = Rect::new(area.right() - w, area.y + (ROW - CONTROL) / 2, w, CONTROL);
        self.push(Kind::Button, rect, action, label, false);
    }
}

/// What the page area shows at the moment.
fn view(d: &Desktop) -> View {
    d.settings.picking.map_or(View::Page(d.settings.page), View::List)
}

/// Lays out the window as it is.
fn layout(d: &Desktop) -> Layout {
    build(d, frame(d), d.settings.scroll, view(d))
}

/// Lays the window out inside a frame.
///
/// # Arguments
/// * `d` - The desktop.
/// * `frame` - The window rectangle.
/// * `scroll` - How far the page is scrolled.
/// * `view` - The page or the application list laid out.
fn build(d: &Desktop, frame: Rect, scroll: f64, view: View) -> Layout {
    let page = page_rect_in(frame);
    let mut out = Layout::default();
    let fixed = |rect: Rect, action: Action, label: &str, active: bool, kind: Kind| Control {
        rect,
        action,
        label: label.to_string(),
        active,
        kind,
        color: None,
        app: None,
        clip: None,
    };
    out.controls.push(fixed(
        Rect::new(frame.right() - PAD - CONTROL, frame.y + PAD, CONTROL, CONTROL),
        Action::Close,
        "",
        false,
        Kind::Close,
    ));
    for (i, item) in Page::ALL.iter().enumerate() {
        out.controls.push(fixed(
            Rect::new(frame.x + PAD, page.y + i as i32 * NAV_ROW, NAV_WIDTH, NAV_ROW),
            Action::Page(*item),
            item.name(),
            d.settings.page == *item,
            Kind::Nav,
        ));
    }
    let footer = |label: &str| {
        let w = chip_width(label);
        Rect::new(
            frame.right() - PAD - w,
            frame.bottom() - PAD - CONTROL,
            w,
            CONTROL,
        )
    };

    if let View::List(key) = view {
        out.controls.push(fixed(
            footer(tr("Back")),
            Action::Back,
            tr("Back"),
            false,
            Kind::Button,
        ));
        let (title, members) = if key == "favorites" {
            (tr("Favorites"), &d.config.favorites)
        } else {
            (tr("Quick launch"), &d.config.launchers)
        };
        out.texts.push(Text {
            rect: Rect::new(page.x + CARD_PAD, page.y, page.w - 2 * CARD_PAD, CAPTION),
            text: title.to_string(),
            caption: true,
        });
        let list = Rect::new(page.x, page.y + CAPTION, page.w, page.h - CAPTION);
        out.cards.push(list);
        for (row, index) in d.apps.all_sorted().into_iter().enumerate() {
            let y = list.y + row as i32 * LIST_ROW - scroll as i32;
            if y + LIST_ROW < list.y || y > list.bottom() {
                continue;
            }
            let entry = &d.apps.entries[index];
            out.controls.push(Control {
                rect: Rect::new(list.x, y, list.w, LIST_ROW),
                action: Action::Toggle(key, entry.id.clone()),
                label: entry.name.clone(),
                active: members.contains(&entry.id),
                kind: Kind::AppRow,
                color: None,
                app: Some(index),
                clip: Some(list),
            });
        }
        out.content = d.apps.entries.len() as i32 * LIST_ROW + CAPTION;
        return out;
    }

    out.controls.push(fixed(
        footer(tr("Reset")),
        Action::Reset,
        tr("Reset"),
        false,
        Kind::Button,
    ));
    let mut groups = Groups {
        out: &mut out,
        clip: page,
        x: page.x,
        width: page.w,
        y: page.y - scroll as i32,
        card_top: None,
        rows: 0,
        started: false,
    };
    let set = |key: &'static str, value: &str| Action::Set(vec![(key, value.to_string())]);
    let focus = d.settings.focus;
    let View::Page(shown) = view else {
        return out;
    };
    match shown {
        Page::Panel => {
            groups.group(tr("Bar"));
            groups.segments(
                tr("Edge"),
                &[
                    (tr("Bottom").to_string(), set("panel_position", "bottom")),
                    (tr("Top").to_string(), set("panel_position", "top")),
                ],
                if d.panel_top() { tr("Top") } else { tr("Bottom") },
            );
            groups.switch(tr("Auto-hide"), "panel_autohide", d.config.panel_autohide);
            for (label, (key, _, _, _, unit)) in [tr("Height"), tr("Width"), tr("Opacity")]
                .iter()
                .zip(SLIDERS.iter())
            {
                groups.slider(label, key, slider_value(d, key), unit);
            }
            groups.field(
                tr("Icon"),
                Field::StartIcon,
                field_text(d, Field::StartIcon),
                focus == Some(Field::StartIcon),
                &[
                    (tr("Browse…"), Action::BrowseIcon),
                    (tr("Default"), set("start_icon", "")),
                ],
            );
            groups.group(tr("Items"));
            groups.switch(tr("Clock"), "show_clock", d.config.show_clock);
            groups.field(
                tr("Format"),
                Field::ClockFormat,
                field_text(d, Field::ClockFormat),
                focus == Some(Field::ClockFormat),
                &[],
            );
            groups.switch(tr("Volume"), "show_volume", d.config.show_volume);
            groups.switch(tr("Tray"), "show_tray", d.config.show_tray);
            groups.switch(tr("Desktop"), "show_desktop_button", d.config.show_desktop_button);
            groups.group(tr("Quick launch"));
            let launchers = d.config.launchers.clone();
            groups.chips(tr("Applications"), "launchers", &launchers, d);
            groups.group(tr("Start menu"));
            let favorites = d.config.favorites.clone();
            groups.chips(tr("Favorites"), "favorites", &favorites, d);
            groups.switch(tr("Recent"), "track_recent", d.config.track_recent);
        }
        Page::Files => {
            groups.group(tr("Files"));
            groups.segments(
                tr("View"),
                &[
                    (tr("Icons").to_string(), set("files_view", "icons")),
                    (tr("List").to_string(), set("files_view", "list")),
                ],
                if d.config.files_list {
                    tr("List")
                } else {
                    tr("Icons")
                },
            );
            let sorts: Vec<(String, Action)> = [
                (tr("Name"), "name"),
                (tr("Size"), "size"),
                (tr("Modified"), "modified"),
                (tr("Type"), "type"),
            ]
            .iter()
            .map(|(label, value)| (label.to_string(), set("files_sort", value)))
            .collect();
            let current = match d.config.files_sort.as_str() {
                "size" => tr("Size"),
                "modified" => tr("Modified"),
                "type" => tr("Type"),
                _ => tr("Name"),
            };
            groups.segments(tr("Sort"), &sorts, current);
            groups.switch(tr("Hidden files"), "files_hidden", d.config.files_hidden);
        }
        Page::Desktop => {
            groups.group(tr("Wallpaper"));
            let monitors = d.monitor_list();
            if monitors.len() > 1 {
                let mut options: Vec<(String, Action)> = vec![(tr("All").to_string(), Action::Monitor(None))];
                for n in 1..=monitors.len() as u32 {
                    options.push((n.to_string(), Action::Monitor(Some(n))));
                }
                let active = d
                    .settings
                    .monitor
                    .map_or(tr("All").to_string(), |n| n.to_string());
                groups.segments(tr("Monitor"), &options, &active);
            }
            let wallpaper = shown_wallpaper(d);
            let style_key = wallpaper_key(d, "background_style");
            groups.segments(
                tr("Background"),
                &[
                    (tr("Solid").to_string(), set(style_key, "solid")),
                    (tr("Gradient").to_string(), set(style_key, "gradient")),
                ],
                match wallpaper.style {
                    BackgroundStyle::Solid => tr("Solid"),
                    BackgroundStyle::Gradient => tr("Gradient"),
                },
            );
            groups.color(
                tr("Color"),
                Field::Color,
                wallpaper_key(d, "background"),
                field_text(d, Field::Color),
                focus == Some(Field::Color),
                &SWATCHES,
            );
            groups.field(
                tr("Image"),
                Field::Image,
                field_text(d, Field::Image),
                focus == Some(Field::Image),
                &[
                    (tr("Browse…"), Action::Browse),
                    (tr("Clear"), set(wallpaper_key(d, "wallpaper"), "none")),
                ],
            );
            let mode_key = wallpaper_key(d, "wallpaper_mode");
            groups.segments(
                tr("Fit"),
                &[
                    (tr("Fill").to_string(), set(mode_key, "fill")),
                    (tr("Fit").to_string(), set(mode_key, "fit")),
                    (tr("Center").to_string(), set(mode_key, "center")),
                    (tr("Stretch").to_string(), set(mode_key, "stretch")),
                ],
                match wallpaper.mode {
                    WallpaperMode::Fill => tr("Fill"),
                    WallpaperMode::Fit => tr("Fit"),
                    WallpaperMode::Center => tr("Center"),
                    WallpaperMode::Stretch => tr("Stretch"),
                },
            );
            if let Some(n) = d.settings.monitor
                && d.config.monitors.contains_key(&n)
            {
                let clear: Vec<(&'static str, String)> = WALLPAPER_KEYS
                    .iter()
                    .map(|key| (monitor_key(key, n), String::new()))
                    .collect();
                groups.button("", tr("Use shared"), Action::Set(clear));
            }
            groups.group(tr("Colors"));
            for color in ThemeColor::ALL {
                let field = Field::Theme(color);
                groups.color(
                    color.name(),
                    field,
                    color.key(),
                    field_text(d, field),
                    focus == Some(field),
                    color.presets(),
                );
            }
        }
    }
    groups.finish();
    out.content = groups.y + scroll as i32 - page.y;
    out
}

/// The value a slider shows: the one being dragged, else the setting.
fn slider_value(d: &Desktop, key: &str) -> i32 {
    if let Some((dragging, value)) = d.settings.drag
        && dragging == key
    {
        return value;
    }
    match key {
        "panel_height" => d.config.panel_height,
        "panel_width" => d.config.panel_width,
        _ => d.config.panel_opacity,
    }
}

/// The track of a slider inside its control.
fn slider_track(rect: Rect) -> Rect {
    Rect::new(
        rect.x + KNOB / 2,
        rect.y + rect.h / 2 - 2,
        rect.w - SLIDER_VALUE_WIDTH - KNOB,
        4,
    )
}

/// The value of a slider for a pointer X over it, snapped to its step.
fn slider_at(key: &str, rect: Rect, x: f64) -> i32 {
    let Some((_, min, max, step, _)) = SLIDERS.iter().find(|(k, ..)| *k == key) else {
        return 0;
    };
    let track = slider_track(rect);
    let ratio = ((x - track.x as f64) / track.w as f64).clamp(0.0, 1.0);
    let raw = *min as f64 + ratio * (*max - *min) as f64;
    let snapped = ((raw / *step as f64).round() as i32) * step;
    snapped.clamp(*min, *max)
}

/// The control rectangle of the slider for a key, when its page is shown.
fn slider_rect(d: &Desktop, key: &'static str) -> Option<Rect> {
    layout(d)
        .controls
        .iter()
        .find(|c| c.action == Action::Slide(key))
        .map(|c| c.rect)
}

/// A check mark path inside a box.
fn check_path(rect: Rect) -> Option<tiny_skia::Path> {
    let mut pb = PathBuilder::new();
    let (x, y) = (rect.x as f32, rect.y as f32);
    pb.move_to(x + 4.0, y + 8.5);
    pb.line_to(x + 7.0, y + 11.5);
    pb.line_to(x + 12.5, y + 5.0);
    pb.finish()
}

/// A cross path centered in a box.
fn cross_path(rect: Rect, reach: f32) -> Option<tiny_skia::Path> {
    let mut pb = PathBuilder::new();
    let (cx, cy) = (
        rect.x as f32 + rect.w as f32 / 2.0,
        rect.y as f32 + rect.h as f32 / 2.0,
    );
    pb.move_to(cx - reach, cy - reach);
    pb.line_to(cx + reach, cy + reach);
    pb.move_to(cx + reach, cy - reach);
    pb.line_to(cx - reach, cy + reach);
    pb.finish()
}

/// Draws a flat button: a tonal pill with its label.
fn draw_button(d: &mut Desktop, canvas: &mut Canvas<'_>, control: &Control, hovered: bool) {
    let rect = control.rect;
    canvas.fill_rounded(rect, 7.0, ui::text_alpha(if hovered { 0x2a } else { 0x16 }));
    ui::label(
        canvas,
        &mut d.fonts,
        &control.label,
        Rect::new(rect.x + 4, rect.y, rect.w - 8, rect.h),
        ui::FONT_SIZE,
        false,
        Align::Center,
        ui::text(),
    );
}

/// Draws a text field with its caret when focused.
fn draw_field(d: &mut Desktop, canvas: &mut Canvas<'_>, control: &Control, hovered: bool) {
    let rect = control.rect;
    canvas.fill_rounded(rect, 7.0, ui::text_alpha(0x10));
    if control.active {
        canvas.stroke_rounded(rect, 7.0, 1.5, ui::accent());
    } else if hovered {
        canvas.stroke_rounded(rect, 7.0, 1.0, ui::text_dim());
    } else {
        canvas.stroke_rounded(rect, 7.0, 1.0, ui::popup_border());
    }
    let text_rect = Rect::new(rect.x + 8, rect.y, rect.w - 16, rect.h);
    let empty = control.label.is_empty();
    let shown = if empty && !control.active {
        "none".to_string()
    } else {
        control.label.clone()
    };
    let color = if empty { ui::text_dim() } else { ui::text() };
    ui::label(
        canvas,
        &mut d.fonts,
        &shown,
        text_rect,
        ui::FONT_SIZE,
        false,
        Align::Left,
        color,
    );
    if control.active {
        let caret_x = text_rect.x
            + ui::text_width(&mut d.fonts, &control.label, ui::FONT_SIZE, false, d.panel.scale)
                .min(text_rect.w - 2);
        canvas.fill_rect(
            Rect::new(caret_x + 1, text_rect.y + 7, 1, text_rect.h - 14),
            ui::text(),
        );
    }
}

/// Draws a toggle switch row: the pill at the row's right.
fn draw_switch(canvas: &mut Canvas<'_>, control: &Control, hovered: bool) {
    let rect = control.rect;
    if hovered {
        canvas.fill_rounded(rect, 6.0, ui::hover_bg());
    }
    let pill = Rect::new(
        rect.right() - SWITCH_WIDTH,
        rect.y + (rect.h - SWITCH_HEIGHT) / 2,
        SWITCH_WIDTH,
        SWITCH_HEIGHT,
    );
    let radius = SWITCH_HEIGHT as f32 / 2.0;
    let track = if control.active {
        ui::accent()
    } else {
        ui::text_alpha(0x30)
    };
    canvas.fill_rounded(pill, radius, track);
    let knob = SWITCH_HEIGHT - 6;
    let knob_x = if control.active {
        pill.right() - 3 - knob
    } else {
        pill.x + 3
    };
    canvas.fill_rounded(
        Rect::new(knob_x, pill.y + 3, knob, knob),
        knob as f32 / 2.0,
        if control.active {
            ui::on_accent()
        } else {
            ui::text()
        },
    );
}

/// Draws a slider with its value.
fn draw_slider(d: &mut Desktop, canvas: &mut Canvas<'_>, control: &Control, hovered: bool) {
    let Action::Slide(key) = control.action else {
        return;
    };
    let Some((_, min, max, _, _)) = SLIDERS.iter().find(|(k, ..)| *k == key) else {
        return;
    };
    let rect = control.rect;
    let value = slider_value(d, key);
    let track = slider_track(rect);
    let ratio = (value - min) as f64 / (max - min).max(1) as f64;
    let fill_w = (track.w as f64 * ratio) as i32;
    canvas.fill_rounded(track, 2.0, ui::text_alpha(0x30));
    canvas.fill_rounded(Rect::new(track.x, track.y, fill_w, track.h), 2.0, ui::accent());
    let dragging = d.settings.drag.is_some_and(|(k, _)| k == key);
    let size = if hovered || dragging { KNOB + 2 } else { KNOB };
    let knob = Rect::new(
        track.x + fill_w - size / 2,
        track.y + track.h / 2 - size / 2,
        size,
        size,
    );
    canvas.fill_rounded(knob, size as f32 / 2.0, ui::text());
    ui::label(
        canvas,
        &mut d.fonts,
        &control.label,
        Rect::new(
            rect.right() - SLIDER_VALUE_WIDTH,
            rect.y,
            SLIDER_VALUE_WIDTH,
            rect.h,
        ),
        ui::FONT_SMALL,
        false,
        Align::Right,
        ui::text_dim(),
    );
}

/// Draws one control.
fn draw_control(d: &mut Desktop, canvas: &mut Canvas<'_>, control: &Control, hovered: bool) {
    let rect = control.rect;
    match control.kind {
        Kind::Nav => {
            if control.active {
                canvas.fill_rounded(rect, 6.0, ui::active_bg());
            } else if hovered {
                canvas.fill_rounded(rect, 6.0, ui::hover_bg());
            }
            ui::label(
                canvas,
                &mut d.fonts,
                &control.label,
                Rect::new(rect.x + 10, rect.y, rect.w - 20, rect.h),
                ui::FONT_SIZE,
                control.active,
                Align::Left,
                ui::text(),
            );
        }
        Kind::AppRow => {
            if hovered {
                canvas.fill_rounded(rect, 5.0, ui::hover_bg());
            }
            let icon_rect = Rect::new(
                rect.x + 8,
                rect.y + (rect.h - ui::ICON_SIZE) / 2,
                ui::ICON_SIZE,
                ui::ICON_SIZE,
            );
            if let Some(icon) = control.app.and_then(|i| d.app_icon(i)) {
                canvas.draw_image_fit(&icon, icon_rect);
            }
            ui::label(
                canvas,
                &mut d.fonts,
                &control.label,
                Rect::new(icon_rect.right() + 10, rect.y, rect.w - 60, rect.h),
                ui::FONT_SIZE,
                false,
                Align::Left,
                ui::text(),
            );
            if control.active {
                let box_rect = Rect::new(rect.right() - 28, rect.y + (rect.h - 16) / 2, 16, 16);
                canvas.fill_rounded(box_rect, 4.0, ui::accent());
                if let Some(path) = check_path(box_rect) {
                    canvas.stroke_path(&path, 2.0, ui::on_accent());
                }
            }
        }
        Kind::Chip => {
            canvas.fill_rounded(rect, 6.0, ui::text_alpha(if hovered { 0x2a } else { 0x16 }));
            let icon_rect = rect.inflate(-5);
            if let Some(icon) = control.app.and_then(|i| d.app_icon_sized(i, CHIP - 10)) {
                canvas.draw_image_fit(&icon, icon_rect);
            }
            if hovered {
                canvas.fill_rounded(rect, 6.0, Color::from_rgba8(0, 0, 0, 0x90));
                if let Some(path) = cross_path(rect, 5.0) {
                    canvas.stroke_path(&path, 2.0, ui::text());
                }
            }
        }
        Kind::Text => draw_field(d, canvas, control, hovered),
        Kind::Swatch => {
            if let Some(color) = control.color {
                canvas.fill_rounded(rect, 5.0, color);
            }
            let border = if control.active {
                ui::accent()
            } else if hovered {
                ui::text()
            } else {
                ui::popup_border()
            };
            canvas.stroke_rounded(rect, 5.0, if control.active { 2.0 } else { 1.0 }, border);
        }
        Kind::Switch => draw_switch(canvas, control, hovered),
        Kind::Close => {
            if hovered {
                canvas.fill_rounded(rect, 5.0, ui::hover_bg());
            }
            if let Some(path) = cross_path(rect, 5.0) {
                canvas.stroke_path(&path, 1.8, ui::text());
            }
        }
        Kind::Slider => draw_slider(d, canvas, control, hovered),
        Kind::Segment => {
            if control.active {
                canvas.fill_rounded(rect, 7.0, ui::accent());
            } else if hovered {
                canvas.fill_rounded(rect, 7.0, ui::hover_bg());
            }
            ui::label(
                canvas,
                &mut d.fonts,
                &control.label,
                Rect::new(rect.x + 4, rect.y, rect.w - 8, rect.h),
                ui::FONT_SIZE,
                control.active,
                Align::Center,
                if control.active {
                    ui::on_accent()
                } else {
                    ui::text()
                },
            );
        }
        Kind::Button => draw_button(d, canvas, control, hovered),
    }
}

/// Draws the settings window; returns its rectangle.
///
/// # Arguments
/// * `d` - The desktop.
/// * `canvas` - The panel canvas.
pub fn draw(d: &mut Desktop, canvas: &mut Canvas<'_>) -> Rect {
    let frame = frame(d);
    let page = page_rect_in(frame);
    ui::draw_popup_frame(canvas, frame);
    ui::label(
        canvas,
        &mut d.fonts,
        tr("Settings"),
        Rect::new(frame.x + PAD, frame.y + PAD, frame.w - 2 * PAD - CONTROL, CONTROL),
        ui::FONT_SIZE + 3.0,
        true,
        Align::Left,
        ui::text(),
    );
    canvas.fill_rect(Rect::new(page.x - PAD / 2, page.y, 1, page.h), ui::separator());
    let layout = layout(d);
    for (index, control) in layout.controls.iter().enumerate() {
        if control.clip.is_none() {
            draw_control(d, canvas, control, d.hover == Hover::Control(index));
        }
    }
    canvas.clip(page);
    for card in &layout.cards {
        canvas.fill_rounded(*card, CARD_RADIUS, ui::field_bg());
    }
    for line in &layout.lines {
        canvas.fill_rect(*line, ui::separator());
    }
    for text in &layout.texts {
        ui::label(
            canvas,
            &mut d.fonts,
            &text.text,
            text.rect,
            ui::FONT_SIZE,
            text.caption,
            Align::Left,
            if text.caption { ui::text_dim() } else { ui::text() },
        );
    }
    for (index, control) in layout.controls.iter().enumerate() {
        if let Some(clip) = control.clip {
            if clip != page {
                canvas.clip(clip);
            }
            draw_control(d, canvas, control, d.hover == Hover::Control(index));
            if clip != page {
                canvas.clip(page);
            }
        }
    }
    canvas.reset_clip();
    frame
}

/// The control under a point, or `None` outside the window.
///
/// # Arguments
/// * `d` - The desktop.
/// * `x` - Pointer X.
/// * `y` - Pointer Y.
pub fn hit(d: &Desktop, x: f64, y: f64) -> Option<Hover> {
    let frame = frame(d);
    if !frame.contains(x, y) {
        return None;
    }
    for (index, control) in layout(d).controls.iter().enumerate() {
        if control.clip.is_some_and(|clip| !clip.contains(x, y)) {
            continue;
        }
        if control.rect.contains(x, y) {
            return Some(Hover::Control(index));
        }
    }
    Some(Hover::None)
}

/// Handles a click; returns whether it landed in the window.
///
/// # Arguments
/// * `d` - The desktop.
/// * `x` - Pointer X.
/// * `y` - Pointer Y.
/// * `button` - The button.
pub fn click(d: &mut Desktop, x: f64, y: f64, button: Button) -> bool {
    let Some(hover) = hit(d, x, y) else {
        return false;
    };
    if button != Button::Left {
        return true;
    }
    let Hover::Control(index) = hover else {
        commit_field(d);
        d.redraw_panel();
        return true;
    };
    let layout = layout(d);
    let Some(control) = layout.controls.get(index) else {
        return true;
    };
    let action = control.action.clone();
    if !matches!(action, Action::Focus(_)) {
        commit_field(d);
    }
    match action {
        Action::Set(changes) => d.write_config(&changes),
        Action::Focus(field) => {
            if d.settings.focus != Some(field) {
                let text = field_text(d, field);
                d.settings.focus = Some(field);
                d.settings.editing = text;
            }
        }
        Action::Monitor(monitor) => d.settings.monitor = monitor,
        Action::Browse => {
            let key = wallpaper_key(d, "wallpaper");
            let monitor = d.settings.monitor;
            d.pick_wallpaper(key, monitor);
            return true;
        }
        Action::BrowseIcon => {
            d.pick_start_icon();
            return true;
        }
        Action::Page(page) => {
            d.settings.page = page;
            d.settings.picking = None;
            d.settings.scroll = 0.0;
        }
        Action::Pick(key) => {
            d.settings.picking = Some(key);
            d.settings.scroll = 0.0;
        }
        Action::Back => {
            d.settings.picking = None;
            d.settings.scroll = 0.0;
        }
        Action::Toggle(key, id) => d.toggle_list_entry(key, &id),
        Action::Reset => {
            let changes: Vec<(&'static str, String)> =
                page_keys(d).into_iter().map(|key| (key, String::new())).collect();
            d.write_config(&changes);
        }
        Action::Slide(key) => {
            let value = slider_at(key, control.rect, x);
            d.settings.drag = Some((key, value));
            d.preview_panel(key, value);
        }
        Action::OpenSettings | Action::Reload => {}
        Action::Close => {
            d.close_popup();
            return true;
        }
    }
    d.set_hover(hit(d, x, y).unwrap_or(Hover::None));
    d.redraw_panel();
    true
}

/// Follows a slider drag, applying the value on screen.
///
/// # Arguments
/// * `d` - The desktop.
/// * `x` - Pointer X.
pub fn drag(d: &mut Desktop, x: f64) {
    let Some((key, current)) = d.settings.drag else {
        return;
    };
    let Some(rect) = slider_rect(d, key) else {
        return;
    };
    let value = slider_at(key, rect, x);
    if value != current {
        d.settings.drag = Some((key, value));
        d.preview_panel(key, value);
    }
    d.redraw_panel();
}

/// Ends a slider drag, writing the value.
///
/// # Arguments
/// * `d` - The desktop.
pub fn release(d: &mut Desktop) {
    if let Some((key, value)) = d.settings.drag.take() {
        d.cancel_preview();
        d.write_config(&[(key, value.to_string())]);
    }
}

/// Writes the focused field's text and drops the focus.
fn commit_field(d: &mut Desktop) {
    let Some(field) = d.settings.focus.take() else {
        return;
    };
    let text = std::mem::take(&mut d.settings.editing);
    let (key, value) = match field {
        Field::Color => match parse_color(&text) {
            Some(_) => (wallpaper_key(d, "background"), text.trim().to_string()),
            None => return,
        },
        Field::Theme(color) => match parse_color(&text) {
            Some(_) => (color.key(), text.trim().to_string()),
            None => return,
        },
        Field::Image => {
            let trimmed = text.trim();
            let value = if trimmed.is_empty() {
                "none".to_string()
            } else {
                trimmed.to_string()
            };
            (wallpaper_key(d, "wallpaper"), value)
        }
        Field::ClockFormat => ("clock_format", text),
        Field::StartIcon => ("start_icon", text.trim().to_string()),
    };
    d.write_config(&[(key, value)]);
}

/// Handles wheel scrolling over the page or the application list; returns
/// whether it landed in the window.
///
/// # Arguments
/// * `d` - The desktop.
/// * `x` - Pointer X.
/// * `y` - Pointer Y.
/// * `delta` - Scroll amount, positive downwards.
pub fn scroll(d: &mut Desktop, x: f64, y: f64, delta: f64) -> bool {
    let frame = frame(d);
    if !frame.contains(x, y) {
        return false;
    }
    let page = page_rect_in(frame);
    let content = build(d, frame, 0.0, view(d)).content as f64;
    let max = (content - page.h as f64).max(0.0);
    let next = (d.settings.scroll + delta.signum() * ui::SCROLL_STEP).clamp(0.0, max);
    if next != d.settings.scroll {
        d.settings.scroll = next;
        d.set_hover(hit(d, x, y).unwrap_or(Hover::None));
        d.redraw_panel();
    }
    true
}

/// Handles a key press while the window is open: typing goes to the focused
/// field, Enter writes it.
///
/// # Arguments
/// * `d` - The desktop.
/// * `key` - The key.
pub fn key(d: &mut Desktop, key: Key) {
    if d.settings.focus.is_none() {
        return;
    }
    match key {
        Key::Char(c) => d.settings.editing.push(c),
        Key::Backspace => {
            d.settings.editing.pop();
        }
        Key::Enter | Key::Tab => commit_field(d),
        _ => return,
    }
    d.redraw_panel();
}
