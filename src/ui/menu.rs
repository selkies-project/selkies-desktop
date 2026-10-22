//! The start menu: a search box, the categories with favorites and recent
//! launches, and the applications of the chosen category.

use crate::desktop::{Desktop, Hover, Popup};
use crate::entries;
use crate::event::Button;
use crate::geom::Rect;
use crate::i18n::{tr, trf};
use crate::render::{Align, Canvas};
use crate::ui;
use crate::xkb::Key;

/// Menu width.
const WIDTH: i32 = 500;
/// Height of the menu.
const HEIGHT: i32 = 440;
/// Shortest the menu shrinks on a small screen.
const MIN_HEIGHT: i32 = 200;
/// Width of the category column.
const CATEGORY_WIDTH: i32 = 180;
/// Height of a list row.
const ROW: i32 = 30;
/// Inner padding.
const PAD: i32 = 10;
/// Height of the search box.
const SEARCH_HEIGHT: i32 = 34;
/// Height of the footer.
const FOOTER_HEIGHT: i32 = 30;
/// Width of an application's right-click menu.
const CONTEXT_WIDTH: i32 = 230;
/// Row height of an application's right-click menu.
const CONTEXT_ROW: i32 = 30;

/// Which list the application column shows.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Section {
    /// The configured favorites.
    Favorites,
    /// Recently launched applications.
    Recent,
    /// Every application.
    All,
    /// One category, by index.
    Category(usize),
}

/// A row of the category column.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum CategoryRow {
    Section(Section),
    Separator,
}

/// The menu's state between events.
pub struct MenuState {
    /// The search text.
    pub query: String,
    /// The section whose applications are listed.
    pub section: Section,
    /// Row of the application list selected with the keyboard.
    pub selected: Option<usize>,
    /// Scroll offset of the category column.
    pub category_scroll: f64,
    /// Scroll offset of the application column.
    pub app_scroll: f64,
    /// The right-click menu open on an application row, with where it opened.
    pub context: Option<(usize, i32, i32)>,
    items: Vec<usize>,
    items_valid: bool,
    recent_ids: Vec<String>,
    identity: String,
}

impl Default for MenuState {
    fn default() -> MenuState {
        MenuState {
            query: String::new(),
            section: Section::All,
            selected: None,
            category_scroll: 0.0,
            app_scroll: 0.0,
            context: None,
            items: Vec::new(),
            items_valid: false,
            recent_ids: Vec::new(),
            identity: identity(),
        }
    }
}

impl MenuState {
    /// Forgets the computed application list.
    pub fn invalidate(&mut self) {
        self.items_valid = false;
    }

    /// Resets the menu for opening.
    ///
    /// # Arguments
    /// * `d` - The desktop, whose applications fill the lists.
    pub fn open(&mut self, d: &Desktop) {
        self.query.clear();
        self.selected = None;
        self.category_scroll = 0.0;
        self.app_scroll = 0.0;
        self.recent_ids = if d.config.track_recent {
            entries::recent()
        } else {
            Vec::new()
        };
        let favorites: Vec<usize> = d
            .config
            .favorites
            .iter()
            .filter_map(|id| d.apps.find(id))
            .collect();
        let recent: Vec<usize> = self.recent_ids.iter().filter_map(|id| d.apps.find(id)).collect();
        self.section = if !favorites.is_empty() {
            Section::Favorites
        } else if !recent.is_empty() {
            Section::Recent
        } else {
            Section::All
        };
        self.items_valid = false;
    }

    /// Clears transient state when the menu closes.
    pub fn close(&mut self) {
        self.query.clear();
        self.selected = None;
        self.context = None;
    }

    /// The application index shown at a row, when the list is current.
    ///
    /// # Arguments
    /// * `_d` - The desktop.
    /// * `row` - Row of the application list.
    pub fn visible_entry(&self, _d: &Desktop, row: usize) -> Option<usize> {
        if self.items_valid {
            self.items.get(row).copied()
        } else {
            None
        }
    }
}

