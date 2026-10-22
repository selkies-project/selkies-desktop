# Selkies Desktop

[![Checks](https://github.com/selkies-project/selkies-desktop/actions/workflows/checks.yml/badge.svg)](https://github.com/selkies-project/selkies-desktop/actions/workflows/checks.yml) [![License: MPL 2.0](https://img.shields.io/badge/License-MPL%202.0-brightgreen.svg)](https://opensource.org/licenses/MPL-2.0) [![Discord](https://img.shields.io/badge/dynamic/json?logo=discord&label=Discord%20Members&query=approximate_member_count&url=https%3A%2F%2Fdiscordapp.com%2Fapi%2Finvites%2FwDNGDeSW5F%3Fwith_counts%3Dtrue)](https://discord.gg/wDNGDeSW5F) [![Ask DeepWiki](https://img.shields.io/badge/Ask-DeepWiki-blue)](https://deepwiki.com/selkies-project/selkies-desktop)

> **Notice:** Selkies Desktop is **not** intended for general consumption. It is
> not aiming to be a general-purpose Desktop Environment (DE) for people to use
> outside of its specific ecosystem.

Selkies Desktop is the panel, start menu, taskbar and desktop layer of
[Selkies](https://github.com/selkies-project/selkies) containers.  It runs on
top of the **labwc** Wayland compositor or the **openbox** X11 window manager,
which stay in charge of the windows; one static binary supports both and picks
Wayland when a compositor with layer shell support is reachable, X11 otherwise.

Rather than a configurable desktop environment, it is the fixed set of things
people reach for in a multi-application container:

* **Start menu** with a search box (type to filter, arrows and Enter to
  launch), favorites, recently launched applications and the usual
  categories, read from the standard `.desktop` files.  Applications that
  appear or disappear, such as a [proot-apps](https://github.com/linuxserver/proot-apps)
  install, show up without a restart.
* **Taskbar** with window icons, hover tooltips, a minimize on click of the
  active window, middle click to close, a right-click menu with minimize,
  maximize and close, and wheel scrolling to cycle windows.
* **Clock** with a calendar popup, **volume** control for the PulseAudio
  sink that Selkies streams, and a **show desktop** button.
* **Wallpaper** layer: an image of the user's choosing over a solid color or
  a dithered gradient, the Selkies logo centered on black by default, and
  launchers mapped from `~/Desktop`.
* **Quick launch** buttons beside the start button and a **system tray**
  beside the clock that hosts the XEmbed icons applications such as Steam
  dock, under openbox and under labwc through XWayland.
* **Settings** from a right click on the bar, in three pages: the bar's edge,
  auto-hide, height, width, opacity, icon, items, quick launch buttons and
  start menu favorites; the file manager's view, sort and hidden files; and
  the wallpaper of every monitor, shared or one by one, with the four colors
  of the interface.  Each page has a reset,
  and a right click on an application in the start menu pins it to the
  favorites or the quick launch.  A right click on the desktop still opens
  the window manager's own menu.
* **Files**, a small file manager in the start menu, opened by `xdg-open` for
  folders and by the wallpaper picker: a sidebar with the home, Desktop,
  Documents, Downloads, Pictures, Music and Videos folders that exist, icon
  and list views with thumbnails for PNG and JPEG files, sorting by name,
  size, date or type, type-to-filter, breadcrumbs with a location field,
  back and forward history, and the basics: open with the default or a
  chosen application, cut, copy and paste, rename, new folder, and delete
  behind a confirmation (there is no trash).  Files are dragged between
  windows, onto the sidebar, and onto the desktop; dropping onto another
  application hands it the paths on Wayland.  Hidden files toggle with
  Ctrl+H, the location field opens with Ctrl+L, and a folder that changes
  on disk refreshes on its own.
* **Every monitor** gets the wallpaper: one layer surface per Wayland output,
  including the screens Selkies adds for extra displays, and each RandR
  monitor on X11.
* **HiDPI** rendering at the output's scale on both display servers.
* **Nineteen languages** for everything the desktop says, picked from the
  locale environment or the `language` setting; the translations are the
  `locales/*.txt` files, one `English = translation` line each, compiled in.

Hardened images that make a terminal unexecutable (`DISABLE_TERMINALS`) see
its menu entries disappear as well, since an entry whose program may not be
run is left out.

## Configuration

Settings live in `~/.config/selkies-desktop/config` as `key = value` lines,
which the settings window (right click on the bar) writes, and every key can
be set from the environment as `SELKIES_DESKTOP_<KEY>` (for example
`SELKIES_DESKTOP_WALLPAPER`), which takes precedence.  The file is watched,
so an edit applies within a moment.

| Key | Default | Meaning |
| --- | --- | --- |
| `panel_position` | `bottom` | `bottom` or `top` |
| `panel_height` | `32` | Height of the bar in logical pixels, 24 to 64; icons grow with it |
| `panel_width` | `100` | Width of the bar as a percentage of the screen, centered with rounded corners when narrower |
| `panel_opacity` | `95` | Opacity of the bar's background in percent |
| `panel_autohide` | `false` | Hide the bar until the pointer touches the screen edge it sits on; it then reserves no space |
| `color_accent` | `#7967c5` | Color of markers, selections, switches and the chosen option |
| `color_panel` | `#0a0e17` | Color of the bar |
| `color_popup` | `#121826` | Color of menus, popups and windows |
| `color_text` | `#eef2f8` | Color of text; hover, separator and field shades derive from it |
| `language` | from `LC_ALL`, `LC_MESSAGES`, `LANG` | Interface language: `en`, `es`, `zh_cn`, `zh_tw`, `hi`, `pt`, `fr`, `ru`, `de`, `tr`, `it`, `nl`, `ar`, `ko`, `ja`, `vi`, `th`, `fil` or `da` |
| `start_icon` | Selkies logo | Icon name from the theme, or the path of a PNG, JPEG or SVG, for the start button |
| `background_style` | `solid` | `solid` or `gradient`, painted first |
| `background` | `#000000` | Color of the solid background or the gradient |
| `wallpaper` | first of `~/.config/selkies-desktop/wallpaper.{png,jpg}`, `/usr/share/selkies/wallpaper.{png,jpg}`, the Selkies logo | PNG or JPEG drawn over the background; `none` for no image |
| `wallpaper_mode` | `fill` (`center` for the logo) | `fill` (cover, cropped), `fit`, `center` or `stretch` |
| `clock_format` | `%H:%M` | strftime pattern of the panel clock |
| `show_clock` | `true` | Show the clock and its calendar |
| `show_volume` | `true` | Show the volume control when PulseAudio answers |
| `show_desktop_button` | `true` | Show the strip at the right end of the bar |
| `favorites` | | Comma separated desktop file IDs (`foot`, `firefox-esr.desktop`) pinned at the top of the menu |
| `background_style_2`, `background_2`, `wallpaper_2`, `wallpaper_mode_2` | | The same four keys with a `_N` suffix override them for monitor N, numbered from 1 with the primary first |
| `launchers` | | Comma separated desktop file IDs shown as quick launch buttons beside the start button |
| `show_tray` | `true` | Host the system tray beside the clock for the applications that dock an icon |
| `files_view` | `icons` | `icons` or `list`, how new file manager windows start |
| `files_sort` | `name` | `name`, `size`, `modified` or `type` |
| `files_hidden` | `false` | Whether new file manager windows show hidden files |
| `track_recent` | `true` | Keep a recently launched list in `~/.local/state/selkies-desktop/recent` |
| `files_manager` | `true` | Offer the built-in file manager: write its desktop entry to `~/.local/share/applications`, make it the folder handler in `~/.config/mimeapps.list` when none is set, and pin it as the default favorite |
| `icon_theme` | GTK setting, else Papirus, breeze or Adwaita | Icon theme name; hicolor is always the fallback |
| `terminal` | `$TERMINAL`, else the first of foot, xterm, st, ... | Command that runs `Terminal=true` entries |

## Talking to the running panel

A second invocation sends a command over the control socket in
`$XDG_RUNTIME_DIR`, so a compositor keybinding can open the menu:

```bash
selkies-desktop --toggle-menu     # open or close the start menu
selkies-desktop --show-desktop    # minimize every window, or restore them
selkies-desktop --files [DIR]     # open the file manager, on a folder when given
selkies-desktop --reload          # read the applications and settings again
selkies-desktop --quit
selkies-desktop --lookup-icon firefox   # print the file an icon name resolves to
```

For labwc, in `rc.xml`:

```xml
<keybind key="Super_L"><action name="Execute" command="selkies-desktop --toggle-menu" /></keybind>
```

Touching `~/.config/panel-reload`, the trigger earlier versions used, still
forces a reload.

## Releases

This repository builds and publishes the binary; the images that run it live
in [docker-baseimage-selkies](https://github.com/linuxserver/docker-baseimage-selkies).
Every release on GitHub carries `selkies-desktop-x86_64` and
`selkies-desktop-aarch64`, static musl binaries with a `.sha256` file beside
each.  A push to `main` publishes them as a pre-release named by the short
commit hash; a `MAJOR.MINOR.PATCH` tag publishes a release whose notes are
`release-notes/<tag>.md`, with `Cargo.toml` carrying the same version.

An image installs the desktop by placing the binary at
`/usr/bin/selkies-desktop`.  It links no system library and speaks Wayland
and X11 over its own transports, so one build serves every distribution.  At
runtime it reads what a desktop image already has: the system fonts, an icon
theme with hicolor, the shared-mime-info database for the file manager, the
desktop files under `XDG_DATA_DIRS`, `pactl` for the volume control and a
terminal for `Terminal=true` entries.

## Building

```bash
cargo build --release                                            # target/release/selkies-desktop
cargo build --release --target x86_64-unknown-linux-musl         # static, as released
cargo build --release --target aarch64-unknown-linux-musl
```

`.cargo/config.toml` makes the musl targets self-contained through rustup's
CRT objects and rust-lld; no cross toolchain is needed.  `cargo test`,
`cargo clippy --all-targets -- -D warnings` and `pre-commit run --all-files`
are what the checks run.

## License

This project is licensed under the **Mozilla Public License 2.0 (MPL-2.0)**.
