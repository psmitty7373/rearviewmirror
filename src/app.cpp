#include "app.h"
#include "net/codec.h"
#include "picker.h"
#include "region.h"

namespace rvm {

namespace {

// Waiting mirrors are re-checked on a timer that backs off to a slow poll and
// keeps going: a source app can be closed and reopened at any time, and a
// Startup-folder launch can win the race against the programs it watches.
constexpr UINT_PTR kTimerRestore = 1;
constexpr UINT_PTR kTimerSave    = 2;
constexpr UINT kRestoreMinPeriodMs = 2000;
constexpr UINT kRestoreMaxPeriodMs = 8000;
constexpr ULONGLONG kRestorePassMinGapMs = 500;   // Alt-tab spam must not become enumeration spam.
constexpr UINT kSaveDelayMs = 250;

HWND g_appWindow = nullptr;

void CALLBACK ForegroundChanged(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
    if (g_appWindow) PostMessageW(g_appWindow, WM_RVM_FOREGROUND_CHANGED, 0, 0);
}

enum HotkeyId : int {
    kHotkeyNewMirror = 1,
    kHotkeyClickThrough,
    kHotkeyCloseAll,
};

enum TrayMenuId : UINT {
    kTrayNewMirror = 1,
    kTrayClickThrough,
    kTrayCloseAll,
    kTrayExit,
    kTrayManager,
    kTrayStreaming,
};

// Menu command ids for the mirror list are offset mirror ids, never positions.
constexpr UINT kMirrorMenuBase = 1000;

}  // namespace

bool App::CreateOwnerWindow() {
    iconLarge_ = LoadAppIcon(GetSystemMetrics(SM_CXICON));
    iconSmall_ = LoadAppIcon(GetSystemMetrics(SM_CXSMICON));

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = &WndProcThunk<App, &App::WndProc>;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = kAppWindowClass;
    wc.hIcon         = iconLarge_;
    wc.hIconSm       = iconSmall_;
    RegisterClassExW(&wc);

    // An ordinary hidden top-level window rather than a message-only one:
    // a popup menu's owner has to be able to take the foreground for the menu
    // to dismiss on an outside click, and message-only windows cannot.
    hwnd_ = CreateWindowExW(0, kAppWindowClass, kAppName, WS_POPUP, 0, 0, 0, 0,
                            nullptr, nullptr, GetModuleHandleW(nullptr), this);
    g_appWindow = hwnd_;
    return hwnd_ != nullptr;
}

void App::CreateTrayIcon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = hwnd_;
    nid.uID              = 1;
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_RVM_TRAY;
    nid.hIcon            = iconSmall_ ? iconSmall_ : LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(nid.szTip, kAppName);
    trayAdded_ = Shell_NotifyIconW(NIM_ADD, &nid) == TRUE;
}

void App::RemoveTrayIcon() {
    if (trayAdded_) {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd   = hwnd_;
        nid.uID    = 1;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        trayAdded_ = false;
    }
    iconLarge_ = nullptr;   // Shared icons; LoadAppIcon owns them.
    iconSmall_ = nullptr;
}

