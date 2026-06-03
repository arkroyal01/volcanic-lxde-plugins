# sonic-lxde-plugins

Small LXDE glue plugins for running [sonic-win](https://github.com/Sonic-DE/sonic-win)
(KWin/X11) as the window manager under an LXDE session — filling the gaps LXDE
leaves when its usual Openbox WM is swapped for kwin_x11.

## Contents

| Subdir | Plugin | What it does |
|---|---|---|
| `lxhotkey/` | `kwin.so` | lxhotkey WM backend for KWin. Lets LXDE's hotkey editor (`lxhotkey`) read/edit KWin's global shortcuts, clearing the *"Could not find a plugin for window manager KWin"* error. |
| `lxpanel/` | _(planned)_ | `sonic-overview`, `sonic-power`, `sonic-volume` panel widgets. |

## lxhotkey/kwin

lxhotkey loads a per-WM backend from `$libdir/lxhotkey/` and matches it against
the running WM's `_NET_WM_NAME` (case-insensitive). KWin reports `KWin`, so this
plugin registers via `FM_DEFINE_MODULE(lxhotkey, KWin)`.

It reads/writes `~/.config/kglobalshortcutsrc` (the `[kwin]` component). That file
is a **KConfig** file, not a GKeyFile — it uses double-bracket groups
(`[services][foo.desktop]`) and KConfig escaping that GLib's GKeyFile rejects — so
the backend parses and edits it **line-by-line**, touching only the `[kwin]` group
and preserving everything else verbatim.

### Known limitations
- **Apply:** `kglobalacceld` owns the live shortcut state and only re-reads the
  file at startup, so `save()` bounces it. A robust v2 would drive the
  `org.kde.kglobalaccel` D-Bus API directly (no file round-trip).
- **App-launch shortcuts** (KDE's `services`/khotkeys component) are not handled
  yet — only WM keys (`[kwin]`).

> Note: bare LXDE does not start `kglobalacceld`, so KWin's shortcuts (Alt+Tab,
> Overview, …) are inert until it runs. Add `@/usr/lib/kglobalacceld` to
> `~/.config/lxsession/LXDE/autostart`.

## Build & install

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
sudo cmake --install build      # kwin.so -> /usr/lib/lxhotkey/
```

Verify: `lxhotkey global` should list KWin shortcuts instead of the missing-plugin
error.

## License
GPL-2.0-or-later (the lxhotkey/lxpanel plugin APIs it builds against are GPL).
