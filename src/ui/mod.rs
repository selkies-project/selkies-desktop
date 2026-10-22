//! The visual language shared by every part of the desktop: sizes, colors
//! and the small widgets drawn more than once.

pub mod background;
pub mod files;
pub mod menu;
pub mod panel;
pub mod popups;
pub mod settings;

use tiny_skia::{Color, Path, PathBuilder};

use crate::geom::Rect;
use crate::render::{Align, Canvas, Fonts, rgb, rgba};

/// Default height of the bar; the `panel_height` setting overrides it.
pub const PANEL_HEIGHT: i32 = 32;
/// Width of the start button.
pub const START_WIDTH: i32 = 44;
/// Icon size in the bar and the menu's application list.
pub const ICON_SIZE: i32 = 18;
/// Icon size of desktop launchers.
pub const DESKTOP_ICON_SIZE: i32 = 48;
/// Width of a desktop launcher cell.
pub const DESKTOP_CELL_WIDTH: i32 = 96;
/// Height of a desktop launcher cell.
pub const DESKTOP_CELL_HEIGHT: i32 = 104;
/// Corner radius of popups.
pub const POPUP_RADIUS: f32 = 8.0;
/// Space between the bar and a popup, and between popups and the screen edge.
pub const POPUP_MARGIN: i32 = 6;
/// The tallest popup area the panel surface must leave room for.
pub const POPUP_AREA_HEIGHT: i32 = 620;
/// Base font size of the UI.
pub const FONT_SIZE: f32 = 13.0;
/// Small font size for secondary text.
pub const FONT_SMALL: f32 = 11.5;
/// Pixels one wheel step scrolls a list.
pub const SCROLL_STEP: f64 = 48.0;
/// Milliseconds a second click may follow the first to count as a double click.
pub const DOUBLE_CLICK_MS: u32 = 400;
/// Milliseconds the pointer rests before a tooltip shows.
pub const TOOLTIP_DELAY_MS: u64 = 600;
/// Narrowest a file manager window may be made.
pub const WINDOW_MIN_WIDTH: i32 = 480;
/// Shortest a file manager window may be made.
pub const WINDOW_MIN_HEIGHT: i32 = 320;

/// The colors the user can set: the four the rest derive from.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Theme {
    /// Markers, selections, switches and the active choice.
    pub accent: Color,
    /// The bar.
    pub panel: Color,
    /// Popups, menus and windows.
    pub popup: Color,
    /// Text.
    pub text: Color,
}

impl Default for Theme {
    fn default() -> Theme {
        Theme {
            accent: rgb(0x79, 0x67, 0xc5),
            panel: rgb(0x0a, 0x0e, 0x17),
            popup: rgb(0x12, 0x18, 0x26),
            text: rgb(0xee, 0xf2, 0xf8),
        }
    }
}

/// The theme in use.
static THEME: std::sync::RwLock<Option<Theme>> = std::sync::RwLock::new(None);

/// Puts a theme in use for everything drawn from now on.
///
/// # Arguments
/// * `theme` - The colors.
pub fn set_theme(theme: Theme) {
    *THEME.write().unwrap_or_else(|poisoned| poisoned.into_inner()) = Some(theme);
}

/// The theme in use.
pub fn theme() -> Theme {
    THEME
        .read()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
        .unwrap_or_default()
}

/// A color with another alpha.
fn with_alpha(color: Color, alpha: u8) -> Color {
    let c = color.to_color_u8();
    rgba(c.red(), c.green(), c.blue(), alpha)
}

/// A color moved towards black or white.
///
/// # Arguments
/// * `color` - The color.
/// * `by` - Steps to add to each channel, negative to darken.
fn shifted(color: Color, by: i32) -> Color {
    let c = color.to_color_u8();
    let step = |v: u8| (v as i32 + by).clamp(0, 255) as u8;
    rgba(step(c.red()), step(c.green()), step(c.blue()), c.alpha())
}

