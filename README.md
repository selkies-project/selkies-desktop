# Selkies Desktop

> **Notice:** Selkies Desktop is **not** intended for general consumption. It is 
> not aiming to be a general-purpose Desktop Environment (DE) for people to use 
> outside of its specific ecosystem.

Selkies Desktop is a dead-simple, purpose-built Wayland panel designed 
specifically to run inside **Selkies containers** on top of the **labwc** 
compositor. 

Rather than being a highly configurable DE, this project intentionally utilizes 
hardcoded paths, fixed sizes, and baked-in styling to perfectly fit its intended 
environment. Its primary goal is to expand upon `labwc`'s default right-click 
menu by providing a more traditional desktop experience, including:
* A categorized application launcher (Start Menu) reading from standard `.desktop` files.
* A persistent bottom panel and taskbar for better visual window management.
* A basic desktop background/wallpaper layer.
* Support for displaying and launching desktop icons mapped from `~/Desktop`.

## Reloading the UI

Selkies Desktop sets up an `inotify` watch to detect when it should refresh its 
internal application lists and redraw the UI. If you install a new application 
or modify the files in `~/Desktop`, you can force a reload by touching the 
following trigger file:

```bash
touch ~/.config/panel-reload
```

## Building

Install deps:

```bash
sudo apt-get update
sudo apt-get install libcairo2-dev libwayland-dev wayland-protocols curl
```

Compile from source:

```bash
git clone https://github.com/selkies-project/selkies-desktop.git
cd selkies-desktop
make
```

Run: 

```bash
./selkies-desktop
```

## License

This project is licensed under the **Mozilla Public License 2.0 (MPL-2.0)**.
