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
| `src/net/codec.*`, `converter.*` | Media Foundation H.264 encode/decode; D3D11 colour conversion |
| `src/streaming.*` | The app's one seam to streaming; `streaming_off.cpp` is the empty stand-in |
| `src/stream_server.*` | Serves mirrors: encoders, sessions, retransmits, limits |
| `src/stream_settings.*` | Streaming settings file and dialog |
| `client/` | Client: connection, decode, canvas, pop-out windows, settings |
| `tests/net_test.cpp` | Headless tests for the streaming stack |
| `tools/sign.ps1` | Code-signing certificate management and signing |

## Mirror pipeline

The picture never leaves the GPU:

1. **Windows Graphics Capture** (`Direct3D11CaptureFramePool`, free-threaded)
   hands over each new frame of the source window as a D3D11 texture. Capture's
   default 16 ms minimum update interval (about 60 fps) is lowered to 1 ms, so
   fast sources run at the display's refresh rate.
2. The frame is copied once into a cache texture, so a repaint after a resize or
   opacity change doesn't wait for the source to change again.
3. The crop is applied on the GPU: a UV rectangle when scaling up, a mipped blit
   when scaling down, which stops heavy minification from shimmering.
4. A pixel shader applies opacity and an antialiased rounded-rect mask and
   presents to a **DirectComposition** swapchain on a
   `WS_EX_NOREDIRECTIONBITMAP` window, so corners blend with what is behind.

There is no render loop. Capture delivers a frame only when the source changes,
so a static mirror costs nothing. The process is per-monitor-DPI-aware v2, so
capture textures, window rects and overlays all use physical pixels.

**Desktop mirrors** use the same pipeline with a different source.
`DesktopCapture` captures every monitor separately, pointer included. Each
monitor's frame is copied into one texture the size of the virtual screen, at
that monitor's place in it, and the whole texture is passed on. Monitors take
turns under one lock, so each picture passed on includes every copy before it.
Gaps between monitors of different sizes stay black. WGC can't follow a change
of monitors, so `WM_DISPLAYCHANGE` restarts desktop capture. If a restart fails
mid-change, the mirror waits and retries like a mirror whose window has gone.
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

- `WindowCapture` runs the frame callback under its state lock, and `Stop()`
  takes the same lock, so a capture owner can't be destroyed mid-callback.
- The renderer's present lock covers one whole draw-and-present, or a resize.
  Otherwise a UI redraw could present between a capture thread's draw and its
  present, which would then show a back buffer nobody drew. The renderer and
  device locks are released before presenting, so a vsync wait holds up only
  that one mirror's other presents.
- Direct2D windows draw only a snapshot taken in `PrepareDraw`, before the
  device lock. Nothing in `OnDraw` calls into a mirror, which keeps the order
  impossible to invert by accident.

Mirrors are addressed by a stable id, never by position, and destroyed lazily.
Context menus, the region selector and message boxes run nested message loops
inside `Mirror` methods, during which the mirror can be removed. Removal tears
the object down at once but frees it only when the outer loop next turns.
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
before. Copy the .pdb along with the .exe to other machines.

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

Mirrors are saved to `mirrors.ini` on every change, coalesced so one hotkey that
touches every mirror writes once. Each write goes to a temp file that replaces
the old one, so a crash mid-save leaves the previous file intact. Files are
UTF-16 with a BOM so titles in any script survive, and every size read back is
clamped to what the device can create.

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
does. A mirror whose source closes while running becomes an orphan and rebinds
the same way.

## Streaming

The app is the server; `RearViewMirrorClient.exe` is the client. One UDP port
carries everything.