/// `user@host` for the footer.
fn identity() -> String {
    let user = unsafe {
        let entry = libc::getpwuid(libc::getuid());
        if entry.is_null() || (*entry).pw_name.is_null() {
            None
        } else {
            Some(
                std::ffi::CStr::from_ptr((*entry).pw_name)
                    .to_string_lossy()
                    .to_string(),
            )
        }
    };
    let user = user
        .filter(|u| !u.is_empty())
        .or_else(|| std::env::var("USER").ok().filter(|u| !u.is_empty()))
        .unwrap_or_else(|| "user".to_string());
    let mut buf = [0u8; 256];
    let host = unsafe {
        if libc::gethostname(buf.as_mut_ptr() as *mut libc::c_char, buf.len()) == 0 {
            std::ffi::CStr::from_ptr(buf.as_ptr() as *const libc::c_char)
                .to_string_lossy()
                .to_string()
        } else {
            String::new()
        }
    };
    if host.is_empty() {
        user
    } else {
        format!("{user}@{host}")
    }
}

/// The rectangles of the menu.
struct Layout {
    frame: Rect,
    search: Rect,
    categories: Rect,
    apps: Rect,
    footer: Rect,
}

/// Computes the menu rectangles for the current panel size.  The menu has a
/// fixed height so it stays put while the lists change; the panel surface
/// may be taller than the screen, so the screen height, which the background
/// surface knows, bounds it.
fn layout(d: &Desktop) -> Layout {
    let bar = d.bar_rect();
    let screen_height = d
        .primary_background()
        .filter(|b| b.geometry.configured)
        .map_or(d.panel.height, |b| b.icon_area().h.min(d.panel.height));
    let searching = !d.menu.query.trim().is_empty();
    let height = HEIGHT
        .min(screen_height - d.config.panel_height - ui::POPUP_MARGIN * 2)
        .max(MIN_HEIGHT);
    let y = if d.panel_top() {
        bar.bottom() + ui::POPUP_MARGIN
    } else {
        bar.y - ui::POPUP_MARGIN - height
    };
    let width = WIDTH.min(d.panel.width - 2 * ui::POPUP_MARGIN);
    let x = (bar.x + ui::POPUP_MARGIN).min(d.panel.width - ui::POPUP_MARGIN - width);
    let frame = Rect::new(x.max(ui::POPUP_MARGIN), y, width, height);
    let search = Rect::new(frame.x + PAD, frame.y + PAD, frame.w - 2 * PAD, SEARCH_HEIGHT);
    let footer = Rect::new(frame.x, frame.bottom() - FOOTER_HEIGHT, frame.w, FOOTER_HEIGHT);
    let content_top = search.bottom() + PAD;
    let content_h = footer.y - content_top - 4;
    let categories = if searching {
        Rect::new(frame.x + PAD, content_top, 0, content_h)
    } else {
        Rect::new(frame.x + PAD, content_top, CATEGORY_WIDTH, content_h)
    };
    let apps_x = if searching {
        frame.x + PAD
    } else {
        categories.right() + PAD
    };
    let apps = Rect::new(apps_x, content_top, frame.right() - PAD - apps_x, content_h);
    Layout {
        frame,
        search,
        categories,
        apps,
        footer,
    }
}

/// The rows of the category column for the loaded applications.
fn category_rows(d: &Desktop) -> Vec<CategoryRow> {
    let mut rows = Vec::new();
    if d.config.favorites.iter().any(|id| d.apps.find(id).is_some()) {
        rows.push(CategoryRow::Section(Section::Favorites));
    }
    if d.menu.recent_ids.iter().any(|id| d.apps.find(id).is_some()) {
        rows.push(CategoryRow::Section(Section::Recent));
    }
    rows.push(CategoryRow::Section(Section::All));
    if !d.apps.categories.is_empty() {
        rows.push(CategoryRow::Separator);
    }
    for i in 0..d.apps.categories.len() {
        rows.push(CategoryRow::Section(Section::Category(i)));
    }
    rows
}

/// Display name of a section.
fn section_name(d: &Desktop, section: Section) -> String {
    match section {
        Section::Favorites => tr("Favorites").to_string(),
        Section::Recent => tr("Recent").to_string(),
        Section::All => tr("All applications").to_string(),
        Section::Category(i) => d
            .apps
            .categories
            .get(i)
            .map(|c| entries::category_name(&c.name).to_string())
            .unwrap_or_default(),
    }
}