/// Bar background at an opacity in percent.
///
/// # Arguments
/// * `opacity` - Opacity from 0 to 100.
pub fn panel_bg(opacity: i32) -> Color {
    with_alpha(theme().panel, (opacity.clamp(0, 100) * 255 / 100) as u8)
}
/// Corner radius of a bar narrower than the screen.
pub const PANEL_RADIUS: f32 = 10.0;
/// Popup background.
pub fn popup_bg() -> Color {
    theme().popup
}
/// Popup outline.
pub fn popup_border() -> Color {
    with_alpha(theme().text, 0x24)
}
/// Raised surface inside a popup, such as a card or the search box.
pub fn field_bg() -> Color {
    with_alpha(theme().text, 0x0c)
}
/// Hover highlight.
pub fn hover_bg() -> Color {
    with_alpha(theme().text, 0x18)
}
/// Pressed or selected highlight.
pub fn active_bg() -> Color {
    with_alpha(theme().accent, 0x48)
}
/// Tooltip background.
pub fn tooltip_bg() -> Color {
    with_alpha(shifted(theme().popup, 14), 0xfa)
}
/// Accent, for markers, selections, switches and the active choice.
pub fn accent() -> Color {
    theme().accent
}
/// The accent at an alpha.
///
/// # Arguments
/// * `alpha` - Opacity from 0 to 255.
pub fn accent_alpha(alpha: u8) -> Color {
    with_alpha(theme().accent, alpha)
}
/// Primary text.
pub fn text() -> Color {
    theme().text
}
/// Secondary text.
pub fn text_dim() -> Color {
    with_alpha(theme().text, 0xa8)
}
/// Separator lines.
pub fn separator() -> Color {
    with_alpha(theme().text, 0x1c)
}
/// The text color at an alpha.
///
/// # Arguments
/// * `alpha` - Opacity from 0 to 255.
pub fn text_alpha(alpha: u8) -> Color {
    with_alpha(theme().text, alpha)
}
/// Text drawn over the accent: black on a light accent, white on a dark one.
pub fn on_accent() -> Color {
    let c = theme().accent.to_color_u8();
    let luma = 0.299 * c.red() as f32 + 0.587 * c.green() as f32 + 0.114 * c.blue() as f32;
    if luma > 160.0 {
        rgb(0x10, 0x10, 0x10)
    } else {
        rgb(0xff, 0xff, 0xff)
    }
}

/// Draws a popup's rounded background with its border.
///
/// # Arguments
/// * `canvas` - The target.
/// * `rect` - The popup's rectangle.
pub fn draw_popup_frame(canvas: &mut Canvas<'_>, rect: Rect) {
    canvas.fill_rounded(rect, POPUP_RADIUS, popup_bg());
    canvas.stroke_rounded(rect, POPUP_RADIUS, 1.0, popup_border());
}

