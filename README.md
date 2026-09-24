# Rear View Mirror

A selective picture-in-picture for Windows. Pick a window, drag a rectangle over
the part you care about, and that slice is redrawn in a small always-on-top
window — live, at the source's own frame rate.

Useful for keeping an eye on a build log, a chat pane, a health dashboard or a
teammate's video tile while you work in something else fullscreen.

## Using it

Launch `RearViewMirror.exe`. On a first run it goes straight into pick mode:

1. **Move the mouse** — the window under the cursor is outlined. **Click** it.
2. The whole window is framed. **Click** to take all of it, or **drag a
   rectangle** over just the part you want. `Esc` cancels.
3. The mirror appears in the top-right of that window's monitor.

The app then lives in the notification area; it has no main window. On later
runs it restores the mirrors you had open instead of asking for a new one.

### Mirror window

| Action | Result |
| --- | --- |
| Drag anywhere inside | Move it |
| Drag an edge or corner | Scale it, proportions locked, with a detent at 100% |
| Hold `Ctrl` while resizing | Slide past the 100% detent |
| Double-click | Snap back to 100%, actual size |
| Right-click | Reselect region, zoom, opacity, resize behaviour, aspect lock, click-through, hide, close |

### Hotkeys

| Key | Action |
| --- | --- |
| `Ctrl+Alt+M` | New mirror |
| `Ctrl+Alt+T` | Click-through on for every mirror, or off for every mirror if all already are |
| `Ctrl+Alt+X` | Close all mirrors and forget them |

## Resizing a mirror

Resizing a mirror **scales** it. The region you selected is fixed, so making the
window bigger magnifies that slice rather than revealing more of the source
around it. Proportions are locked to the region's own aspect ratio, so the image
never stretches.

As the drag passes through **100%** — one mirrored pixel per source pixel, where
the image is at its sharpest because no resampling happens — it sticks there and
the border flashes white. Hold `Ctrl` while dragging to slide straight past it.
Double-clicking a mirror jumps back to 100% outright.

Corner drags project the cursor onto the aspect line rather than following a
single axis, so diagonal movement tracks properly and shrinking feels the same
as growing.

Turning off **Lock aspect ratio** lets the image stretch; the 100% detent still
applies, but to each axis independently.

## When the source window is resized

A mirror's region is stored against the size the source was when you picked it,
so there are two sensible things to do when that window later changes size.
Both are on the mirror's right-click menu, under **When source resizes**:

- **Stay at fixed offset** (default) — the region keeps the same pixel offset
  from the source's top-left. Right for a toolbar, a sidebar, a status strip:
  anything pinned to a corner.
- **Scale with the window** — the region scales with the source, so it stays
  over the same *content*. Right for a video, a chart, a map pane: anything
  that reflows to fill its window.

The mapping happens on the GPU as part of the normal draw, so neither mode
costs anything extra.

## Remembering your setup

Mirrors are saved to `%APPDATA%\RearViewMirror\mirrors.ini` whenever anything
changes — created, moved, resized, closed, or reconfigured. Saves are coalesced,
so a hotkey that touches every mirror writes once, and each write goes to a temp
file that is renamed over the old one, so a crash mid-save leaves the previous
file intact rather than a truncated one. The file is UTF-16 with a BOM, so
titles in any script round-trip exactly.

Since window handles don't survive a restart, each mirror records the source's
executable, window class and title, and is re-bound at launch to the best live
match. The executable has to agree *and* either the class or the title has to
agree too — a bare executable match would bind a mirror to whichever of that
app's windows happened to be frontmost. A title that shares a long enough prefix
still counts, so a mirror survives its source being retitled by a file change or
an unread badge, and two saved mirrors are never bound to the same window.

If a source app isn't running yet, its mirror isn't lost. The app re-checks with
backoff — 2 s, 4 s, then every 8 s for as long as anything is waiting — which
covers the case where Rear View Mirror is in your Startup folder and wins the
race against the programs it watches. A mirror whose window is found but cannot
be captured at that instant is kept and retried too. One window enumeration
every 8 s is the whole cost of waiting.

The same holds while the app is running. **Close a source window and its mirror
doesn't vanish** — it goes quiet, keeps its place and every setting, and shows
*waiting for its window* in the manager. Reopen the app and the mirror rebinds
and reappears at once: reopening brings the new window to the foreground, and
that single system event triggers an immediate rebind, with the poll only as a
backstop. Only *Close*, *Remove* and **Close and forget all**
(`Ctrl+Alt+X`) actually discard one.

The file is plain INI and safe to hand-edit or delete; every size in it is
clamped to what the graphics device can actually create.

**Click-through** is a per-mirror setting: a click-through mirror ignores the
mouse, so you can click straight through it as though it weren't there. Turn it
on from the mirror's right-click menu or its card in the manager. Since a
click-through mirror can no longer be right-clicked, the manager card, the tray
item *Click-through, all mirrors* and `Ctrl+Alt+T` are the ways back out.