/// Recomputes the application list when the query, section or applications changed.
fn ensure_items(d: &mut Desktop) {
    if d.menu.items_valid {
        return;
    }
    let query = d.menu.query.trim().to_string();
    let items = if !query.is_empty() {
        d.apps.search(&query)
    } else {
        match d.menu.section {
            Section::Favorites => d
                .config
                .favorites
                .iter()
                .filter_map(|id| d.apps.find(id))
                .collect(),
            Section::Recent => d
                .menu
                .recent_ids
                .iter()
                .filter_map(|id| d.apps.find(id))
                .collect(),
            Section::All => d.apps.all_sorted(),
            Section::Category(i) => d
                .apps
                .categories
                .get(i)
                .map(|c| c.entries.clone())
                .unwrap_or_default(),
        }
    };
    if !d
        .apps
        .categories
        .iter()
        .enumerate()
        .any(|(i, _)| d.menu.section == Section::Category(i))
        && matches!(d.menu.section, Section::Category(_))
    {
        d.menu.section = Section::All;
    }
    d.menu.items = items;
    d.menu.items_valid = true;
    if let Some(selected) = d.menu.selected
        && selected >= d.menu.items.len()
    {
        d.menu.selected = if d.menu.items.is_empty() {
            None
        } else {
            Some(d.menu.items.len() - 1)
        };
    }
}

/// The rows of an application's right-click menu: each toggles the
/// application's place in a list setting.
fn context_rows(d: &Desktop, row: usize) -> Vec<(&'static str, &'static str, String)> {
    let Some(index) = d.menu.items.get(row) else {
        return Vec::new();
    };
    let id = d.apps.entries[*index].id.clone();
    let favorite = d.config.favorites.contains(&id);
    let launcher = d.config.launchers.contains(&id);
    vec![
        (
            if favorite {
                tr("Unpin from favorites")
            } else {
                tr("Pin to favorites")
            },
            "favorites",
            id.clone(),
        ),
        (
            if launcher {
                tr("Remove from quick launch")
            } else {
                tr("Add to quick launch")
            },
            "launchers",
            id,
        ),
    ]
}

/// The rectangle of the open right-click menu, kept inside the panel surface.
fn context_rect(d: &Desktop) -> Option<Rect> {
    let (row, x, y) = d.menu.context?;
    let height = context_rows(d, row).len() as i32 * CONTEXT_ROW + 12;
    let x = x
        .min(d.panel.width - CONTEXT_WIDTH - ui::POPUP_MARGIN)
        .max(ui::POPUP_MARGIN);
    let y = if y + height > d.panel.height - ui::POPUP_MARGIN {
        y - height
    } else {
        y
    };
    Some(Rect::new(x, y.max(ui::POPUP_MARGIN), CONTEXT_WIDTH, height))
}

/// The hover target under a point in the menu, or `None` outside of it.
///
/// # Arguments
/// * `d` - The desktop.
/// * `x` - Pointer X.
/// * `y` - Pointer Y.
pub fn hit(d: &mut Desktop, x: f64, y: f64) -> Option<Hover> {
    ensure_items(d);
    let layout = layout(d);
    if let Some(rect) = context_rect(d) {
        if rect.contains(x, y) {
            let row = ((y - rect.y as f64 - 6.0) / CONTEXT_ROW as f64).floor();
            if row >= 0.0 && (row as usize) < 2 {
                return Some(Hover::MenuContextItem(row as usize));
            }
            return Some(Hover::None);
        }
        if !layout.frame.contains(x, y) {
            return Some(Hover::None);
        }
        return Some(Hover::None);
    }
    if !layout.frame.contains(x, y) {
        return None;
    }
    if layout.categories.w > 0 && layout.categories.contains(x, y) {
        let row = ((y - layout.categories.y as f64 + d.menu.category_scroll) / ROW as f64).floor();
        if row >= 0.0 {
            let rows = category_rows(d);
            if let Some(CategoryRow::Section(section)) = rows.get(row as usize) {
                if d.menu.section != *section && d.menu.query.trim().is_empty() {
                    d.menu.section = *section;
                    d.menu.app_scroll = 0.0;
                    d.menu.selected = None;
                    d.menu.items_valid = false;
                    ensure_items(d);
                }
                return Some(Hover::MenuCategory(row as usize));
            }
        }
        return Some(Hover::None);
    }
    if layout.apps.contains(x, y) {
        let row = ((y - layout.apps.y as f64 + d.menu.app_scroll) / ROW as f64).floor();
        if row >= 0.0 && (row as usize) < d.menu.items.len() {
            return Some(Hover::MenuApp(row as usize));
        }
    }
    Some(Hover::None)
}

