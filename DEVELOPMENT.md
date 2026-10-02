# Development notes

How Rear View Mirror works inside, and why. For using and building it, see
[README.md](README.md).

## Layout

| File | Role |
| --- | --- |
| `src/gfx.*` | Shared D3D11/D2D/DWrite device, composition surface, logging |
| `src/capture.*` | Windows Graphics Capture of a window, or of every monitor as one desktop |
| `src/renderer.*` | Crop, mip, shade and present a mirror; the streaming tee |
| `src/mirror.*` | The floating always-on-top window and its menu |
| `src/manager.*` | The manager window: cards, sliders, switches, buttons |
| `src/picker.*` | Hover-highlight, click-to-choose a window |
| `src/region.*` | Dimmed drag-a-rectangle overlay |
| `src/overlay.*` | Direct2D window base shared by the picker, region selector, manager and client |
| `src/persist.*` | Saved-mirror file and window re-matching |
| `src/app.*` | Tray icon, hotkeys, restore-on-launch, mirror lifecycle |
| `src/app.rc` | Icon, version and dialog resources |
| `src/net/crypto.*`, `channel.*` | PBKDF2/HMAC key derivation, AES-GCM, replay-protected datagrams |
| `src/net/udp.*`, `packetizer.*` | Dual-stack UDP socket; frame splitting, reassembly, NACK |
| `src/net/codec.*`, `converter.*` | Media Foundation H.264 encode (GPU or CPU) and decode; D3D11 colour conversion |
| `src/net/control.*` | Optional focused input, reliable delivery, exclusive server lease, keyboard hook |
| `client/control_ui.cpp` | Desktop control mode, focus lifetime and letterboxed pointer mapping |
| `src/streaming.*` | The app's one seam to streaming; `streaming_off.cpp` is the empty stand-in |
| `src/stream_server.*` | Serves mirrors: encoders, sessions, retransmits, limits |
| `src/stream_settings.*` | Streaming settings file and dialog |
| `client/` | Client: connection, decode, canvas, pop-out windows, settings |
| `service/` | Optional sign-in screen service: install, session supervisor, Desktop Duplication helper |
| `tests/net_test.cpp` | Headless tests for the streaming stack |
| `tools/sign.ps1` | Code-signing certificate management and signing |

## Mirror pipeline

The picture never leaves the GPU:

1. **Windows Graphics Capture** (`Direct3D11CaptureFramePool`, free-threaded)
   hands over each new frame of the source window as a D3D11 texture. Capture's
   default 16 ms minimum update interval (about 60 fps) is lowered to 1 ms, so
   fast sources run at the display's refresh rate.
2. Only the effective crop, plus a one-pixel margin that filtering at its edges
   reads, is copied into a cache texture, so a repaint after a resize or opacity
   change doesn't wait for the source to change again. A crop that grows into
   pixels never kept restarts capture, which sends a whole frame.
3. The crop is applied on the GPU: a UV rectangle when scaling up, a mipped blit
   when scaling down, which stops heavy minification from shimmering. A
   minified mirror that isn't streamed copies frames straight into the top mip
   of its crop texture instead; the cache catches up only when something needs
   it.
4. A pixel shader applies opacity and an antialiased rounded-rect mask and
   presents to a **DirectComposition** swapchain on a
   `WS_EX_NOREDIRECTIONBITMAP` window, so corners blend with what is behind.

There is no render loop. Capture delivers a frame only when the source changes,
so a static mirror costs nothing. The process is per-monitor-DPI-aware v2, so
capture textures, window rects and overlays all use physical pixels.

**Sleeping.** A hidden mirror whose frames nobody wants (no viewer watching)
stops capturing after a 3 s grace. A viewer subscribing, or showing the mirror
again, restarts capture. A desktop mirror frees its textures while asleep; a
window mirror keeps its last picture, since a minimized window sends none on
waking. Nothing reports a sleeping source closing, and its handle may be
reused, so the source's thread and process are checked again on waking.