**Hidden** is for mirrors that exist only to be streamed: the window goes away
but the capture carries on, so the stream keeps flowing and nothing is drawn or
presented locally. *Hide* is on the right-click menu;
the manager card's *Hide* button toggles it, and the tray list marks a
hidden mirror as such. This is different from switching a mirror off, which
stops the capture entirely.

Both settings are remembered between runs.

## The manager window

**Double-click the tray icon** (or pick *Manage mirrors…*) to open the manager:
a resizable window with a card per mirror, each carrying its own controls.

| Control | Effect |
| --- | --- |
| **Switch** | Turn that mirror off or back on |
| **Opacity** slider | 5% – 100%, live as you drag |
| **Size** slider | 25% – 300%, with a detent at 100% |
| **Region…** | Pick a different slice of the same window |
| **Remove** | Delete just that mirror |
| **Click-through** | Let the mouse pass through that mirror; fills in when on |
| **Hide** | Keep capturing and streaming with no window on screen; fills in when on |
| **Click-through all** | Header button: click-through on for every mirror, or off for all |
| **Streaming** | Header button: opens the streaming settings, where the server is started and stopped; fills in while running, with the client count |
| **New mirror** | Start picking another, from the header |

The sliders drive the mirrors live while you drag, and the value is written to
the settings file once on release rather than on every pixel of movement. The
list scrolls when it outgrows the window, and closing the manager leaves the
mirrors running — it's a control panel, not the app itself.

## The quick list

For a faster path, the tray icon's **Mirrors** entry is a plain list, one line
per mirror, labelled with its source window's title and check-marked when it is
switched on.

- **Click** a line to switch that mirror off or back on.
- **Right-click** a line to remove it, after a confirmation.

Switching a mirror off is not the same as removing it. It stops capturing
entirely and hides the window, so it costs nothing at all while off, but it
keeps its place in the list along with its region, size, position and settings —
ready to switch straight back on. That state is saved too, so a mirror you left
switched off comes back switched off, and it still appears in the list even if
its source application isn't running. Switching it on re-binds it to the source
window, including one that has been closed and reopened since.

You can run several mirrors at once — launching the app again just starts
another pick instead of a second copy.

## Streaming to another machine

Any mirror can be watched live from another PC. The app is the server; a
separate `RearViewMirrorClient.exe` connects to it and shows the mirrors you
pick in a grid.

**On the machine with the mirrors:** open *Streaming* from the tray menu or the
manager's header. Choose a UDP port, press *Generate* for a shared key (or type
your own, 8+ characters), and press *Start*. Forward that UDP port on your
router to this PC. The top line shows whether the server is running and how
many clients are connected, live; *Stop* ends it. A running server starts again
at the next launch. The dialog names the hardware encoder it found; if there is
none, streaming is unavailable.

*Bitrate* and *Frame rate* apply per mirror. The frame rate is a ceiling, 1 to
240 fps: frames beyond it are skipped on an even cadence, a new viewer's first
frame is never held back, and when the source stops changing its final picture
is still sent. A still window sends far fewer frames than the ceiling, and no
stream can beat the refresh rate of the monitor the source window is on.
Changing a setting while the server runs restarts it; clients reconnect on
their own.

**On the other machine:** run the client and press *Add server…*: enter the
host (or public address), the port and the same key. The sidebar lists each
server with its mirrors underneath; click a mirror to put it on the canvas,
click again to take it off. Several servers can be added; `✕` next to a server
disconnects and forgets it. The `‹` button (or `Tab`) collapses the sidebar to
a thin handle so the canvas gets the whole window.

**The canvas** is free-form. Each mirror is a box on it: drag the box to move
it, drag an edge or corner to resize it, to any position and size. Edges snap
when they come within a few pixels of the canvas edge or another box's edge,
either flush or with a small gap, and a thin line shows what they lined up
with; hold `Alt` to place without snapping. Boxes may overlap; the one you last
touched is on top. Boxes keep their size when the window grows, and are pulled
in when it shrinks. A box's name shows while the mouse is over it and for a
couple of seconds after, then fades, so the picture is left clean. Double-click a box to show it alone, `Esc` to go back.
Right-click a box for *Size to stream*, *Bring to front*, *Send to back*,
*Remove from the canvas*, and:

**Pop out.** A box can leave the canvas and become its own window, behaving
exactly like a local mirror: borderless, always on top, drag to move, edge-resize
with locked proportions and a detent at 100%, double-click for 100%, right-click
for zoom, opacity, click-through and *Return to grid*. The box stays on the
canvas as a placeholder so it returns to the same spot; click the placeholder or
close the window to bring it back. A click-through pop-out cannot be
right-clicked, so its placeholder's menu carries the same toggle.