/// Handles a click; returns whether it landed inside the menu.
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
    if let Some((row, _, _)) = d.menu.context.take() {
        if button == Button::Left
            && let Hover::MenuContextItem(item) = hover
            && let Some((_, key, id)) = context_rows(d, row).get(item).cloned()
        {
            d.toggle_list_entry(key, &id);
        }
        let hover = hit(d, x, y).unwrap_or(Hover::None);
        d.set_hover(hover);
        d.redraw_panel();
        return true;
    }
    if button == Button::Right {
        if let Hover::MenuApp(row) = hover {
            d.menu.context = Some((row, x as i32, y as i32));
            d.set_hover(Hover::None);
            d.redraw_panel();
        }
        return true;
    }
    if button != Button::Left {
        return true;
    }
    match hover {
        Hover::MenuApp(row) => {
            if let Some(index) = d.menu.items.get(row).copied() {
                d.launch_app(index);
            }
        }
        Hover::MenuCategory(_) => {
            d.redraw_panel();
        }
        _ => {}
    }
    true
}

/// Handles wheel scrolling; returns whether it landed inside the menu.
///
/// # Arguments
/// * `d` - The desktop.
/// * `x` - Pointer X.
/// * `y` - Pointer Y.
/// * `delta` - Scroll amount, positive downwards.
pub fn scroll(d: &mut Desktop, x: f64, y: f64, delta: f64) -> bool {
    ensure_items(d);
    let layout = layout(d);
    if !layout.frame.contains(x, y) {
        return false;
    }
    let step = delta.signum() * ui::SCROLL_STEP;
    if layout.categories.w > 0 && layout.categories.contains(x, y) {
        let content = category_rows(d).len() as f64 * ROW as f64;
        let max = (content - layout.categories.h as f64).max(0.0);
        d.menu.category_scroll = (d.menu.category_scroll + step).clamp(0.0, max);
        d.redraw_panel();
    } else if layout.apps.contains(x, y) {
        let content = d.menu.items.len() as f64 * ROW as f64;
        let max = (content - layout.apps.h as f64).max(0.0);
        d.menu.app_scroll = (d.menu.app_scroll + step).clamp(0.0, max);
        if let Some(hover) = hit(d, x, y) {
            d.set_hover(hover);
        }
        d.redraw_panel();
    }
    true
}

/// Handles a key press while the menu is open.
///
/// # Arguments
/// * `d` - The desktop.
/// * `key` - The key.
pub fn key(d: &mut Desktop, key: Key) {
    ensure_items(d);
    let count = d.menu.items.len();
    match key {
        Key::Char(c) => {
            d.menu.context = None;
            d.menu.query.push(c);
            d.menu.items_valid = false;
            d.menu.app_scroll = 0.0;
            ensure_items(d);
            d.menu.selected = if d.menu.items.is_empty() { None } else { Some(0) };
        }
        Key::Backspace => {
            if d.menu.query.pop().is_some() {
                d.menu.items_valid = false;
                d.menu.app_scroll = 0.0;
                ensure_items(d);
                d.menu.selected = if d.menu.query.is_empty() || d.menu.items.is_empty() {
                    None
                } else {
                    Some(0)
                };
            }
        }
        Key::Enter => {
            let row = d.menu.selected.or(if count > 0 { Some(0) } else { None });
            if let Some(index) = row.and_then(|r| d.menu.items.get(r).copied()) {
                d.launch_app(index);
                return;
            }
        }
        Key::Down => move_selection(d, 1),
        Key::Up => move_selection(d, -1),
        Key::PageDown => move_selection(d, 10),
        Key::PageUp => move_selection(d, -10),
        Key::Home => {
            if count > 0 {
                d.menu.selected = Some(0);
                scroll_to_selection(d);
            }
        }
        Key::End => {
            if count > 0 {
                d.menu.selected = Some(count - 1);
                scroll_to_selection(d);
            }
        }
        Key::Left | Key::Right => {
            if d.menu.query.trim().is_empty() {
                let rows = category_rows(d);
                let sections: Vec<Section> = rows
                    .iter()
                    .filter_map(|r| {
                        if let CategoryRow::Section(s) = r {
                            Some(*s)
                        } else {
                            None
                        }
                    })
                    .collect();
                if let Some(pos) = sections.iter().position(|s| *s == d.menu.section) {
                    let next = if key == Key::Right {
                        (pos + 1) % sections.len()
                    } else {
                        (pos + sections.len() - 1) % sections.len()
                    };
                    d.menu.section = sections[next];
                    d.menu.items_valid = false;
                    d.menu.selected = None;
                    d.menu.app_scroll = 0.0;
                    let row_index = rows
                        .iter()
                        .position(|r| *r == CategoryRow::Section(sections[next]))
                        .unwrap_or(0);
                    let layout = layout(d);
                    let row_top = row_index as f64 * ROW as f64;
                    if row_top < d.menu.category_scroll {
                        d.menu.category_scroll = row_top;
                    } else if row_top + ROW as f64 > d.menu.category_scroll + layout.categories.h as f64 {
                        d.menu.category_scroll = row_top + ROW as f64 - layout.categories.h as f64;
                    }
                }
            }
        }
        Key::Tab | Key::Delete | Key::Escape | Key::Super | Key::Function(_) => {}
    }
    d.redraw_panel();
}