**Desktop mirrors** use the same pipeline with a different source.
`DesktopCapture` captures every monitor separately, pointer included. Each
monitor's frame is copied straight into the renderer's cache at that monitor's
offset in the virtual screen; there is no composite texture. Monitors take
turns under the renderer's lock, so each draw or streamed frame includes every
copy before it. Gaps between monitors of different sizes stay black. WGC
can't follow a change of monitors, so `WM_DISPLAYCHANGE` restarts desktop
capture. If a restart fails mid-change, the mirror waits and retries like a
mirror whose window has gone.
The mirror's own window is excluded from capture with
`WDA_EXCLUDEFROMCAPTURE`, or showing it would capture itself over and over.
The source kind is saved as `Source=desktop`. A desktop mirror needs no window
matching, so it restores at once.

The picker and region selector are Direct2D windows on the same device. The
picker uses low-level mouse and keyboard hooks so the choosing click never
reaches the target application. The hooks only record what happened and wake
the picker's loop; finding the window under the cursor happens there, because
a low-level hook stalls every mouse event on the desktop until it returns.
Pointing at the wallpaper (`Progman`, `WorkerW`) or a taskbar
(`Shell_TrayWnd`, `Shell_SecondaryTrayWnd`) highlights the whole virtual
screen instead and picks the desktop, with the labels kept on the monitor
under the pointer so they never straddle two screens.

## Threading

One D3D11 device is shared. Capture threads render mirrors; the UI thread
renders the manager, overlays and client through Direct2D. `ID3D11Multithread`
is not enough: it makes single calls atomic, but a mirror's state-setting
sequence landing inside a Direct2D `BeginDraw`/`EndDraw` still clobbers render
target, viewport and shaders. `Gfx::deviceMutex` is held across each complete
draw.

Lock order is **capture state → renderer present → renderer → device**, never
reversed.

- `WindowCapture` copies each frame under its state lock, then draws and
  presents in an after-frame callback under a separate mutex, once the WGC
  frame is released, so a vsync wait never holds up the next copy. `Stop()`
  takes both, so a capture owner can't be destroyed mid-callback.
- The renderer's present lock covers one whole draw-and-present, or a resize.
  Otherwise a UI redraw could present between a capture thread's draw and its
  present, which would then show a back buffer nobody drew. The renderer and
  device locks are released before presenting, so a vsync wait holds up only
  that one mirror's other presents. Presents coalesce: a thread that finds
  another already waiting to draw leaves its frame to that draw.
- Direct2D windows draw only a snapshot taken outside the device lock, in
  `PrepareDraw` (the manager takes its own on `Refresh` and clicks). Nothing in
  `OnDraw` calls into a mirror, which keeps the order impossible to invert by
  accident. They present with sync interval 1, and input and frames redraw
  through `Invalidate`, which coalesces into one `WM_PAINT`. One
  DirectComposition device serves every surface.

**Starting and stopping capture.** Both are cross-process COM calls, during
which the UI thread pumps messages, and starting or stopping another capture
from one of those messages deadlocked inside WGC. So every start and stop runs
inside an `App::Transition`; a message that would start or stop a capture
while one is in progress is deferred, and posted again when the outermost
transition ends. `WindowCapture::Stop()` takes its members out before closing
them, so a nested call finds nothing half-done.

Mirrors are addressed by a stable id, never by position, and destroyed lazily.
Context menus, the region selector and message boxes run nested message loops
inside `Mirror` methods, during which the mirror can be removed. Removal takes
the mirror out of the list first, so nothing handled while it stops can find
it, tears it down at once, and frees it only when the outer loop next turns.
Nested loops re-post `WM_QUIT` rather than swallowing it.

Exceptions never cross a window procedure: `WndProcThunk` catches them, because
a C++ exception can't unwind through the kernel callback that calls it. Graphics
paths return failure rather than throw.

**Crash reports.** Release builds carry symbols (`/Zi`, `/DEBUG`), with the .pdb
files beside the executables and no change to the generated code. On a crash,
`InstallCrashHandler` logs the exception and the crashing thread's stack, with
function names and lines when the .pdb is present, and writes a small minidump
to `%APPDATA%\RearViewMirror`. It works on a fresh thread, since the crashing
thread's stack may be what is broken, and then lets Windows report the crash as
before. Log lines are not flushed one by one; the crash handler flushes the
log before the dump. Copy the .pdb along with the .exe to other machines.

