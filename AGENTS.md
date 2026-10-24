# Working on this repository

selkies-desktop is the panel, start menu, taskbar and desktop layer that the
[Selkies baseimages](https://github.com/linuxserver/docker-baseimage-selkies) start over labwc or
openbox when `SELKIES_DESKTOP=true`.  It is deliberately not a desktop environment: the window
manager owns the windows, and this program owns the bar, the popups above it and the wallpaper.

Rust, edition 2024.  Rustdoc on every public item in Google style: a one-line summary, then
`# Arguments` (`* name - description`), `# Returns`, `# Errors` as applicable; types are always
explicit in signatures.  Comments describe the current code for a reader, not the change that
produced it; no inline comments, no issue numbers.  American English.

The binary stays pure Rust and static: wayland-client's and x11rb's own transports, tiny-skia,
cosmic-text and resvg for drawing, no system library, so the one musl build in `.cargo/config.toml`
serves every distribution the layer images are built from.  A new dependency that links a C library
breaks that and needs a maintainer's agreement.

## Layout

- `src/desktop.rs` holds the state and handles `event::Event`s into `event::Request`s; it knows
  nothing about display servers.
- `src/ui/` draws and hit-tests the bar (`panel.rs`), the start menu (`menu.rs`), the wallpaper and
  launchers (`background.rs`), the small popups (`popups.rs`) and the file manager windows
  (`files.rs`); sizes and colors sit in `ui/mod.rs`.
- `src/files/` is the file manager's model: listings, sorting, places, the shared clipboard and
  the per-folder watch (`mod.rs`), the mime database and application associations (`mime.rs`),
  and the worker threads that copy, move, delete and decode thumbnails (`ops.rs`).
- `src/backend/` are the two display servers behind the `Backend` trait: `wayland.rs` (layer shell,
  xdg-shell windows, data device drags, foreign toplevel, shm buffers) and `x11.rs` (dock, desktop
  and ordinary windows, shape extension, EWMH, pointer-grab drags).
- `src/i18n.rs` translates every string shown through `tr`/`trf`, from the `locales/*.txt` files
  compiled in; a new user-facing string goes into every locale file, and the test that keeps the
  files in step fails until it does.
- `src/entries.rs`, `src/icons.rs`, `src/xpm.rs` read desktop files and icons; `src/render.rs` is the
  canvas; `src/xkb.rs` turns keycodes into keys; `src/audio.rs`, `src/ipc.rs`, `src/watch.rs`,
  `src/tray.rs` (the XEmbed system tray host on the session's X display, XWayland under labwc) and
  `src/clock.rs` are the side channels the main loop polls.

## Testing

`cargo test`, `cargo clippy --all-targets -- -D warnings` and `pre-commit run --all-files` are the
floor.  A feature lands with unit tests for the logic that can be tested without a display:
parsing and clamping of its settings, the text or bitmaps it generates, layout and geometry
arithmetic, and any edit that is applied repeatedly, which must settle after one pass so the
desktop does not rewrite files or signal the window manager on every config write.  Presets are
tested as bundles: each must match a config built from its own changes and nothing else.  A test
in `src/i18n.rs` scans the sources for every `tr` and `trf` key and fails until each has a line in
every locale file, so new labels go into all of `locales/*.txt` in the same change.  What needs a
compositor, a window manager or a screenshot is checked in a container as described below and
is reported as such, not left untested in silence.  This repository builds and publishes the binary and holds no Docker build logic; the
images that run it live in docker-baseimage-selkies.  Everything visible is tested in a container
from an `lsiobase/selkies` image (`debiantrixie`, `arch`, `alpine324`) with the static musl build
copied over `/usr/bin/selkies-desktop`, one with `PIXELFLUX_WAYLAND=true` for labwc and one without
for openbox, both with `SELKIES_DESKTOP=false` so the binary under test is started by hand with
`SELKIES_DESKTOP_DEBUG=1` and its log kept:

- Wayland: `grim` screenshots the labwc output, `wtype` types, and `tests/driver` (its own crate,
  `cargo build --release --target x86_64-unknown-linux-musl` there) holds a virtual pointer open
  and takes `move X Y`, `click`, `scroll`, `sleep`, `shot FILE` on stdin.  `wlrctl` destroys its
  pointer after each command, which clears every hover state, so it cannot show tooltips.
- X11: `xdotool` drives, `xwd -root | convert xwd:- out.png` screenshots.
- Neither image ships `grim` or `imagemagick`; `apt-get update` then `apt-get install grim` or
  `apt-get install imagemagick` in the running container first.  A file manager window opens with
  `selkies-desktop --files DIR` through the control socket.  Under labwc XWayland listens on
  `DISPLAY=:0`, under openbox the server is `:1`; the tray needs that variable.  `tests/trayicon`
  (its own crate, built like the driver) docks a colored square through XEmbed and prints the
  clicks it receives, which is how the tray is checked.

A bind-mounted binary keeps its inode when cargo rebuilds, so copy a new build in with
`docker cp` or restart the container.  Every claim in a report has been run that way, and anything
that could not be run says so.

Releases: a push to `main` publishes the static binaries to a pre-release named by the short commit
hash; a `MAJOR.MINOR.PATCH` tag releases with `release-notes/<tag>.md` as its body, and `Cargo.toml`
carries the same version as the tag (a `-rc1` style pre-release carries the version it leads to, and
its notes by default).  Releases are cut by running the
[Release](https://github.com/selkies-project/selkies-desktop/actions/workflows/release.yml) workflow
from the Actions tab, **Run workflow** with **Use workflow from** on `main` and the tag, or by
pushing the tag.  A run releases the commit it started from: a new tag is created on it once the
checks pass, and a tag that already exists has to be on it, so an earlier commit's tag is released
by a run started from that tag.  The `latest` input decides the "Latest" badge: `auto` gives it to a
release and not to a pre-release, `never` holds it back for a fix to an older series, and `always`
gives it to a pre-release as well.  The release workflow refuses a tag whose notes are missing or
whose version differs, so a release commit bumps `Cargo.toml` and adds the notes together.  The
notes are kept as the work happens, not written at release time: every change a user can see, a
feature, a setting, a fix or a removal, adds or amends a line in the notes of the version in
`Cargo.toml` in the same commit, in the same plain prose as the existing files and the README,
naming the setting keys it adds.  Once a version is tagged its notes are frozen; the next
user-visible change bumps `Cargo.toml` and starts `release-notes/<new version>.md`.  The assets are
`selkies-desktop-x86_64` and `selkies-desktop-aarch64`, each with a `.sha256`; the images download
them, nothing is built downstream.
