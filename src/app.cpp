#include "app.h"
#include "crash.h"
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
    kHotkeyCloseAll,
};

enum TrayMenuId : UINT {
    kTrayNewMirror = 1,
    kTrayCloseAll,
    kTrayExit,
    kTrayManager,
};

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
    // Mirrors and streaming are managed from the manager window; the menu is
    // just the way in, plus the actions that must work without it.
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kTrayManager, L"Manage mirrors…");
    SetMenuDefaultItem(menu, kTrayManager, FALSE);   // Matches the double-click.
    AppendMenuW(menu, MF_STRING, kTrayNewMirror, L"New mirror…\tCtrl+Alt+M");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    const bool anything = !mirrors_.empty() || !pending_.empty();
    AppendMenuW(menu, MF_STRING | (anything ? 0 : MF_GRAYED), kTrayCloseAll,
                L"Close and forget all\tCtrl+Alt+X");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayExit, L"Exit");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd_);
    const UINT cmd = static_cast<UINT>(TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd_, nullptr));
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);

    switch (cmd) {
    case kTrayManager:   manager_.Open(this); break;
    case kTrayNewMirror: RequestNewMirror(); break;
    case kTrayCloseAll:  ConfirmCloseAll(); break;
    case kTrayExit:      PostMessageW(hwnd_, WM_CLOSE, 0, 0); break;
    default: break;
    }
}

void App::NewMirror() {
    // Picking and selecting run nested loops; a second request arriving
    // meanwhile (hotkey, second launch, manager button) must not start a
    // second, overlapping selection.
    if (selecting_) return;
    selecting_ = true;
    struct Reset { bool& flag; ~Reset() { flag = false; } } reset{ selecting_ };

    const PickResult pick = PickSource();
    if (pick.desktop) {
        NewDesktopMirror();
        return;
    }
    HWND target = pick.window;
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
    streaming_.Attach(*mirror);
    mirrors_.push_back(std::move(mirror));
    MarkDirty();
    manager_.Refresh();
}

// The whole desktop, or a region of it. Where it can be streamed, it is made
// without a window on screen: it is almost always there to be streamed, and a
// full-screen always-on-top copy of the screen would be in the way. The
// manager can show it. A build without streaming shows it, or it would be
// for nothing.
void App::NewDesktopMirror() {
    const RECT bounds = DesktopCapture::Bounds();
    const SIZE size{ RectW(bounds), RectH(bounds) };
    RECT crop{};
    if (!SelectScreenRegion(bounds, size, RECT{}, crop)) return;

    MirrorState state;
    state.source   = SourceKind::Desktop;
    state.crop     = crop;
    state.baseSize = size;
    state.hidden   = Streaming::Available();
    state.group    = NewGroup();

    auto mirror = std::make_unique<Mirror>(nextId_++);
    if (!mirror->Create(state, nullptr, hwnd_)) {
        MessageBoxW(nullptr, L"Could not start capturing the desktop.", kAppName,
                    MB_OK | MB_ICONWARNING);
        return;
    }
    streaming_.Attach(*mirror);
    mirrors_.push_back(std::move(mirror));
    MarkDirty();
    manager_.Refresh();
    if (Streaming::Available()) {
        ShowBalloon(streaming_.Running()
            ? L"The desktop is ready to stream. It has no window here; the manager can show one."
            : L"Desktop mirror added, without a window here. Turn on Streaming to share it.");
    }
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

void App::MarkDirty() {
    dirty_ = true;
    SetTimer(hwnd_, kTimerSave, kSaveDelayMs, nullptr);
    streaming_.MirrorsChanged();   // Every state change can change what clients may list.
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
        if (target && m->Target() == target && m->Group() != 0) return m->Group();
    }
    return NewGroup();
}

uint32_t App::NewGroup() const {
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
        if (it->enabled && it->source == SourceKind::Window) {
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
        streaming_.Attach(*mirror);
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

    case WM_DISPLAYCHANGE: {
        // Monitors came, went or changed resolution: desktop mirrors start
        // over on the new layout.
        bool any = false;
        for (auto& m : mirrors_) {
            if (m->IsDesktop()) {
                m->RestartDesktop();
                any = true;
            }
        }
        if (any) {
            streaming_.MirrorsChanged();
            manager_.Refresh();
        }
        break;
    }

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
        case kHotkeyCloseAll:     ConfirmCloseAll(); break;
        default: break;
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd_);
        return 0;

    case WM_DESTROY:
        SaveNow();   // Placements, before anything is torn down.
        streaming_.Shutdown();   // Before the mirrors its frame tees point at.
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
    InstallCrashHandler(L"server");
    Gfx::Get().Init();

    if (!CreateOwnerWindow()) return 1;
    CreateTrayIcon();

    // Posted from whatever thread first sees the device go; handled below.
    Gfx::Get().SetDeviceLostHandler([hwnd = hwnd_] { PostMessageW(hwnd, WM_RVM_DEVICE_LOST, 0, 0); });

    const struct { int id; UINT key; const wchar_t* name; } hotkeys[] = {
        { kHotkeyNewMirror, 'M', L"Ctrl+Alt+M" },
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

    Streaming::Hooks hooks;
    hooks.window  = hwnd_;
    hooks.mirrors = &mirrors_;
    hooks.changed = [this] { manager_.Refresh(); };
    hooks.notify  = [this](const std::wstring& text) { ShowBalloon(text); };
    streaming_.Start(std::move(hooks));

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
    UnregisterHotKey(hwnd_, kHotkeyCloseAll);
    RemoveTrayIcon();
    return static_cast<int>(msg.wParam);
}

}  // namespace rvm