**Device loss.** Every swapchain, capture pool, texture and encoder belongs to
the one device, so a removed or reset device can't be patched up in place.
Failing calls go through `Gfx::CheckDevice`, which recognises a lost device and
fires a handler once. The app or client saves, starts a fresh copy of itself
with `--after-device-loss <pid>`, and exits. The new process waits for the old
one before taking the single-instance mutex. A device lost again within 30 s of
such a restart is reported instead, so a broken driver can't cause a restart
loop. If the device can't be created with video support, it is created without
it: mirrors still work, streaming does not.

## Persistence

Mirrors are saved to `mirrors.ini` after every change, coalesced so one hotkey
that touches every mirror saves once, and the file is written only when its
content changed. Each write goes to a temp file that replaces the old one, so a
crash mid-save leaves the previous file intact. Files are UTF-16 with a BOM so
titles in any script survive, and every size read back is clamped to what the
device can create. The file holds at most 32 mirrors, waiting ones included, so
the app refuses to make a 33rd, with a message, rather than lose one on the
next save.

Window handles don't survive a restart, so each mirror records the source's
executable, class and title. At launch it binds to the best live match: the
executable must agree, and so must the class or the title. Titles are compared
without decorations like a leading "(3) " or a trailing unsaved-changes mark,
and a long shared prefix counts, so retitled windows still match. A match on
executable and class alone is taken only when exactly one window fits; with no
executable recorded, the title must match.

Mirrors made from one window share a **group** number, saved with them. A group
binds to one window together, and different groups never share a window, so
two mirrors of one app's two windows don't collapse onto the same one. Files
from before groups get one derived from each mirror's recorded identity.

Unmatched mirrors wait. The app re-checks at 2 s, 4 s, then every 8 s, and also
the moment any window comes to the foreground, which is what reopening an app
does. That foreground hook wakes the app on every switch system-wide, so it
exists only while something waits. A mirror whose source closes while running becomes an orphan and rebinds
the same way.

## Streaming

The app is the server; `RearViewMirrorClient.exe` is the client. One UDP port
carries everything.

**One seam.** The app reaches streaming only through the `Streaming` class. It
is told when a mirror is created, to attach a frame sink and a cheap "wanted"
probe that the renderer asks on every frame, and when mirrors change, to
republish the list, which goes out only if it changed. It posts requests for a
picture back to the app window, and it runs the settings dialog. Mirrors and
the renderer know nothing of streaming: a mirror offers a generic frame sink
and a way to resend its last frame. `-DRVM_STREAMING=OFF` compiles `streaming_off.cpp`, which
does nothing and reports streaming unavailable, and leaves out the libraries,
the client and the tests. The streaming seam needs no conditional compilation
at its call sites.

| Library | Contents | Used by |
| --- | --- | --- |
| `rvmcore` | Device, overlays, persistence, logging | Everything |
| `rvmnet` | Crypto, UDP, packetizer, codec, converter | Both ends |
| `rvmserver` | `stream_server.cpp` | App, tests |
| `rvmclientnet` | `client/stream_client.cpp` | Client, tests |