/// Moves the keyboard selection by a number of rows.
fn move_selection(d: &mut Desktop, by: i32) {
    let count = d.menu.items.len() as i32;
    if count == 0 {
        d.menu.selected = None;
        return;
    }
    let current = d.menu.selected.map(|s| s as i32).unwrap_or(-1);
    let next = (current + by).clamp(0, count - 1);
    d.menu.selected = Some(next as usize);
    scroll_to_selection(d);
}

/// Scrolls the application list so the selected row is visible.
fn scroll_to_selection(d: &mut Desktop) {
    let Some(selected) = d.menu.selected else {
        return;
    };
    let layout = layout(d);
    let row_top = selected as f64 * ROW as f64;
    if row_top < d.menu.app_scroll {
        d.menu.app_scroll = row_top;
    } else if row_top + ROW as f64 > d.menu.app_scroll + layout.apps.h as f64 {
        d.menu.app_scroll = row_top + ROW as f64 - layout.apps.h as f64;
    }
}

/// Draws the menu and returns its rectangle.
///
/// # Arguments
/// * `d` - The desktop.
/// * `canvas` - The panel canvas.
pub fn draw(d: &mut Desktop, canvas: &mut Canvas<'_>) -> Rect {
    ensure_items(d);
    let layout = layout(d);
    ui::draw_popup_frame(canvas, layout.frame);

    canvas.fill_rounded(layout.search, 6.0, ui::field_bg());
    if let Some(glass) = ui::search_path(layout.search.x + 16, layout.search.y + layout.search.h / 2) {
        canvas.stroke_path(&glass, 1.6, ui::text_dim());
    }
    let text_rect = Rect::new(
        layout.search.x + 32,
        layout.search.y,
        layout.search.w - 40,
        layout.search.h,
    );
    if d.menu.query.is_empty() {
        ui::label(
            canvas,
            &mut d.fonts,
            tr("Search applications"),
            text_rect,
            ui::FONT_SIZE,
            false,
            Align::Left,
            ui::text_dim(),
        );
    } else {
        let query = d.menu.query.clone();
        ui::label(
            canvas,
            &mut d.fonts,
            &query,
            text_rect,
            ui::FONT_SIZE,
            false,
            Align::Left,
            ui::text(),
        );
    }
    if d.popup == Popup::Menu {
        let query = d.menu.query.clone();
        let caret_x = text_rect.x
            + ui::text_width(&mut d.fonts, &query, ui::FONT_SIZE, false, d.panel.scale).min(text_rect.w - 2);
        canvas.fill_rect(
            Rect::new(caret_x + 1, text_rect.y + 9, 1, text_rect.h - 18),
            ui::text(),
        );
    }

    if layout.categories.w > 0 {
        canvas.clip(layout.categories);
        let rows = category_rows(d);
        for (row, item) in rows.iter().enumerate() {
            let y = layout.categories.y + row as i32 * ROW - d.menu.category_scroll as i32;
            if y + ROW < layout.categories.y || y > layout.categories.bottom() {
                continue;
            }
            match item {
                CategoryRow::Separator => {
                    canvas.fill_rect(
                        Rect::new(layout.categories.x + 8, y + ROW / 2, layout.categories.w - 16, 1),
                        ui::separator(),
                    );
                }
                CategoryRow::Section(section) => {
                    let rect = Rect::new(layout.categories.x, y, layout.categories.w, ROW);
                    let current = d.menu.section == *section;
                    let hovered = d.hover == Hover::MenuCategory(row);
                    if current {
                        canvas.fill_rounded(rect, 5.0, ui::active_bg());
                    } else if hovered {
                        canvas.fill_rounded(rect, 5.0, ui::hover_bg());
                    }
                    let name = section_name(d, *section);
                    let label_rect = Rect::new(rect.x + 10, rect.y, rect.w - 26, rect.h);
                    ui::label(
                        canvas,
                        &mut d.fonts,
                        &name,
                        label_rect,
                        ui::FONT_SIZE,
                        current,
                        Align::Left,
                        ui::text(),
                    );
                    if let Some(chevron) = ui::chevron_path(rect.right() - 12, rect.y + rect.h / 2) {
                        canvas.stroke_path(&chevron, 1.5, if current { ui::text() } else { ui::text_dim() });
                    }
                }
            }
        }
        canvas.reset_clip();
    }

    canvas.clip(layout.apps);
    if d.menu.items.is_empty() {
        let message = if d.menu.query.trim().is_empty() {
            tr("No applications")
        } else {
            tr("No matches")
        };
        let rect = Rect::new(layout.apps.x, layout.apps.y, layout.apps.w, ROW);
        ui::label(
            canvas,
            &mut d.fonts,
            message,
            rect,
            ui::FONT_SIZE,
            false,
            Align::Left,
            ui::text_dim(),
        );
    }
    let items = d.menu.items.clone();
    for (row, index) in items.iter().enumerate() {
        let y = layout.apps.y + row as i32 * ROW - d.menu.app_scroll as i32;
        if y + ROW < layout.apps.y || y > layout.apps.bottom() {
            continue;
        }
        let rect = Rect::new(layout.apps.x, y, layout.apps.w, ROW);
        let selected = d.menu.selected == Some(row);
        let hovered = d.hover == Hover::MenuApp(row);
        if selected {
            canvas.fill_rounded(rect, 5.0, ui::active_bg());
        } else if hovered {
            canvas.fill_rounded(rect, 5.0, ui::hover_bg());
        }
        let icon_rect = Rect::new(
            rect.x + 8,
            rect.y + (ROW - ui::ICON_SIZE) / 2,
            ui::ICON_SIZE,
            ui::ICON_SIZE,
        );
        if let Some(icon) = d.app_icon(*index) {
            canvas.draw_image_fit(&icon, icon_rect);
        } else {
            canvas.fill_rounded(icon_rect.inflate(-3), 3.0, ui::field_bg());
        }
        let name = d.apps.entries[*index].name.clone();
        let label_rect = Rect::new(
            icon_rect.right() + 10,
            rect.y,
            rect.right() - icon_rect.right() - 18,
            rect.h,
        );
        ui::label(
            canvas,
            &mut d.fonts,
            &name,
            label_rect,
            ui::FONT_SIZE,
            false,
            Align::Left,
            ui::text(),
        );
    }
    canvas.reset_clip();

    canvas.fill_rect(
        Rect::new(
            layout.footer.x + PAD,
            layout.footer.y,
            layout.footer.w - 2 * PAD,
            1,
        ),
        ui::separator(),
    );
    let identity = d.menu.identity.clone();
    let footer_text = Rect::new(
        layout.footer.x + PAD + 2,
        layout.footer.y + 2,
        layout.footer.w - 2 * PAD,
        layout.footer.h - 6,
    );
    ui::label(
        canvas,
        &mut d.fonts,
        &identity,
        footer_text,
        ui::FONT_SMALL,
        false,
        Align::Left,
        ui::text_dim(),
    );
    let count_text = trf("{n} apps", &[("n", &d.apps.entries.len().to_string())]);
    ui::label(
        canvas,
        &mut d.fonts,
        &count_text,
        footer_text,
        ui::FONT_SMALL,
        false,
        Align::Right,
        ui::text_dim(),
    );

    if let Some((row, _, _)) = d.menu.context
        && let Some(rect) = context_rect(d)
    {
        canvas.fill_rounded(
            rect.inflate(1),
            ui::POPUP_RADIUS,
            crate::render::rgba(0, 0, 0, 0x50),
        );
        ui::draw_popup_frame(canvas, rect);
        for (i, (label, _, _)) in context_rows(d, row).into_iter().enumerate() {
            let item = Rect::new(
                rect.x + 6,
                rect.y + 6 + i as i32 * CONTEXT_ROW,
                rect.w - 12,
                CONTEXT_ROW,
            );
            if d.hover == Hover::MenuContextItem(i) {
                canvas.fill_rounded(item, 5.0, ui::hover_bg());
            }
            ui::label(
                canvas,
                &mut d.fonts,
                label,
                Rect::new(item.x + 10, item.y, item.w - 20, item.h),
                ui::FONT_SIZE,
                false,
                Align::Left,
                ui::text(),
            );
        }
    }

    layout.frame
}
