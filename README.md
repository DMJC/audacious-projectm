# Audacious-ProjectM
ProjectM (Milkdrop) visualizer plugin for Audacious.

## Backend selection behavior
- Audacious now shows a single ProjectM visualizer plugin entry.
- The plugin exposes both GTK3 and Qt6 widget backends in one module.
- Audacious selects the backend automatically based on the active UI toolkit (`get_gtk_widget()` for GTK UI, `get_qt_widget()` for Qt UI).

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

The installed module is `projectm` under `.../audacious/Visualization`.