void App::ShowBalloon(const std::wstring& text) {
    if (!trayAdded_) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = hwnd_;
    nid.uID    = 1;
    nid.uFlags = NIF_INFO;
    wcscpy_s(nid.szInfoTitle, kAppName);
    wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
    nid.dwInfoFlags = NIIF_NONE;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void App::ShowTrayMenu() {
    // A plain list, one line per mirror, checked when it is switched on.
    // Clicking toggles it; right-clicking offers to remove it.
    HMENU mirrorList = CreatePopupMenu();
    if (mirrors_.empty()) {
        AppendMenuW(mirrorList, MF_STRING | MF_GRAYED, 0, L"(no mirrors)");
    } else {
        for (const auto& m : mirrors_) {
            std::wstring label = m->DisplayName();
            if (m->Enabled() && m->Hidden()) label += L"   (hidden)";
            AppendMenuW(mirrorList, MF_STRING | (m->Enabled() ? MF_CHECKED : 0),
                        kMirrorMenuBase + m->Id(), label.c_str());
        }
    }
    mirrorListMenu_ = mirrorList;
    rightClickedId_ = 0;

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kTrayManager, L"Manage mirrors…");
    SetMenuDefaultItem(menu, kTrayManager, FALSE);   // Matches the double-click.
    AppendMenuW(menu, MF_STRING, kTrayNewMirror, L"New mirror…\tCtrl+Alt+M");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(mirrorList), L"Mirrors");
    std::wstring streaming = L"Streaming…";
    if (server_.Running()) {
        streaming = L"Streaming (on, " +
                    Plural(static_cast<int>(server_.ClientCount()), L"client", L"clients") + L")…";
    }
    AppendMenuW(menu, MF_STRING, kTrayStreaming, streaming.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    const bool allThrough = AllClickThrough();
    AppendMenuW(menu, MF_STRING | (allThrough ? MF_CHECKED : 0) | (mirrors_.empty() ? MF_GRAYED : 0),
                kTrayClickThrough, L"Click-through, all mirrors\tCtrl+Alt+T");
    const bool anything = !mirrors_.empty() || !pending_.empty();
    AppendMenuW(menu, MF_STRING | (anything ? 0 : MF_GRAYED), kTrayCloseAll,
                L"Close and forget all\tCtrl+Alt+X");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayExit, L"Exit");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd_);
    // No TPM_NONOTIFY: WM_MENURBUTTONUP is how a right-click on an entry is
    // reported. No TPM_RIGHTBUTTON either, so a right-click does not also
    // activate the item it lands on.
    const UINT cmd = static_cast<UINT>(TrackPopupMenu(
        menu, TPM_RETURNCMD, pt.x, pt.y, 0, hwnd_, nullptr));
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);
    mirrorListMenu_ = nullptr;

    if (rightClickedId_ != 0) {
        const uint32_t id = rightClickedId_;
        rightClickedId_ = 0;
        ConfirmCloseMirror(id);
        return;
    }

    if (cmd >= kMirrorMenuBase) {
        if (Mirror* m = FindMirror(cmd - kMirrorMenuBase)) {
            if (m->Enabled() && m->Hidden()) {
                m->SetHidden(false);   // Listed as "(hidden)": the click shows it.
                return;
            }
            if (!SetMirrorEnabled(*m, !m->Enabled())) {
                ShowBalloon(L"Could not switch that mirror back on: its source "
                            L"window is not open.");
            }
        }
        return;
    }

    switch (cmd) {
    case kTrayManager:      manager_.Open(this); break;
    case kTrayStreaming:    ShowStreamSettings(); break;
    case kTrayNewMirror:    RequestNewMirror(); break;
    case kTrayClickThrough: SetClickThroughAll(!allThrough); break;
    case kTrayCloseAll:     ConfirmCloseAll(); break;
    case kTrayExit:         PostMessageW(hwnd_, WM_CLOSE, 0, 0); break;
    default: break;
    }
}