/// Shapes text so it fits a width, replacing the tail with an ellipsis when it
/// does not.
///
/// # Arguments
/// * `fonts` - The font system.
/// * `text` - The text.
/// * `px` - Font size in physical pixels.
/// * `bold` - Whether to use the bold weight.
/// * `max_width` - Width limit in physical pixels.
pub fn shape_fit(
    fonts: &mut Fonts,
    text: &str,
    px: f32,
    bold: bool,
    max_width: f32,
) -> std::rc::Rc<crate::render::Text> {
    let full = fonts.shape(text, px, bold);
    if full.width <= max_width || max_width <= 0.0 {
        return full;
    }
    let chars: Vec<char> = text.chars().collect();
    let mut lo = 0usize;
    let mut hi = chars.len();
    while lo < hi {
        let mid = (lo + hi).div_ceil(2);
        let candidate: String = chars[..mid].iter().collect::<String>().trim_end().to_string() + "…";
        if fonts.shape(&candidate, px, bold).width <= max_width {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    let candidate: String = chars[..lo].iter().collect::<String>().trim_end().to_string() + "…";
    fonts.shape(&candidate, px, bold)
}

/// Draws a single line of text fitted into a box.
///
/// # Arguments
/// * `canvas` - The target.
/// * `fonts` - The font system.
/// * `text` - The text.
/// * `rect` - The logical box; text is vertically centered in it.
/// * `size` - Font size in logical pixels.
/// * `bold` - Whether to use the bold weight.
/// * `align` - Horizontal alignment.
/// * `color` - Text color.
#[allow(clippy::too_many_arguments)]
pub fn label(
    canvas: &mut Canvas<'_>,
    fonts: &mut Fonts,
    text: &str,
    rect: Rect,
    size: f32,
    bold: bool,
    align: Align,
    color: Color,
) {
    if rect.is_empty() || text.is_empty() {
        return;
    }
    let scale = canvas.scale() as f32;
    let shaped = shape_fit(fonts, text, size * scale, bold, rect.w as f32 * scale);
    canvas.draw_text(fonts, &shaped, rect.x, rect.y + rect.h / 2, rect.w, align, color);
}

/// Splits a name over at most two lines at word boundaries, the second
/// ellipsized when it still does not fit.
///
/// # Arguments
/// * `fonts` - The font system.
/// * `text` - The text.
/// * `px` - Font size in physical pixels.
/// * `max_width` - Width limit in physical pixels.
pub fn wrap_two_lines(
    fonts: &mut Fonts,
    text: &str,
    px: f32,
    max_width: f32,
) -> (
    std::rc::Rc<crate::render::Text>,
    Option<std::rc::Rc<crate::render::Text>>,
) {
    let whole = fonts.shape(text, px, false);
    if whole.width <= max_width {
        return (whole, None);
    }
    let words: Vec<&str> = text.split_whitespace().collect();
    let mut split = 0;
    for i in 1..words.len() {
        let head = words[..i].join(" ");
        if fonts.shape(&head, px, false).width <= max_width {
            split = i;
        } else {
            break;
        }
    }
    if split == 0 {
        return (shape_fit(fonts, text, px, false, max_width), None);
    }
    let first = words[..split].join(" ");
    let rest = words[split..].join(" ");
    let line1 = fonts.shape(&first, px, false);
    let line2 = shape_fit(fonts, &rest, px, false, max_width);
    (line1, Some(line2))
}

/// Width in logical pixels a line of text takes.
///
/// # Arguments
/// * `fonts` - The font system.
/// * `text` - The text.
/// * `size` - Font size in logical pixels.
/// * `bold` - Whether to use the bold weight.
/// * `scale` - The output scale.
pub fn text_width(fonts: &mut Fonts, text: &str, size: f32, bold: bool, scale: i32) -> i32 {
    (fonts.shape(text, size * scale as f32, bold).width / scale as f32).ceil() as i32
}

/// A speaker glyph with waves for the volume level, as a path in logical
/// coordinates inside a box.
///
/// # Arguments
/// * `rect` - The box the glyph is centered in.
/// * `level` - Volume from 0 to 100; selects how many waves are drawn.
/// * `muted` - Whether to draw the mute cross instead of waves.
pub fn speaker_path(rect: Rect, level: u8, muted: bool) -> (Path, Option<Path>) {
    let cx = rect.x as f32 + rect.w as f32 / 2.0;
    let cy = rect.y as f32 + rect.h as f32 / 2.0;
    let mut pb = PathBuilder::new();
    pb.move_to(cx - 7.0, cy - 2.5);
    pb.line_to(cx - 4.0, cy - 2.5);
    pb.line_to(cx - 0.5, cy - 6.0);
    pb.line_to(cx - 0.5, cy + 6.0);
    pb.line_to(cx - 4.0, cy + 2.5);
    pb.line_to(cx - 7.0, cy + 2.5);
    pb.close();
    let body = pb
        .finish()
        .unwrap_or_else(|| PathBuilder::from_rect(tiny_skia::Rect::from_xywh(cx, cy, 1.0, 1.0).unwrap()));
    let mut waves = PathBuilder::new();
    if muted {
        waves.move_to(cx + 2.0, cy - 3.0);
        waves.line_to(cx + 7.0, cy + 3.0);
        waves.move_to(cx + 7.0, cy - 3.0);
        waves.line_to(cx + 2.0, cy + 3.0);
    } else {
        if level > 0 {
            waves.move_to(cx + 2.0, cy - 2.5);
            waves.quad_to(cx + 4.0, cy, cx + 2.0, cy + 2.5);
        }
        if level > 33 {
            waves.move_to(cx + 4.0, cy - 5.0);
            waves.quad_to(cx + 7.5, cy, cx + 4.0, cy + 5.0);
        }
        if level > 66 {
            waves.move_to(cx + 6.0, cy - 7.5);
            waves.quad_to(cx + 11.0, cy, cx + 6.0, cy + 7.5);
        }
    }
    (body, waves.finish())
}

/// A chevron pointing right, for category rows.
///
/// # Arguments
/// * `x` - Logical X of the chevron's left.
/// * `cy` - Logical Y of its vertical center.
pub fn chevron_path(x: i32, cy: i32) -> Option<Path> {
    let mut pb = PathBuilder::new();
    pb.move_to(x as f32, cy as f32 - 4.0);
    pb.line_to(x as f32 + 4.0, cy as f32);
    pb.line_to(x as f32, cy as f32 + 4.0);
    pb.finish()
}

/// A magnifying glass for the search box.
///
/// # Arguments
/// * `cx` - Logical X of the glass center.
/// * `cy` - Logical Y of the glass center.
pub fn search_path(cx: i32, cy: i32) -> Option<Path> {
    let mut pb = PathBuilder::new();
    pb.push_circle(cx as f32 - 1.5, cy as f32 - 1.5, 5.0);
    pb.move_to(cx as f32 + 2.5, cy as f32 + 2.5);
    pb.line_to(cx as f32 + 6.5, cy as f32 + 6.5);
    pb.finish()
}
