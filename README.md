# Audacious-ProjectM
ProjectM (Milkdrop) visualizer plugins for Audacious.

## What changed
- Added a Qt6 visualizer plugin build (`projectm-qt6`).
- Kept the existing GTK3 visualizer plugin build (`projectm-gtk`).
- Updated the build/loader output so Audacious can load either GTK3 or Qt6 plugin modules from the same Visualization plugin directory.

## Requirements
- [LibProjectM4](https://github.com/projectM-visualizer/projectm/releases/tag/v4.1.6)
- libaudacious-dev
- libgtk+-3.0-dev
- libepoxy-dev
- qt6-base-dev

## Build
```bash
meson setup build
meson compile -C build
meson install -C build
```

Audacious will detect both plugin modules (`projectm-gtk` and `projectm-qt6`) under `.../audacious/Visualization`.