void App::ConfirmCloseMirror(uint32_t id) {
    Mirror* m = FindMirror(id);
    if (!m) return;
    const std::wstring text = L"Close this mirror?\n\n" + m->DisplayName();
    SetForegroundWindow(hwnd_);
    const int answer = MessageBoxW(nullptr, text.c_str(), kAppName,
                                   MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
    if (answer == IDYES) CloseMirror(id);   // Re-resolved: the box ran a nested loop.
}

void App::NewMirror() {
    // Picking and selecting run nested loops; a second request arriving
    // meanwhile (hotkey, second launch, manager button) must not start a
    // second, overlapping selection.
    if (selecting_) return;
    selecting_ = true;
    struct Reset { bool& flag; ~Reset() { flag = false; } } reset{ selecting_ };

    HWND target = PickWindow();
    if (!target) return;

    const SIZE captureSize = CaptureItemSize(target);
    if (captureSize.cx <= 0 || captureSize.cy <= 0) {
        MessageBoxW(nullptr, L"That window cannot be captured.", kAppName, MB_OK | MB_ICONWARNING);
        return;
    }

    RECT crop{};
    if (!SelectRegion(target, captureSize, RECT{}, crop)) return;

    MirrorState state;
    state.crop     = crop;
    state.baseSize = captureSize;
    FillIdentity(target, state);
    state.group = GroupForWindow(target);

    auto mirror = std::make_unique<Mirror>(nextId_++);
    if (!mirror->Create(state, target, hwnd_)) {
        MessageBoxW(nullptr, L"Could not start capturing that window.", kAppName,
                    MB_OK | MB_ICONWARNING);
        return;
    }
    AttachStream(*mirror);
    mirrors_.push_back(std::move(mirror));
    MarkDirty();
    manager_.Refresh();
}

// The streaming tee. The sink outlives every mirror: server_ is destroyed with
// the App, after all of them.
void App::AttachStream(Mirror& mirror) {
    const uint32_t id = mirror.Id();
    Log(L"app: stream tee attached to mirror %u", id);
    mirror.SetFrameSink([this, id](ID3D11Texture2D* cache, const RECT& crop) {
        server_.SubmitFrame(id, cache, crop);
    });
}

Mirror* App::MirrorAt(size_t index) {
    return index < mirrors_.size() ? mirrors_[index].get() : nullptr;
}

Mirror* App::FindMirror(uint32_t id) {
    for (auto& m : mirrors_) {
        if (m->Id() == id) return m.get();
    }
    return nullptr;
}

void App::RequestNewMirror() {
    PostMessageW(hwnd_, WM_RVM_NEW_MIRROR, 0, 0);
}

// Tears the mirror down immediately so it disappears at once, but defers
// deleting the object until the outer message loop turns.
void App::RetireMirror(size_t index) {
    if (index >= mirrors_.size()) return;
    mirrors_[index]->Destroy();
    retired_.push_back(std::move(mirrors_[index]));
    mirrors_.erase(mirrors_.begin() + static_cast<ptrdiff_t>(index));
}

void App::CloseMirror(uint32_t id) {
    for (size_t i = 0; i < mirrors_.size(); ++i) {
        if (mirrors_[i]->Id() == id) {
            RetireMirror(i);
            MarkDirty();
            manager_.Refresh();
            return;
        }
    }
}

// The graphics device is gone, and every capture, swapchain and encoder with
// it. Save, and let a fresh process pick up from the saved state on a new
// device. A device that fails again straight after a relaunch is reported
// rather than relaunched, so a broken GPU cannot cause a restart loop.
void App::OnDeviceLost() {
    if (deviceLostHandled_) return;
    deviceLostHandled_ = true;
    SaveNow();
    const bool looping = relaunched_ && GetTickCount64() - startedMs_ < 30000;
    if (looping || !RelaunchSelf()) {
        MessageBoxW(nullptr,
                    L"The graphics device stopped working and did not recover. "
                    L"Your mirrors are saved; start Rear View Mirror again once the display "
                    L"driver is working.",
                    kAppName, MB_OK | MB_ICONERROR);
    }
    PostMessageW(hwnd_, WM_CLOSE, 0, 0);
}

// Everything, including mirrors still waiting for their apps, is forgotten for
// good, and the hotkey is system-wide: confirm first.
void App::ConfirmCloseAll() {
    const int count = static_cast<int>(mirrors_.size() + pending_.size());
    if (count == 0) return;
    const std::wstring question = L"Close and forget " + Plural(count, L"mirror", L"mirrors") +
                                  L"? This cannot be undone.";
    SetForegroundWindow(hwnd_);
    if (MessageBoxW(hwnd_, question.c_str(), kAppName, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES) {
        CloseAll();
    }
}

void App::CloseAll() {
    while (!mirrors_.empty()) RetireMirror(mirrors_.size() - 1);
    pending_.clear();
    KillTimer(hwnd_, kTimerRestore);
    restoreTimerActive_ = false;
    MarkDirty();
    manager_.Refresh();
}

bool App::AllClickThrough() const {
    if (mirrors_.empty()) return false;
    return std::all_of(mirrors_.begin(), mirrors_.end(),
                       [](const auto& m) { return m->ClickThrough(); });
}

void App::SetClickThroughAll(bool on) {
    for (auto& m : mirrors_) m->SetClickThrough(on);   // Each posts a state change.
    manager_.Refresh();
}

void App::MarkDirty() {
    dirty_ = true;
    SetTimer(hwnd_, kTimerSave, kSaveDelayMs, nullptr);
    PushMirrorList();   // Every state change is also a change to what clients may list.
}

void App::PushMirrorList() {
    std::vector<MirrorInfo> list;
    for (const auto& m : mirrors_) {
        if (!m->Enabled() || m->Orphaned()) continue;
        const SIZE native = m->NativeSize();
        list.push_back({ m->Id(), m->DisplayName(),
                         static_cast<UINT>((std::max)(native.cx, 0L)),
                         static_cast<UINT>((std::max)(native.cy, 0L)) });
    }
    server_.SetMirrorList(std::move(list));
}

// Stops the server, and starts it again if the settings say it should run.
// False if it should run but could not.
bool App::ApplyStreamSettings() {
    server_.Stop();
    manager_.Refresh();
    if (!streamSettings_.enabled) return true;
    // The server asks on its network thread; the mirror lives on this one.
    server_.SetFrameRequester([hwnd = hwnd_](uint32_t mirrorId) {
        PostMessageW(hwnd, WM_RVM_STREAM_WANT_FRAME, mirrorId, 0);
    });
    if (!server_.Start(streamSettings_)) return false;
    PushMirrorList();
    manager_.Refresh();
    return true;
}

void App::ShowStreamSettings() {
    StreamControl control;
    control.start = [this](const StreamSettings& s, std::wstring& error) {
        streamSettings_ = s;
        streamSettings_.enabled = true;
        const bool ok = ApplyStreamSettings();
        if (!ok) {
            streamSettings_.enabled = false;
            error = L"Streaming could not start on UDP port " + std::to_wstring(s.port) +
                    L". Is another program using it?";
        }
        SaveStreamSettings(streamSettings_);
        return ok;
    };
    control.stop = [this] {
        streamSettings_.enabled = false;
        SaveStreamSettings(streamSettings_);
        ApplyStreamSettings();
    };
    control.running = [this] { return server_.Running(); };
    control.status = [this] {
        if (!server_.Running()) return std::wstring(L"Stopped");
        return L"Running on UDP port " + std::to_wstring(streamSettings_.port) + L"  ·  " +
               Plural(static_cast<int>(server_.ClientCount()), L"client", L"clients") +
               L" connected";
    };

    StreamSettings edited = streamSettings_;
    if (!ShowStreamSettingsDialog(hwnd_, edited, control)) return;

    // OK: keep the fields. A running server picks up changed ones by restarting;
    // its clients reconnect by themselves.
    const StreamSettings before = streamSettings_;
    streamSettings_ = edited;
    SaveStreamSettings(streamSettings_);
    const bool changed = before.port != edited.port || before.key != edited.key ||
                         before.bitrateKbps != edited.bitrateKbps || before.fps != edited.fps;
    if (server_.Running() && changed && !ApplyStreamSettings()) {
        ShowBalloon(L"Streaming could not restart on UDP port " +
                    std::to_wstring(streamSettings_.port) + L". Is another program using it?");
        streamSettings_.enabled = false;
        SaveStreamSettings(streamSettings_);
    }
}

void App::SaveNow() {
    KillTimer(hwnd_, kTimerSave);
    dirty_ = false;

    std::vector<MirrorState> states;
    states.reserve(mirrors_.size() + pending_.size());
    for (const auto& m : mirrors_) states.push_back(m->SaveState());
    // Unmatched entries stay on disk until their app comes back.
    for (const auto& p : pending_) states.push_back(p);
    SaveMirrorStates(states);
}

std::vector<HWND> App::TargetsOfOtherGroups(uint32_t group) const {
    std::vector<HWND> targets;
    for (const auto& m : mirrors_) {
        if (m->Group() != group && m->Target() && IsWindow(m->Target())) targets.push_back(m->Target());
    }
    return targets;
}

HWND App::TargetOfGroup(uint32_t group, const Mirror* except) const {
    for (const auto& m : mirrors_) {
        if (m.get() != except && m->Group() == group && !m->Orphaned() && m->Target() &&
            IsWindow(m->Target())) {
            return m->Target();
        }
    }
    return nullptr;
}

// A second mirror of a window another mirror already shows joins its group;
// otherwise a fresh, unused group number.
uint32_t App::GroupForWindow(HWND target) const {
    for (const auto& m : mirrors_) {
        if (m->Target() == target && m->Group() != 0) return m->Group();
    }
    std::random_device random;
    for (;;) {
        const uint32_t g = random();
        if (g != 0 && std::none_of(mirrors_.begin(), mirrors_.end(),
                                   [&](const auto& m) { return m->Group() == g; })) {
            return g;
        }
    }
}

bool App::SetMirrorEnabled(Mirror& mirror, bool on) {
    return mirror.SetEnabled(on, TargetsOfOtherGroups(mirror.Group()),
                             TargetOfGroup(mirror.Group(), &mirror));
}

int App::TryRebindOrphans() {
    int rebound = 0;
    for (auto& m : mirrors_) {
        if (!m->Orphaned()) continue;
        if (m->TryRebind(TargetsOfOtherGroups(m->Group()), TargetOfGroup(m->Group(), m.get()))) {
            ++rebound;
        }
    }
    return rebound;
}

bool App::AnythingWaiting() const {
    if (!pending_.empty()) return true;
    for (const auto& m : mirrors_) {
        if (m->Orphaned()) return true;
    }
    return false;
}

void App::EnsureRestoreTimer() {
    if (restoreTimerActive_ || !AnythingWaiting()) return;
    restorePeriodMs_ = kRestoreMinPeriodMs;
    restoreTimerActive_ = true;
    SetTimer(hwnd_, kTimerRestore, restorePeriodMs_, nullptr);
}

// One attempt to bind everything that is waiting. The mirror reappearing is
// its own feedback, so no balloon here; only the launch-time restore announces.
int App::RunRestorePass() {
    const ULONGLONG now = GetTickCount64();
    if (now - lastRestorePassTick_ < kRestorePassMinGapMs) return 0;
    lastRestorePassTick_ = now;

    const int restored = TryRestorePending() + TryRebindOrphans();
    if (restored > 0) {
        MarkDirty();
        manager_.Refresh();
    }
    return restored;
}

int App::TryRestorePending() {
    int restored = 0;
    for (auto it = pending_.begin(); it != pending_.end();) {
        HWND target = nullptr;
        if (it->enabled) {
            // A group member already showing the window takes it straight
            // away; otherwise search, never among other groups' windows.
            target = TargetOfGroup(it->group, nullptr);
            if (!target) target = FindMatchingWindow(*it, TargetsOfOtherGroups(it->group));
            if (!target) {
                ++it;
                continue;
            }
        }
        // A mirror saved disabled needs no source yet: it takes its place in
        // the listing and binds when it is switched on.
        auto mirror = std::make_unique<Mirror>(nextId_++);
        if (!mirror->Create(*it, target, hwnd_)) {
            ++it;   // Keep it for the next attempt rather than forgetting it.
            continue;
        }
        AttachStream(*mirror);
        mirrors_.push_back(std::move(mirror));
        ++restored;
        it = pending_.erase(it);
    }
    return restored;
}

void App::RestoreSaved() {
    pending_ = LoadMirrorStates();
    if (pending_.empty()) {
        RequestNewMirror();   // Nothing to bring back: go straight to picking.
        return;
    }

    const int restored = TryRestorePending();
    if (restored > 0) ShowBalloon(L"Restored " + Plural(restored, L"mirror", L"mirrors") + L".");

    EnsureRestoreTimer();
    MarkDirty();
}

LRESULT App::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    // Explorer restarting takes every notification icon with it; it
    // broadcasts this once it is back, and the icon has to be added again.
    static const UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (msg == taskbarCreated && taskbarCreated != 0) {
        trayAdded_ = false;
        CreateTrayIcon();
        return 0;
    }

    switch (msg) {
    case WM_RVM_NEW_MIRROR:
        NewMirror();
        return 0;

    case WM_RVM_MIRROR_CLOSED:
        CloseMirror(static_cast<uint32_t>(wp));
        return 0;

    case WM_RVM_STATE_CHANGED:
        MarkDirty();
        manager_.Refresh();
        return 0;

    case WM_RVM_MIRROR_ORPHANED:
        MarkDirty();
        manager_.Refresh();
        EnsureRestoreTimer();
        return 0;

    case WM_RVM_FOREGROUND_CHANGED:
        if (AnythingWaiting()) RunRestorePass();
        return 0;

    case WM_RVM_DEVICE_LOST:
        OnDeviceLost();
        return 0;

    case WM_RVM_STREAM_WANT_FRAME: {
        Mirror* m = FindMirror(static_cast<uint32_t>(wp));
        RVM_LOG_SAMPLED(100, L"app: frame wanted for mirror %u -> %s", static_cast<uint32_t>(wp),
                        m ? (m->Orphaned() ? L"orphaned" : L"found") : L"NOT FOUND");
        if (m) m->RepushFrame();
        return 0;
    }

    case WM_MENURBUTTONUP:
        // Right-click on a mirror entry: note which, then close the menu so the
        // confirmation is not competing with it for input.
        if (reinterpret_cast<HMENU>(lp) == mirrorListMenu_) {
            const UINT item = GetMenuItemID(mirrorListMenu_, static_cast<int>(wp));
            if (item != static_cast<UINT>(-1) && item >= kMirrorMenuBase) {
                rightClickedId_ = item - kMirrorMenuBase;
                EndMenu();
            }
        }
        return 0;

    case WM_TIMER:
        if (wp == kTimerSave) {
            SaveNow();
        } else if (wp == kTimerRestore) {
            KillTimer(hwnd_, kTimerRestore);
            restoreTimerActive_ = false;

            RunRestorePass();
            if (AnythingWaiting()) {
                restorePeriodMs_ = (std::min)(restorePeriodMs_ * 2, kRestoreMaxPeriodMs);
                restoreTimerActive_ = true;
                SetTimer(hwnd_, kTimerRestore, restorePeriodMs_, nullptr);
            }
        }
        return 0;

    case WM_RVM_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
            ShowTrayMenu();
        } else if (LOWORD(lp) == WM_LBUTTONDBLCLK) {
            manager_.Open(this);
        }
        return 0;

    case WM_HOTKEY:
        switch (static_cast<int>(wp)) {
        case kHotkeyNewMirror:    RequestNewMirror(); break;
        case kHotkeyClickThrough: SetClickThroughAll(!AllClickThrough()); break;
        case kHotkeyCloseAll:     ConfirmCloseAll(); break;
        default: break;
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd_);
        return 0;

    case WM_DESTROY:
        SaveNow();   // Placements, before anything is torn down.
        server_.Stop();   // Before the mirrors its frame tees point at.
        KillTimer(hwnd_, kTimerRestore);
        manager_.Destroy();
        // Retire rather than delete: this can run inside a nested loop that is
        // still executing a Mirror method further up the stack.
        while (!mirrors_.empty()) RetireMirror(mirrors_.size() - 1);
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

int App::Run(bool relaunched) {
    relaunched_ = relaunched;
    startedMs_ = GetTickCount64();
    if (!CaptureSupported()) {
        MessageBoxW(nullptr,
                    L"Windows Graphics Capture is not available on this system.\n"
                    L"Rear View Mirror needs Windows 10 version 1903 or newer.",
                    kAppName, MB_OK | MB_ICONERROR);
        return 1;
    }

    LogOpen(L"server");
    Gfx::Get().Init();

    if (!CreateOwnerWindow()) return 1;
    CreateTrayIcon();

    // Posted from whatever thread first sees the device go; handled below.
    Gfx::Get().SetDeviceLostHandler([hwnd = hwnd_] { PostMessageW(hwnd, WM_RVM_DEVICE_LOST, 0, 0); });

    const struct { int id; UINT key; const wchar_t* name; } hotkeys[] = {
        { kHotkeyNewMirror, 'M', L"Ctrl+Alt+M" },
        { kHotkeyClickThrough, 'T', L"Ctrl+Alt+T" },
        { kHotkeyCloseAll, 'X', L"Ctrl+Alt+X" },
    };
    for (const auto& h : hotkeys) {
        if (!RegisterHotKey(hwnd_, h.id, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, h.key)) {
            Log(L"app: hotkey %s is taken by another program (%lu)", h.name, GetLastError());
        }
    }

    foregroundHook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                                      &ForegroundChanged, 0, 0,
                                      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    RestoreSaved();

    streamSettings_ = LoadStreamSettings();
    if (!ApplyStreamSettings()) {
        if (streamSettings_.key.empty() && !streamSettings_.lockedKey.empty()) {
            ShowBalloon(L"Streaming is off: the saved key could not be decrypted by this Windows "
                        L"account. Open Streaming and enter it again.");
        } else {
            ShowBalloon(L"Streaming could not start on UDP port " +
                        std::to_wstring(streamSettings_.port) + L". Is another program using it?");
        }
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        retired_.clear();   // Only here, never in a nested loop: see retired_.
    }
    retired_.clear();

    if (foregroundHook_) UnhookWinEvent(foregroundHook_);
    g_appWindow = nullptr;
    UnregisterHotKey(hwnd_, kHotkeyNewMirror);
    UnregisterHotKey(hwnd_, kHotkeyClickThrough);
    UnregisterHotKey(hwnd_, kHotkeyCloseAll);
    RemoveTrayIcon();
    return static_cast<int>(msg.wParam);
}

}  // namespace rvm