- **Zero-copy into the encoder.** The renderer's cache texture is teed to the
  server, which crops it into NV12 with one video-processor blit and hands it to
  the GPU's H.264 encoder through Media Foundation (NVENC, Quick Sync or AMF).
  Only the compressed bitstream reaches the CPU. (Without a usable hardware
  encoder, frames are read back and encoded on the CPU instead; see
  [Encoding on the CPU](#encoding-on-the-cpu).)
- **One encoder per mirror**, shared by every client watching it; nothing is
  encoded while nobody watches, and unwatched streams are freed.
- **Event-driven encoding.** The hardware encoder is asynchronous. Its "want
  input" and "have output" events arrive through an `IMFAsyncCallback` relay
  that queues them and wakes the encode thread. Nothing polls, so a frame costs
  only the encoder's own time (about 1 ms), independent of the system timer's
  15.6 ms tick.
- **Asleep when idle.** The encode thread sleeps until a frame, an encoder
  event or a viewer leaving wakes it, or until its earliest real deadline; the
  network thread until a datagram, a wake from another thread, or its earliest
  deadline. An idle server wakes zero times. The client's network thread waits
  the same way (`UdpSocket::Wait`/`Wake`).
- **Packetised once.** Each encoded frame is split into packets once, in one
  shared buffer, then sealed and sent outside the stream's lock. NACKs are
  answered from that same frame, without copies.
- **Latest frame wins.** Frames pass through five texture slots between capture
  and encoder. Each frame goes in as a tracked sample, so the encoder's own
  release of the sample says when its slot is free again. If the encoder is
  behind, older frames are dropped, never queued. The client does the same on
  its decode queue.
- **Frame-rate ceiling.** Frames beyond the configured rate are skipped on an
  even cadence. A frame a new viewer or keyframe is waiting for always passes,
  and if the source stops on a skipped frame, that frame is fetched again.
- **UDP with targeted recovery.** Frames are split into ≤1200-byte datagrams so
  nothing fragments. The client NACKs exactly the missing pieces, and asks for a
  keyframe only when a frame is hopeless, so one loss never stalls the stream.
  A frame lost whole leaves only a gap, so its first packet is NACKed, which
  says how many more there are. An incomplete frame is dropped 80 ms after its
  last packet. A Subscribe counts as a keyframe request, and unanswered
  requests back off from 200 ms to 1.6 s.
- **Still sources.** Capture sends nothing while a window is still, so the
  server asks the mirror to repush its last frame when a client subscribes or
  needs a keyframe.
- **Decode on the GPU.** The client decodes with DXVA into textures and draws
  them with Direct2D; the canvas is composited, never copied. A new frame
  redraws the canvas only if its stream has a box there, and a pop-out only
  for its own stream. Each frame is staged into the decoder outside the device
  lock; only the decode itself takes it. Sidebar text layouts are cached.
- **True proportions.** A very thin strip can't be scaled to fit both the
  256 px floor and the 4096 px ceiling in proportion, and 16-pixel padding
  stretches a frame slightly. `net::EncodeSize` is shared by both ends: when
  the listed crop explains a stream's size, the client draws it in the crop's
  proportions, not the stream's.
- **Video processor.** The converter caches its input and output views for the
  few textures that come round every frame, and turns off the driver's
  automatic processing so the conversion is a plain one.

### Rate control and keyframes

The encoder runs at a constant bitrate with no B-frames, in low-latency mode,
and sends full pictures only when asked: a new viewer, or a frame lost for
good. It never sends them on a timer, which at desktop sizes would cost several
hundred KB every few seconds. It asks for the longest keyframe interval the
encoder accepts, falling back to shorter ones for encoders that cap it.

Constant bitrate was kept after measuring the alternatives on NVENC with
text-like 1080p content at 8 Mbps:

| Mode | Pointer-only frame | Every frame unrelated |
| --- | --- | --- |
| Constant | 2.3 KB | 56 Mbps |
| Peak-constrained VBR | identical to constant | identical |
| Quality-based | 0.2 KB | 340 to 600 Mbps, peak limit ignored |

Constant bitrate already spends little on small changes. Quality mode would
save more, but it has no ceiling, and bursts that large would flood the link
and lose packets. No mode keeps a stream of unrelated pictures within the
limit, because the encoder will not drop quality any further. Only sending
fewer frames could. The rate-control test reports all of this on whatever
encoder it runs on.

The *Encoder preset* setting maps to `CODECAPI_AVEncCommonQualityVsSpeed`:
*Fastest* is 0, *Quality* is 100, and *Balanced* leaves the encoder's own
default. NVENC has three steps, at 0 to 32, 33 to 65 and 66 to 100. At 4096x1152
and 20 Mbps on scrolling text (`rvmnet_test --presets`):

| Preset | Encode time per frame | Text PSNR |
| --- | --- | --- |
| Fastest | 2.4 ms | 33.0 dB |
| Balanced (default) | 4.8 ms | 31.9 dB |
| Quality | 6.4 ms | 31.3 dB |

On this content the fastest preset was also the sharpest; the slower presets
spend their effort on motion search, which text does not reward.

### Encoder quirks

- Intel Quick Sync changes its output type on the first frame
  (`MF_E_TRANSFORM_STREAM_CHANGE`). The encoder renegotiates and waits for the
  next `METransformHaveOutput`; calling `ProcessOutput` again at once returns
  `E_UNEXPECTED`.
- Some encoders keep the latest frame until another arrives. If a real frame
  produces nothing for 40 ms, the last picture is fed again (up to 8 times).
- If a keyframe arrives without an SPS and the output type carries a sequence
  header, the header is prepended so a decoder can start.
- Encoders refuse tiny frames; crops are scaled up to at least 256 px. A size an
  encoder rejects is retried at 16-pixel alignment, then with larger floors
  (384, 512, 768, 1024 px on the smallest side), keeping the crop's shape. The
  client recognises every floor, so it still draws the true shape. Once nothing
  is left to adjust, a hardware stream moves to the CPU encoder (below);
  after that, retries back off from 2 s to a minute, and the viewer is told
  the mirror cannot be encoded; a new size is tried at once.
- **Encoder sessions are limited.** GeForce cards run only so many encoder
  sessions at once: three on the drivers that still support a GT 730, twelve on
  an RTX 4080's current driver. A refused session looks like any other failure
  (NVIDIA's reports `MF_E_UNSUPPORTED_D3D_TYPE` from `SetOutputType`), so a
  failure while other streams hold sessions is taken to be the limit. The frame
  is not enlarged; the stream waits and is retried the moment another stream
  lets its encoder go, or every 10 s. Its viewers are sent `StreamStatus`, and
  the client shows "The server's encoder is busy with other streams" instead of
  waiting in silence. Before this, the GT 730 machine rebuilt a refused encoder
  every 2 seconds indefinitely, which was the last thing it logged before a
  crash.
- A retry needs a picture, and a still source sends none of its own, so the
  server asks the mirror for one when a retry falls due.
- The encoder on the device's own adapter is preferred (`MFTEnum2` with the
  adapter LUID), then every other hardware encoder in turn. Each failure is
  logged with its HRESULT.

### Encoding on the CPU

Where no hardware encoder can be fed, the server encodes with Windows' own
H.264 encoder ("H264 Encoder MFT") on the CPU. That means no hardware encoder,
or no D3D11 video processor to convert frames for one: basic display adapters,
WARP, many virtual machines and Remote Desktop sessions. `ChooseEncoder`
decides at server start, and the settings dialog shows the same choice. A
hardware stream that every encoder refuses at every size, such as one whose
only encoder sits on another GPU, also moves to the CPU, for good. The wire
protocol is the same, so clients cannot tell.

- **Conversion without a video processor.** `Nv12Packer` converts on the
  capture thread with two pixel-shader passes into one R8 texture laid out
  exactly as NV12 is in memory: the luma rows, then the interleaved chroma
  rows. One `CopyResource` puts it in a staging slot, so the CPU reads 1.5
  bytes a pixel, already converted. BT.709 studio range, like the video
  processor; the tests check the values exactly.
- **Readback off the capture thread.** The encode thread maps the staging slot
  with `D3D11_MAP_FLAG_DO_NOT_WAIT`, taking the device lock only for each
  attempt. While the GPU is still busy, `IDXGIDevice2::EnqueueSetEvent` arms an
  event for when its queued work has run, and the thread waits on that outside
  the lock rather than on the timer tick. The copy out also runs outside the
  lock, into a reused input buffer, and the slot is handed back before the
  frame is encoded.
- **Synchronous.** The CPU encoder always takes input, and `Encode` returns
  with whatever the frame produced: no events, no relay, no nudging. It gets
  the same constant bitrate, no B-frames, longest keyframe interval and
  low-latency mode, and it honours forced keyframes.
- **Cost.** Measured on a development VM with no GPU: about 2.3 ms a frame at
  640x360, and 13 to 15 ms at 1080p on detailed content that changes every
  frame. The presets made no measurable difference there. One
  encode thread serves every stream, so a busy CPU stream also delays the
  others; latest-frame-wins drops what the CPU cannot keep up with.
- The viewing client still decodes on the GPU. Windows' decoder buffers
  pictures for reordering unless low-latency mode is set, which the client and
  the tests' CPU decoder both do.

## Security

Everything on the wire is AES-256-GCM under a key derived from the shared
passphrase (PBKDF2-SHA256, 600k rounds). Each session derives two keys from a
two-random handshake, one per direction, so neither side's traffic can be
reflected back at it. Per-direction counters are the nonces, and a 64-packet
window rejects replays. Probes of the port see ciphertext and get no reply.
This is protocol version 3, which doesn't interoperate with earlier versions.

The WELCOME echoes the client's random, so a client ignores any WELCOME that
isn't the answer to its own HELLO. A HELLO only earns a pending handshake; a
client takes a slot only after its first message under the session key. A
HELLO seen in the last two minutes is refused outright, and an older one
replayed still can't produce a message under the new session key, so a
captured HELLO gets nowhere. A datagram too short to hold a tag and a body is
dropped before decryption. Even a peer holding the key is bounded:

| Limit | Value |
| --- | --- |
| Clients | 8 |
| Pending handshakes | 16, expiring after 5 s; 5 admissions a second |
| Subscriptions | Listed mirrors only, 64 per client |
| Keyframes and frame requests | 4 a second per mirror |
| Retransmits | 2000 a second per client, each packet once per NACK |
| Frame size | 8192 packets (about 9.5 MB) |
| Client reassembly | Two frames' worth of packets pending per stream; 64 unwanted mirror ids tracked |
| Log file | 16 MB |

Send and receive use separate cipher objects, since they run on different
threads. Keys are stored DPAPI-protected to the Windows account on both ends.
A saved key that can't be decrypted, because the file came from another
account or PC, is kept as it is rather than erased on the next save, and the
user is told to enter it again. Key fields are masked, with a *Show* box.

## Optional desktop control

`RVM_REMOTE_CONTROL` defaults to `OFF` and requires `RVM_STREAMING`. CMake
exports it as a numeric `0` or `1` definition through `rvm_options` to every
target, so public class layouts agree. `#if RVM_REMOTE_CONTROL` gates control
members, UI integration, network handlers, and desktop eligibility code.
CMake adds `net/control.cpp`, `client/control_ui.cpp`, and the control test
target only when enabled. OFF builds contain no input implementation or
no-op substitute. Use `#if`, not `#ifdef`: the macro is defined in both builds.

Both builds use protocol 3 and retain its capability byte and message IDs for
viewing compatibility. An OFF server always advertises zero capability and
ignores control messages; an OFF client ignores advertised control support.
The server advertises control only for enabled, bound,
uncropped desktop mirrors covering the current virtual screen. The server
checks the capability and the viewer's subscription again for every input
packet and while servicing a lease.

Control is explicit from a canvas tile's context menu and separate from the
double-click enlarged view. One tile has keyboard focus in a client, and one
viewer has a control lease on each server. The client keeps the other desktop
tiles visible. A foreground-only low-level keyboard hook forwards physical
scan codes (including Windows shortcuts); Ctrl+Alt+F12 posts a local release
message. The hook never renders or sends network traffic. Mouse input is mapped
through the same aspect ratio and letterboxing as the displayed image, to
absolute virtual-desktop coordinates. Button capture supports drags outside
the tile; clicks in letterbox bars are local.

Input travels inside the existing authenticated session, with its own ordered
sequence and acknowledgement. Batches carry up to 32 events, with at most 256
pending events. Unsent adjacent pointer moves coalesce; taps and wheel events
are retried and injected once. Input batches are paced with the performance
counter, up to every 8 ms, independently of the system timer's granularity.
Per-peer input is limited to 2000 events/second
with a 2000-event burst. A monotonically increasing control token prevents a
delayed request, release, or input from reviving or ending a newer focus lease.
The server tracks injected keys and buttons and releases them on relinquish,
unsubscribe, capability loss, disconnect, shutdown, or a 1.5-second lease
timeout. Missing acknowledgements or a full client queue also end control.
Reconnect restores viewing only. Tests inject into a recording sink, never
the machine's actual keyboard or mouse.

## Optional sign-in screen service

`RVM_LOGIN_SERVICE` (default `OFF`, requires `RVM_STREAMING`) builds
`RearViewMirrorService.exe` from `service/`. It is a separate executable. Its
hooks in shared code are `SetConfigDir`, which points a process's logs and crash
dumps somewhere other than a user's AppData, the `StreamServer` it reuses as is,
and the app's handoff below, compiled only with the option
(`src/login_handoff.*`, `#if RVM_LOGIN_SERVICE`).

**Why a service, and why a helper.** Services run in session 0, which has no
screen. The sign-in screen is the Winlogon desktop of the console session, and
only a LocalSystem process on that desktop can capture it or send it input.
So the service runs nothing but a supervisor. Its helper is the same executable
with `--helper`, launched as SYSTEM into the console session: the service's own
token is duplicated, given the console's session id, and passed to
`CreateProcessAsUserW` with the desktop `winsta0\winlogon`.

**When it runs.** The rule is that whatever the console shows is what gets
streamed. The supervisor keeps exactly one helper alive, in the console
session, while that session has nobody signed in or is locked, and none
otherwise. Session-change notifications (sign-in, sign-out, lock, unlock,
console connect) wake it, and it checks every 5 s regardless. Signing in counts
from the moment Windows records a user name, which is before the desktop
appears. A helper that exits is started again, with a
backoff from 1 s to a minute if it keeps exiting quickly. It is stopped through
an unnamed event, and ended after 5 s if it does not stop: the service passes
its process id and the event's handle on the command line, and the helper
duplicates the handle. The helper also exits if the service process ends.

**What the helper does.** Windows Graphics Capture is not built for secure
desktops, so `DuplicationCapture` uses DXGI Desktop Duplication instead. Each
monitor of the device's adapter has a thread of its own, which copies only
what changed (duplication's move and dirty rectangles) to the monitor's place
in one virtual-screen texture. Only while a client watches, and at most at the
stream's frame rate, the picture is composed with the pointer, which
duplication reports separately, drawn on top with Direct2D, and teed into a
`StreamServer` as mirror `0x7F000001`, "Sign-in screen". It serves on the app's port with the app's key, so a client
sees one server throughout: the sign-in screen, a brief reconnect, then the
app's mirrors. Duplication ends at every desktop switch or mode change; the
capture thread then moves itself to the new input desktop with
`OpenInputDesktop` and `SetThreadDesktop` and attaches again.

**The handoff.** Both serve on one port, so the app lets go of it whenever its
session is not what the console shows: locked, disconnected after Remote
Desktop, remote while Remote Desktop is connected, or switched away from. It
watches its own session's changes (`WTSRegisterSessionNotification`) and pauses
`Streaming`, which stops the server without touching the settings. It does so
only while the service is running, so without the service a locked app streams
as it always did. Back on the console and unlocked, it unpauses, trying each
second for half a minute while the helper lets go of the port; the app also
starts that way, since right after signing in the helper may still hold it.
While its session is away from the console it rechecks every 5 s, in case a
notification came between sessions or the service started or stopped.

**Remote input needs nothing new.** Every thread of the helper starts on the
Winlogon desktop, and that stays the input desktop for as long as the helper
runs, since it is stopped the moment the console shows a desktop. So the existing
`ControlHost` injects from the server's network thread exactly as it does in
the app, with `MOUSEEVENTF_VIRTUALDESK` addressing the same whole-screen
picture.

**Settings and trust.** Nobody is signed in when the helper runs, so neither
`%APPDATA%` nor a key encrypted to a user will do. `--install` (elevated) copies
the current user's port, key, bitrate, frame rate and preset to
`%ProgramData%\RearViewMirror\login.ini`, with the key encrypted to the machine
(DPAPI local-machine scope). The key must be at least 20 characters, which
*Generate* makes; an older install with a shorter key stops serving until
`--install` is run again. The folder's protected DACL, owned by Administrators
and granting only SYSTEM and Administrators, is what keeps it private, and it
is re-applied on every install. A link at that path is removed (the link, not
its target), and a folder anyone else could have made or changed is moved
aside to `RearViewMirror.untrusted-*` and made afresh. The service and helper
check the folder whenever they start and refuse one that isn't trusted; with nowhere
safe to log, the service stops with `ERROR_ACCESS_DENIED`, which Windows
records in the event log. The service binary is copied to
`%ProgramFiles%\RearViewMirror` first: a SYSTEM service running from a folder
its user can write would hand that user SYSTEM. Logs and crash dumps go to the
same ProgramData folder. Windows Firewall would ask before letting the helper's
port in, and nobody can answer at the sign-in screen, so `--install` adds a
rule as narrow as it can be: that program, inbound UDP, that port, private and
domain networks. `--uninstall` removes it.

**Not covered yet.**
- An app from before the handoff, or one built without the option, keeps the
  port in a locked or disconnected session; the helper keeps trying until it is
  free.
- While Remote Desktop is connected, the app's session is remote, so the client
  sees the console's sign-in screen rather than the Remote Desktop session.
- Ctrl+Alt+Delete is not sent (`SendSAS`). Windows 11 does not ask for it at
  sign-in unless a policy requires it.
- Monitors on a second adapter, and rotated monitors, stay black.
- Inverting pointer pixels (the text I-beam) are drawn black.
- Secure desktops cannot be tested headlessly. `--helper-test` runs the
  capture and stream in the current session, by hand.

## Platform notes

- Click-through needs `WS_EX_TRANSPARENT` **and** `WS_EX_LAYERED`; the first
  alone still lets hit-testing land on the window. Layering a
  `WS_EX_NOREDIRECTIONBITMAP` window doesn't blank its composition content, but
  `SetLayeredWindowAttributes` must still be called or it can appear empty.
- `WindowFromPoint` asks windows on the calling thread with `WM_NCHITTEST`
  instead of going by `WS_EX_TRANSPARENT`. The picker's click-through
  highlight answers `HTTRANSPARENT`, or the lookup finds the highlight itself
  rather than what is under it. Over the desktop, that made it hide and
  reappear on every mouse move, and the taskbar re-laid itself out each time.
- The video processor can't read a texture bound only as a shader resource, so
  the renderer's cache is also bound as a render target (and the converter
  copies through a scratch texture otherwise).
- DXVA output is padded to macroblock rows; decoded frames are always cropped
  to the display aperture.

## Signing

`tools/sign.ps1` keeps a self-signed code-signing certificate,
`CN=Rear View Mirror`, in `Cert:\CurrentUser\My` (RSA 3072, ten years). Its
private key can't be exported, and it is marked as not a certificate authority,
so trusting it can't vouch for anything else. CMake creates it once per build
before anything links, then signs each executable after linking, with a
timestamp when the network allows. Without signtool, CMake turns signing off
with a warning. Nothing is stored in the repository.

| Command | Effect |
| --- | --- |
| `sign.ps1 -Path <file>` | Sign one file |
| `sign.ps1 -Trust` | Trust the certificate on this PC (root and publisher stores) |
| `sign.ps1 -Remove` | Delete the certificate from all three stores |

## Tests

`build\rvmnet_test.exe` runs headlessly: crypto, replay protection,
packetisation with loss, the assembler's limits and keyframe back-off, GPU
encode/decode round trips, encoder size limits (including whole-desktop
sizes), desktop capture (frames counted, never read back), a full
server-to-client loopback, reconnects, hostile-client limits, the frame-rate
cap, rate control and keyframe policy, and a 120 fps end-to-end stream. Any
machine runs the CPU path (the frame packer's exact BT.709 values, the CPU
codec with forced keyframes and 1080p timings, a CPU stream to a raw peer) and
the wake-up checks: the client's network thread wakes for what the UI asks,
the encode thread only when it has a reason, and an idle server's network
thread not at all. Tests that need a hardware encoder, a video processor or
screen capture skip without one; on the GPU-less development VM that leaves
109 checks with remote control built, 107 without. It logs to `test.log` beside the app's logs. Timing
checks leave room for a busy machine; servers bind port 0 so a running copy of
the app doesn't get in the way.

| Mode | Purpose |
| --- | --- |
| `rvmnet_test.exe` | Run every test |
| `--bench` | Time the encoder path on this machine |
| `--presets` | Time and quality of each encoder preset at desktop size |
| `--live` | Connect to this PC's running app and report what arrives |
| `--crash-probe` | Crash on purpose, with no error dialog, to check the crash report |

With remote control enabled, `build\rvmcontrol_test.exe` checks lost and
duplicated datagrams, ordering, stale control tokens, exclusive admission,
malformed input, pointer coalescing, bounded queues, focus expiry, disconnects,
capability revocation, and injection failure without generating real input.