Everything is remembered in `client.ini`: servers and keys, the sidebar state,
every box's cells, and each pop-out's placement, opacity and settings. The
client reconnects to every remembered server at launch, after a timeout, or
after the other machine restarts the app, and puts the mirrors you had chosen
back where they were. A box whose mirror is temporarily missing on the server
(its window is closed, say) waits in the file and returns when the mirror does.

How it stays fast and light:

- **Zero-copy into the encoder.** The mirror already has the source frame as a
  GPU texture. The streaming tee crops it into an NV12 texture with one video-
  processor blit and hands that to the GPU's own H.264 encoder through Media
  Foundation (NVENC, QuickSync or AMF — whichever the machine has). Nothing is
  read back to the CPU except the compressed bitstream, a few hundred KB/s.
- **Encode once, however many watch.** Each mirror has one encoder shared by
  every subscribed client; frames are only encoded while someone is watching.
- **UDP with targeted recovery.** Frames are split into ≤1200-byte datagrams so
  nothing fragments. A lost packet only delays the frame it belonged to: the
  client asks for exactly the missing pieces, and if a frame is hopeless it
  asks for a fresh keyframe rather than stalling every stream, which is what TCP
  would do.
- **Latest frame wins.** Between capture and encoder the frames are triple-
  buffered; if the encoder is momentarily behind, the older frame is skipped
  rather than queued. The client does the same on its decode queue. Nothing in
  the path is allowed to build a backlog.
- **Capture is not throttled.** Windows limits a capture session to one frame
  every 16 ms by default, about 60 fps. The app lifts that to 1 ms, so a fast
  source is mirrored and streamed at its own rate up to the display's refresh.
- **Nothing polls.** The hardware encoder announces when it wants a frame and
  when one is finished, and the encode thread sleeps until one of those or a
  new capture wakes it. A frame costs only the encoder's own time, about a
  millisecond, so the frame rate is set by the source and the Frame rate
  ceiling, not by the system timer.
- **Decode straight to the GPU.** The client decodes with DXVA into textures on
  its own device and draws them with Direct2D; the grid is composited, never
  copied.

Security: everything on the wire is AES-256-GCM under a key derived from the
shared passphrase (PBKDF2, 100k rounds), with per-session keys from a two-random
handshake, per-direction counters as nonces, and a replay window. Anyone probing
the forwarded port sees only ciphertext and gets no reply. A client takes a slot
only after its first message under the session key, so a captured handshake
replayed later gets nowhere. What even a key-holding client can ask of the
server is bounded: subscriptions only to listed mirrors and at most 64 each,
keyframes and frame requests at most four a second per mirror, retransmits
within a per-client budget, and the log file capped at 16 MB. The passphrase is
stored DPAPI-protected to your Windows account on both ends.

## How it works

The pipeline never touches the CPU or leaves the GPU:

1. **Windows Graphics Capture** (`Direct3D11CaptureFramePool`, free-threaded)
   hands over each new frame of the source window as a D3D11 texture.
2. The frame is copied once into a cache texture, so a repaint after a resize or
   an opacity change doesn't need to wait for the source to move again.
3. The crop is applied on the GPU — as a UV rectangle when scaling up, or via a
   mipped blit when scaling down, which keeps heavy minification from
   shimmering.
4. A pixel shader applies opacity plus an antialiased rounded-rect mask and
   presents to a **DirectComposition** swapchain on a
   `WS_EX_NOREDIRECTIONBITMAP` window, so the corners blend properly against
   whatever is behind them.

### Threading

One D3D11 device is shared by everything, and two kinds of thread draw on it:
capture threads render the mirrors, while the UI thread renders the manager and
the overlays through Direct2D. `ID3D11Multithread` is *not* sufficient here — it
makes individual calls atomic, but a mirror's ~20-call state-setting sequence
landing inside a Direct2D `BeginDraw`/`EndDraw` pair still clobbers its render
target, viewport and shaders, blanking the window. `Gfx::deviceMutex` is held
across each complete draw instead.

The lock order is **capture state → renderer → device**, never the reverse, and
`Present` happens outside all three:

- `WindowCapture` invokes the frame callback while holding its own state lock,
  and `Stop()` takes that same lock — so `Stop()` cannot return, and the owner
  cannot be destroyed, while a frame is still inside the callback.
- The renderer draws under its own lock plus the device lock, then releases
  both before presenting, so a vsync wait never blocks the UI thread or another
  mirror's frame. `Present` and `ResizeBuffers` are serialised against each
  other by a small dedicated mutex and nothing else.
- The manager's Direct2D draw runs under the device lock and touches **only a
  snapshot** of the mirror list, built in `PrepareDraw` before the lock is
  taken. Nothing inside `OnDraw` calls into a `Mirror`; that is what makes the
  ordering impossible to invert by accident.

