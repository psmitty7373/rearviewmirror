# Rear View Mirror

Mirror any part of any window into a small always-on-top view, and stream those
mirrors live to other PCs. Windows 10 (1903) or newer.

## Quick start

1. Run `RearViewMirror.exe`. It starts in pick mode.
2. Hover a window (it is outlined) and **click** it.
3. **Click** again for the whole window, or **drag** a rectangle for part of it.
   `Esc` cancels.

The mirror appears top-right. The app lives in the notification area and
restores your mirrors on the next launch.

## Mirror window

| Action | Result |
| --- | --- |
| Drag inside | Move |
| Drag an edge or corner | Scale (proportions locked, sticks at 100%) |
| `Ctrl` while resizing | Skip the 100% stop |
| Double-click | Back to 100% |
| Right-click | Region, zoom, opacity, resize behaviour, aspect lock, click-through, hide, close |

**When source resizes** (right-click): *Stay at fixed offset* suits toolbars and
side panels; *Scale with the window* suits videos and charts that reflow.

**Click-through** lets clicks pass straight through a mirror. Undo it from the
manager, the tray, or `Ctrl+Alt+T`.

**Hide** removes the window but keeps capturing, for mirrors that are only
streamed. **Off** (manager switch or tray list) stops capturing entirely.

## Hotkeys

| Key | Action |
| --- | --- |
| `Ctrl+Alt+M` | New mirror |
| `Ctrl+Alt+T` | Click-through on for all mirrors, or off if all are on |
| `Ctrl+Alt+X` | Close all mirrors and forget them |

## Tray and manager

- **Double-click the tray icon** for the manager: one card per mirror with an
  on/off switch, opacity and size sliders, *Region…*, *Remove*,
  *Click-through* and *Hide*. The header has *Streaming*, *Click-through all*
  and *New mirror*.
- **Right-click the tray icon** for the menu. Its *Mirrors* list toggles a
  mirror on or off with a click, and removes it with a right-click.

If a mirror's source window closes, the mirror waits and comes back when the
window reopens. Only *Close*, *Remove* and `Ctrl+Alt+X` discard a mirror.

## Streaming

**On the PC with the mirrors:** open *Streaming* (tray menu or manager header).

1. Pick a UDP port and press *Generate* for a shared key.
2. Set *Bitrate* and *Frame rate* (an upper limit, 1 to 240 fps).
3. Press *Start*. Forward the UDP port on your router to this PC, and allow the
   app through Windows Firewall when asked.

The dialog shows whether the server is running and how many clients are
connected. A running server starts again at the next launch. Streaming needs a
hardware H.264 encoder (NVIDIA, Intel or AMD); the dialog names the one it
found.

**On the viewing PC:** run `RearViewMirrorClient.exe`, press *Add server…*, and
enter the host or public address, the port and the same key. Click a mirror in
the sidebar to add it to the canvas; click again to remove it. You can add
several servers.

| Canvas action | Result |
| --- | --- |
| Drag a box / its edge | Move / resize (edges snap; hold `Alt` to not snap) |
| Double-click a box | Show it alone; `Esc` to go back |
| Right-click a box | Pop out, size to stream, bring to front, send to back, remove |
| `Tab` or `‹` | Hide or show the sidebar |

**Pop out** turns a box into its own always-on-top window that behaves like a
local mirror. Close it, or click its placeholder, to return it to the canvas.

The client remembers servers, keys and layout, and reconnects on its own.

**Frame rate:** a stream can't exceed the refresh rate of the monitor showing
the source window, and a window that isn't changing sends few frames. For high
frame rates, hide the local mirror and raise the bitrate.

## Building

Needs Visual Studio 2022 Build Tools (C++ workload) and the Windows 10/11 SDK.

```
build.bat
```

This produces `build\RearViewMirror.exe` and `build\RearViewMirrorClient.exe`,
both self-contained. They are signed with a self-signed certificate that the
first build creates in your certificate store. Run
`powershell -File tools\sign.ps1 -Trust` to have this PC treat it as a trusted
publisher, or build with `-DRVM_SIGN=OFF` to skip signing.

## Files

Everything lives in `%APPDATA%\RearViewMirror`. Keys are encrypted to your
Windows account.

| File | Contents |
| --- | --- |
| `mirrors.ini` | Saved mirrors |
| `stream.ini` | Streaming settings and key |
| `client.ini` | Client servers, keys and layout |
| `server.log`, `client.log` | Diagnostics |

Internals, design notes and tests are in [DEVELOPMENT.md](DEVELOPMENT.md).
