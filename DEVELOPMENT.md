# Development notes

How Rear View Mirror works inside, and why. For using and building it, see
[README.md](README.md).

## Layout

| File | Role |
| --- | --- |
| `src/gfx.*` | Shared D3D11/D2D/DWrite device, composition surface, logging |
| `src/capture.*` | Windows Graphics Capture session, frames as GPU textures |
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

The picker and region selector are Direct2D windows on the same device. The
picker uses low-level mouse and keyboard hooks so the choosing click never
reaches the target application.

## Threading

One D3D11 device is shared. Capture threads render mirrors; the UI thread
renders the manager, overlays and client through Direct2D. `ID3D11Multithread`
is not enough: it makes single calls atomic, but a mirror's state-setting
sequence landing inside a Direct2D `BeginDraw`/`EndDraw` still clobbers render
target, viewport and shaders. `Gfx::deviceMutex` is held across each complete
draw.

Lock order is **capture state → renderer → device**, never reversed, and
`Present` happens outside all three.

- `WindowCapture` runs the frame callback under its state lock, and `Stop()`
  takes the same lock, so a capture owner can't be destroyed mid-callback.
- The renderer draws under its own lock plus the device lock, then releases
  both before presenting, so a vsync wait blocks nothing else. `Present` and
  `ResizeBuffers` are serialised by a small dedicated mutex.
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
paths return failure rather than throw, so a lost device leaves a mirror that
stops updating, not a crash.

## Persistence

Mirrors are saved to `mirrors.ini` on every change, coalesced so one hotkey that
touches every mirror writes once. Each write goes to a temp file that replaces
the old one, so a crash mid-save leaves the previous file intact. Files are
UTF-16 with a BOM so titles in any script survive, and every size read back is
clamped to what the device can create.

Window handles don't survive a restart, so each mirror records the source's
executable, class and title. At launch it binds to the best live match: the
executable must agree, and so must the class or the title (a long shared prefix
counts, so retitled windows still match). Two mirrors never bind to one window.

Unmatched mirrors wait. The app re-checks at 2 s, 4 s, then every 8 s, and also
the moment any window comes to the foreground, which is what reopening an app
does. A mirror whose source closes while running becomes an orphan and rebinds
the same way.

## Streaming

The app is the server; `RearViewMirrorClient.exe` is the client. One UDP port
carries everything.

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
- **Latest frame wins.** Frames pass through four texture slots between capture
  and encoder. A slot the encoder still holds is protected until its output
  appears; if the encoder is behind, older frames are dropped, never queued. The
  client does the same on its decode queue.
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
  them with Direct2D; the canvas is composited, never copied.

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
  encoder rejects is retried at 16-pixel alignment.
- The encoder on the device's own adapter is preferred (`MFTEnum2` with the
  adapter LUID), then every other hardware encoder in turn. Each failure is
  logged with its HRESULT.

## Security

Everything on the wire is AES-256-GCM under a key derived from the shared
passphrase (PBKDF2, 100k rounds). Each session gets its own key from a
two-random handshake; per-direction counters are the nonces, and a 64-packet
window rejects replays. Probes of the port see ciphertext and get no reply.

A HELLO only earns a pending handshake; a client takes a slot only after its
first message under the session key, so a captured HELLO replayed later gets
nowhere. Even a client holding the key is bounded:

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

## Platform notes

- Click-through needs `WS_EX_TRANSPARENT` **and** `WS_EX_LAYERED`; the first
  alone still lets hit-testing land on the window. Layering a
  `WS_EX_NOREDIRECTIONBITMAP` window doesn't blank its composition content, but
  `SetLayeredWindowAttributes` must still be called or it can appear empty.
- The video processor can't read a texture bound only as a shader resource, so
  the renderer's cache is also bound as a render target (and the converter
  copies through a scratch texture otherwise).
- DXVA output is padded to macroblock rows; decoded frames are always cropped
  to the display aperture.

## Signing

`tools/sign.ps1` keeps a self-signed code-signing certificate,
`CN=Rear View Mirror`, in `Cert:\CurrentUser\My` (RSA 3072, ten years). CMake
creates it once per build before anything links, then signs each executable
after linking, with a timestamp when the network allows. Nothing is stored in
the repository.

| Command | Effect |
| --- | --- |
| `sign.ps1 -Path <file>` | Sign one file |
| `sign.ps1 -Trust` | Trust the certificate on this PC (root and publisher stores) |
| `sign.ps1 -Remove` | Delete the certificate from all three stores |

## Tests

`build\rvmnet_test.exe` runs headlessly: crypto, replay protection,
packetisation with loss, GPU encode/decode round trips, encoder size limits, a
full server-to-client loopback, reconnects, hostile-client limits, the
frame-rate cap and a 120 fps end-to-end stream. It logs to `test.log` beside the
app's logs.

| Mode | Purpose |
| --- | --- |
| `rvmnet_test.exe` | Run every test |
| `--bench` | Time the encoder path on this machine |
| `--live` | Connect to this PC's running app and report what arrives |