**One seam.** The app reaches streaming only through the `Streaming` class. It
is told when a mirror is created (to attach the mirror's frame hook) and when
mirrors change (to republish the list). It posts requests for a picture back
to the app window, and it runs the settings dialog. Mirrors and the renderer
know nothing of streaming: a mirror offers a generic frame hook and a way to
resend its last frame. `-DRVM_STREAMING=OFF` compiles `streaming_off.cpp`, which
does nothing and reports streaming unavailable, and leaves out the libraries,
the client and the tests. There is no conditional compilation in the code.

| Library | Contents | Used by |
| --- | --- | --- |
| `rvmcore` | Device, overlays, persistence, logging | Everything |
| `rvmnet` | Crypto, UDP, packetizer, codec, converter | Both ends |
| `rvmserver` | `stream_server.cpp` | App, tests |
| `rvmclientnet` | `client/stream_client.cpp` | Client, tests |

- **Zero-copy into the encoder.** The renderer's cache texture is teed to the
  server, which crops it into NV12 with one video-processor blit and hands it to
  the GPU's H.264 encoder through Media Foundation (NVENC, Quick Sync or AMF).
  Only the compressed bitstream reaches the CPU.
- **One encoder per mirror**, shared by every client watching it; nothing is
  encoded while nobody watches, and unwatched streams are freed.
- **Event-driven encoding.** The hardware encoder is asynchronous. Its "want
  input" and "have output" events arrive through an `IMFAsyncCallback` relay
  that queues them and wakes the encode thread. Nothing polls, so a frame costs
  only the encoder's own time (about 1 ms), independent of the system timer's
  15.6 ms tick.
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
- **Still sources.** Capture sends nothing while a window is still, so the
  server asks the mirror to repush its last frame when a client subscribes or
  needs a keyframe.
- **Decode on the GPU.** The client decodes with DXVA into textures and draws
  them with Direct2D; the canvas is composited, never copied. A new frame
  redraws the canvas only if one of that server's boxes is on it.
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
  is left to adjust, retries back off from 2 s to a minute, and the viewer is
  told the mirror cannot be encoded; a new size is tried at once.
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

## Security

Everything on the wire is AES-256-GCM under a key derived from the shared
passphrase (PBKDF2-SHA256, 600k rounds). Each session derives two keys from a
two-random handshake, one per direction, so neither side's traffic can be
reflected back at it. Per-direction counters are the nonces, and a 64-packet
window rejects replays. Probes of the port see ciphertext and get no reply.
This is protocol version 2, which doesn't interoperate with version 1.

The WELCOME echoes the client's random, so a client ignores any WELCOME that
isn't the answer to its own HELLO. A HELLO only earns a pending handshake; a
client takes a slot only after its first message under the session key. A
HELLO seen in the last two minutes is refused outright, and an older one
replayed still can't produce a message under the new session key, so a
captured HELLO gets nowhere. A datagram too short to hold a tag and a body is dropped before
decryption. Even a client holding the key is bounded:

| Limit | Value |
| --- | --- |
| Clients | 8 |
| Pending handshakes | 16, expiring after 5 s; 5 admissions a second |
| Subscriptions | Listed mirrors only, 64 per client |
| Keyframes and frame requests | 4 a second per mirror |
| Retransmits | 2000 a second per client, each packet once per NACK |
| Log file | 16 MB |

Send and receive use separate cipher objects, since they run on different
threads. Keys are stored DPAPI-protected to the Windows account on both ends.
A saved key that can't be decrypted, because the file came from another
account or PC, is kept as it is rather than erased on the next save, and the
user is told to enter it again. Key fields are masked, with a *Show* box.

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
packetisation with loss, GPU encode/decode round trips, encoder size limits
(including whole-desktop sizes), desktop capture (frames counted, never read
back), a full server-to-client loopback, reconnects, hostile-client limits, the
frame-rate cap, rate control and keyframe policy, and a 120 fps end-to-end
stream. It logs to `test.log` beside the
app's logs. Timing checks leave room for a busy machine; servers bind port 0
so a running copy of the app doesn't get in the way.

| Mode | Purpose |
| --- | --- |
| `rvmnet_test.exe` | Run every test |
| `--bench` | Time the encoder path on this machine |
| `--presets` | Time and quality of each encoder preset at desktop size |
| `--live` | Connect to this PC's running app and report what arrives |
| `--crash-probe` | Crash on purpose, with no error dialog, to check the crash report |
