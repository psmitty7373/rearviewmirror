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

## Desktop

To mirror the whole desktop, point at the wallpaper or the taskbar while
picking, so every screen lights up, and click. Pressing `D` while picking does
the same. Then **click** for every monitor, or **drag** a rectangle for one
monitor or part of the screen.

A desktop mirror streams all of it, pointer included, but has no window of its
own. To see one here, turn off *Hide* on its manager card. After you add,
remove or resize a monitor, it picks up the new layout by itself. Over several
monitors, the stream is scaled to at most 4096 pixels wide.

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

**Click-through** lets clicks pass straight through a mirror. It is set for each
mirror on its own; undo it from that mirror's manager card.

**Hide** removes the window but keeps capturing, for mirrors that are only
streamed. **Off** (the manager card's switch) stops capturing entirely.

## Hotkeys

| Key | Action |
| --- | --- |
| `Ctrl+Alt+M` | New mirror |
| `Ctrl+Alt+X` | Close all mirrors and forget them (asks first) |

## Tray and manager

- **Double-click the tray icon** for the manager: one card per mirror with an
  on/off switch, opacity and size sliders, *Region…*, *Remove*,
  *Click-through* and *Hide*. The header has *Streaming* and *New mirror*.
- **Right-click the tray icon** for *Manage mirrors…*, *New mirror…*, *Close
  and forget all* and *Exit*. Everything else is in the manager.

If a mirror's source window closes, the mirror waits and comes back when the
window reopens. Only *Close*, *Remove* and `Ctrl+Alt+X` discard a mirror.
Several mirrors made from one window come back on the same window together.

If the graphics driver resets, the app and the client restart themselves and
pick up where they left off.

## Streaming

**On the PC with the mirrors:** open *Streaming* in the manager's header.

1. Pick a UDP port and press *Generate* for a shared key. The key is shown so
   you can copy it; *Show* reveals or hides it.
2. Set *Bitrate*, *Frame rate* (an upper limit, 1 to 240 fps) and *Encoder
   preset*. *Fastest* uses the least GPU time and suits high frame rates;
   *Balanced* is the encoder's default.
3. Press *Start*. Forward the UDP port on your router to this PC, and allow the
   app through Windows Firewall when asked.

The dialog shows whether the server is running and how many clients are
connected. A running server starts again at the next launch. Streaming needs a
hardware H.264 encoder (NVIDIA, Intel or AMD); the dialog names the one it
found.

The app and the client must be the same version. This release changed the
protocol, so update both PCs together.

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
Starting it again brings the open client to the front.

### Desktop control (optional build)

Build both PCs with `RVM_REMOTE_CONTROL=ON` to send mouse and keyboard input to
**full-desktop mirrors**. It is off by default and requires streaming. Window
mirrors and desktop regions remain view-only. With this feature enabled, viewers
with the streaming key can control the shared desktop.

Add desktops from several servers to the canvas as usual. Right-click a desktop
tile and choose **Control desktop**. Its blue border and label show which desktop
has input focus; the tile can stay beside the other desktops, or be enlarged with
the existing double-click view before entering control. Mouse clicks, drags,
vertical/horizontal scrolling, and keyboard shortcuts go to that PC.

Press **Ctrl+Alt+F12** to release control and move or resize tiles again. Clicking
outside the desktop picture or switching away from the client also releases it.
Only one viewer controls a server at a time. Control is released on disconnect
and must be selected again after reconnecting. Pop-out windows are view-only;
return one to the canvas to control it.

Input uses the server's keyboard layout and Windows input permissions. Secure
desktops (including UAC prompts), Ctrl+Alt+Delete, and applications running at a
higher privilege level cannot be controlled by an ordinary unelevated server.

**Frame rate:** a stream can't exceed the refresh rate of the monitor showing
the source window, and a window that isn't changing sends few frames. For high
frame rates, hide the local mirror and raise the bitrate.

## Building

Needs Visual Studio 2022 (any edition, or the Build Tools) with the C++
workload, and the Windows 10/11 SDK. `build.bat` finds them itself.

```
build.bat
```

This produces `build\RearViewMirror.exe` and `build\RearViewMirrorClient.exe`,
both self-contained. They are signed with a self-signed certificate that the
first build creates in your certificate store. Run
`powershell -File tools\sign.ps1 -Trust` to have this PC treat it as a trusted
publisher, or run `build.bat --no-sign` to skip signing. Without the SDK's
signtool, the build skips signing with a warning.

For mirrors only, run `build.bat --mirrors-only`. The app then has no
streaming and no network code, the manager shows no *Streaming* button, and
desktop mirrors start with a window. The client is not built.

To enable desktop control on the server and client:

```
build.bat --remote-control
```

Run `build.bat --streaming-only` to return to viewing-only streaming. The
selection is remembered; plain `build.bat` rebuilds the selected features.
A fresh build defaults to streaming without control. Existing configuration
and target arguments still work, for example
`build.bat Debug rvmclient --remote-control`. Use `--no-pause` for unattended
builds, or `--help` to list the options. These options set the corresponding
`RVM_STREAMING`, `RVM_REMOTE_CONTROL`, and `RVM_SIGN` CMake cache variables.

Protocol version 3 adds desktop-control capabilities; update the server and client
together, including when control is disabled.

## Files

Everything lives in `%APPDATA%\RearViewMirror`. Keys are encrypted to your
Windows account, so a settings file copied to another user or PC can't unlock
them. The app then asks for the key again; in the client, remove that server
and add it again.

| File | Contents |
| --- | --- |
| `mirrors.ini` | Saved mirrors |
| `stream.ini` | Streaming settings and key |
| `client.ini` | Client servers, keys and layout |
| `server.log`, `client.log` | Diagnostics |
| `server-crash-*.dmp`, `client-crash-*.dmp` | Written if the app or client crashes |

Internals, design notes and tests are in [DEVELOPMENT.md](DEVELOPMENT.md).