Mirror objects are destroyed lazily and addressed by a stable id, never by
position. A context menu, the region selector and a confirmation box all run
nested message loops from inside a `Mirror` method, during which the source
window can close and the mirror be removed — even the app's own `WM_DESTROY`
can arrive there. The object is torn down at once but freed only when the outer
message loop next turns, and every menu command, hit-test and slider drag
resolves its mirror by id at the moment it acts. The nested loops themselves
re-post `WM_QUIT` rather than swallowing it, so quitting from inside one still
ends the outer loop.

Exceptions never cross a window procedure. The shared `WndProcThunk` stops them
at the boundary, because a C++ exception cannot unwind through the kernel
callback that invokes a window procedure and would take the process down; the
graphics paths return failure instead of throwing, so a lost device degrades to
a mirror that stops updating rather than a crash.

One wrinkle worth recording: making a mirror click-through needs
`WS_EX_TRANSPARENT` **and** `WS_EX_LAYERED`. The transparent style alone leaves
the hit test landing on the mirror, no matter how the frame is invalidated.
Layering a `WS_EX_NOREDIRECTIONBITMAP` window sounds like it ought to blank the
composition content, but it doesn't — the DirectComposition visual keeps
rendering, and `SetLayeredWindowAttributes` is still required or the window can
come up empty.

Two consequences worth knowing:

- **Idle cost is ~zero.** There is no render loop. WGC only delivers a frame
  when the source window actually changes, so mirroring a static pane costs
  nothing until it moves.
- **Coordinates are physical pixels throughout.** The process is
  per-monitor-DPI-aware v2, so capture textures, window rects and the selection
  overlay all agree without scaling fixups on mixed-DPI setups.

The picker and region selector are Direct2D drawn onto the same shared D3D11
device, and the picker uses low-level mouse and keyboard hooks so it can
highlight and select a window without the click ever reaching that application.

## Building

Needs MSVC (Visual Studio 2022 Build Tools with the C++ workload) and the
Windows 10/11 SDK. CMake and Ninja are used for the build.

```
build.bat
```

The result is `build\RearViewMirror.exe` (the app) and
`build\RearViewMirrorClient.exe` (the streaming client). Both are self-contained
executables with no runtime dependencies beyond Windows itself.

Both are Authenticode-signed after linking. The first build creates a
self-signed code-signing certificate, `CN=Rear View Mirror`, in your personal
certificate store (`Cert:\CurrentUser\My`) and every later build signs with the
same one; nothing is stored in the repository. Signatures are timestamped when
the network allows. Windows does not trust a self-signed certificate by itself:
`tools\sign.ps1 -Trust` adds it to your user's trusted roots and publishers so
the files verify as a known publisher on this machine, `tools\sign.ps1 -Remove`
deletes it again, and `-DRVM_SIGN=OFF` on the CMake line skips signing.
`build\rvmnet_test.exe` exercises the streaming stack headlessly, including a
GPU encode/decode round trip and a full server-to-client loopback.

## Requirements

Windows 10 version 1903 or newer, for `Direct3D11CaptureFramePool`. Borderless
capture and cursor suppression are applied where the OS supports them and
skipped silently where it doesn't.

## Layout

| File | Role |
| --- | --- |
| `src/gfx.*` | Shared D3D11/D2D/DWrite device and the composition surface |
| `src/capture.*` | Windows Graphics Capture session, frames as GPU textures |
| `src/renderer.*` | Crop, mip, shade and present a mirror |
| `src/mirror.*` | The floating always-on-top window and its menu |
| `src/manager.*` | The manager window: cards, sliders, switches, buttons |
| `src/picker.*` | Hover-highlight, click-to-choose a window |
| `src/region.*` | Dimmed drag-a-rectangle overlay |
| `src/overlay.*` | Direct2D overlay window shared by the two above |
| `src/persist.*` | Saved-mirror file and window re-matching |
| `src/app.*` | Tray icon, hotkeys, restore-on-launch, mirror lifecycle |
| `src/app.rc` | Icon, version and dialog resources |
| `src/net/crypto.*`, `channel.*` | PBKDF2/HMAC key derivation, AES-GCM, replay-protected datagrams |
| `src/net/udp.*`, `packetizer.*` | Dual-stack UDP socket; frame splitting, reassembly, NACK |
| `src/net/codec.*`, `converter.*` | Media Foundation H.264 encode/decode; D3D11 colour conversion |
| `src/stream_server.*` | Serves mirrors: encoders, sessions, retransmits |
| `src/stream_settings.*` | Streaming settings file and dialog |
| `client/` | The client: connection, decode, canvas window, pop-out windows, settings |
| `tests/net_test.cpp` | Headless tests for all of the above |
