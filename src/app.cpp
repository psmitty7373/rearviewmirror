#include "app.h"
#include "crash.h"
#include "login_handoff.h"
#include "picker.h"
#include "region.h"

namespace rvm {

namespace {

constexpr UINT_PTR kTimerRestore = 1;
constexpr UINT_PTR kTimerSave    = 2;
constexpr UINT_PTR kTimerHandoff = 3;
constexpr UINT_PTR kTimerIdle    = 4;
// Grace before an unwatched hidden mirror sleeps, so a viewer who comes
// straight back does not cost a restart.
constexpr UINT kIdleGraceMs = 3000;
// Away from the console, re-check: notifications can race session switches,
// and the service can start or stop.
constexpr UINT kHandoffRecheckMs = 5000;
// The service's helper lets go of the port a moment after the session returns.
constexpr UINT kResumeRetryMs = 1000;
constexpr int  kResumeTries   = 30;
// Waiting mirrors are polled for good, backing off: a source can reopen any time.
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

bool StartsOrStopsCapture(UINT msg, WPARAM wp) {
    switch (msg) {
    case WM_DISPLAYCHANGE:
    case WM_RVM_NEW_MIRROR:
    case WM_RVM_MIRROR_CLOSED:
    case WM_RVM_MIRROR_ORPHANED:
    case WM_RVM_MIRROR_RESTART:
    case WM_RVM_STREAM_WANT_FRAME:
    case WM_RVM_FOREGROUND_CHANGED:
    case WM_RVM_TRAY:
    case WM_HOTKEY:
    case WM_CLOSE:
        return true;
    case WM_TIMER:
        return wp == kTimerRestore || wp == kTimerIdle;
    default:
        return false;
    }
}

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

    // Not message-only: a popup menu dismisses on an outside click only if its
    // owner can take the foreground.
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
    // Picking runs a nested loop; a second request meanwhile must not overlap it.
    if (selecting_) return;
    selecting_ = true;
    struct Reset { bool& flag; ~Reset() { flag = false; } } reset{ selecting_ };

    // Beyond what the file holds, a mirror would be lost on the next save.
    if (mirrors_.size() + pending_.size() >= static_cast<size_t>(kMaxMirrors)) {
        const std::wstring text = L"There are already " + std::to_wstring(kMaxMirrors) +
                                  L" mirrors, counting those waiting for their apps, which is "
                                  L"as many as Rear View Mirror keeps. Close one to make another.";
        MessageBoxW(nullptr, text.c_str(), kAppName, MB_OK | MB_ICONINFORMATION);
        return;
    }

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
    if (!CreateMirror(*mirror, state, target)) {
        MessageBoxW(nullptr, L"Could not start capturing that window.", kAppName,
                    MB_OK | MB_ICONWARNING);
        return;
    }
    streaming_.Attach(*mirror);
    mirrors_.push_back(std::move(mirror));
    MarkDirty();
    manager_.Refresh();
}

// Made hidden where it can be streamed: a full-screen topmost copy of the
// screen would be in the way.
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
    if (!CreateMirror(*mirror, state, nullptr)) {
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

App::Transition::~Transition() {
    if (--app.transitions_ > 0) return;
    for (const Deferred& d : std::exchange(app.deferred_, {})) {
        PostMessageW(app.hwnd_, d.msg, d.wp, d.lp);
    }
}

void App::Defer(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DISPLAYCHANGE) wp = lp = 0;   // One restart covers any number of changes.
    const bool queued = std::any_of(deferred_.begin(), deferred_.end(), [&](const Deferred& d) {
        return d.msg == msg && d.wp == wp && d.lp == lp;
    });
    if (!queued) deferred_.push_back(Deferred{ msg, wp, lp });
}

bool App::CreateMirror(Mirror& mirror, const MirrorState& state, HWND target) {
    Transition transition(*this);
    return mirror.Create(state, target, hwnd_);
}

// Tears the mirror down now and deletes it later (see retired_). It leaves the
// list first, so nothing handled while it stops can find it.
void App::RetireMirror(size_t index) {
    if (index >= mirrors_.size()) return;
    Mirror* mirror = mirrors_[index].get();
    retired_.push_back(std::move(mirrors_[index]));
    mirrors_.erase(mirrors_.begin() + static_cast<ptrdiff_t>(index));
    Transition transition(*this);
    mirror->Destroy();
}

void App::CloseMirror(uint32_t id) {
    if (transitions_ > 0) {
        Defer(WM_RVM_MIRROR_CLOSED, id, 0);
        return;
    }
    for (size_t i = 0; i < mirrors_.size(); ++i) {
        if (mirrors_[i]->Id() == id) {
            RetireMirror(i);
            MarkDirty();
            manager_.Refresh();
            return;
        }
    }
}

// Saves and relaunches onto a new device. A loss soon after a relaunch is
// reported instead, so a broken GPU cannot cause a restart loop.
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

// Forgets waiting mirrors too, and the hotkey is system-wide: confirm first.
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
    UpdateForegroundHook();
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
    for (const auto& p : pending_) states.push_back(p.state);
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

// The group of a mirror already showing the window, else a new one.
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
    if (transitions_ > 0) return true;
    Transition transition(*this);
    return mirror.SetEnabled(on, TargetsOfOtherGroups(mirror.Group()),
                             TargetOfGroup(mirror.Group(), &mirror));
}

int App::TryRebindOrphans(ExeNameCache& exes) {
    Transition transition(*this);
    // By id: a nested message can retire a mirror meanwhile.
    std::vector<uint32_t> ids;
    for (const auto& m : mirrors_) {
        if (m->Orphaned()) ids.push_back(m->Id());
    }
    int rebound = 0;
    for (const uint32_t id : ids) {
        Mirror* m = FindMirror(id);
        if (m && m->TryRebind(TargetsOfOtherGroups(m->Group()), TargetOfGroup(m->Group(), m), &exes)) {
            ++rebound;
        }
    }
    return rebound;
}

// Hidden mirrors that had frames nobody wanted, and still nobody watches.
void App::SleepIdleMirrors() {
    Transition transition(*this);
    std::vector<uint32_t> ids;
    for (const auto& m : mirrors_) {
        if (m->TakeIdleReport()) ids.push_back(m->Id());
    }
    for (const uint32_t id : ids) {
        if (Mirror* m = FindMirror(id); m && !streaming_.Watched(id)) m->Sleep();
    }
}

bool App::AnythingWaiting() const {
    if (!pending_.empty()) return true;
    for (const auto& m : mirrors_) {
        if (m->Orphaned()) return true;
    }
    return false;
}

void App::EnsureRestoreTimer() {
    UpdateForegroundHook();
    if (restoreTimerActive_ || !AnythingWaiting()) return;
    restorePeriodMs_ = kRestoreMinPeriodMs;
    restoreTimerActive_ = true;
    SetTimer(hwnd_, kTimerRestore, restorePeriodMs_, nullptr);
}

// Every foreground change system-wide wakes this thread, so only while it helps.
void App::UpdateForegroundHook() {
    const bool want = AnythingWaiting();
    if (want && !foregroundHook_) {
        foregroundHook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                                          &ForegroundChanged, 0, 0,
                                          WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    } else if (!want && foregroundHook_) {
        UnhookWinEvent(std::exchange(foregroundHook_, nullptr));
    }
}

void App::RestartDesktops() {
    bool any = false;
    {
        Transition transition(*this);
        std::vector<uint32_t> ids;
        for (const auto& m : mirrors_) {
            if (m->IsDesktop()) ids.push_back(m->Id());
        }
        for (const uint32_t id : ids) {
            if (Mirror* m = FindMirror(id)) {
                m->RestartDesktop();
                any = true;
            }
        }
    }
    if (any) {
        streaming_.MirrorsChanged();
        manager_.Refresh();
    }
}

#if RVM_LOGIN_SERVICE
void App::UpdateHandoff() {
    const bool away = AwayFromConsole();
    const bool handOff = away && LoginServiceRunning();
    if (handOff != handedOff_) {
        Log(handOff ? L"app: this session is not on the console; the sign-in service streams it"
                    : L"app: no longer handed off to the sign-in service; streaming resumes");
        handedOff_ = handOff;
        resumeTries_ = 0;
    }
    KillTimer(hwnd_, kTimerHandoff);
    if (streaming_.SetPaused(handOff)) {
        resumeTries_ = 0;
        if (away) SetTimer(hwnd_, kTimerHandoff, kHandoffRecheckMs, nullptr);
        return;
    }
    if (++resumeTries_ < kResumeTries) {
        SetTimer(hwnd_, kTimerHandoff, kResumeRetryMs, nullptr);
    } else {
        resumeTries_ = 0;
        ShowBalloon(L"Streaming could not start: its UDP port is still in use.");
    }
}
#endif

// No balloon: the mirror reappearing says enough.
int App::RunRestorePass() {
    const ULONGLONG now = GetTickCount64();
    if (now - lastRestorePassTick_ < kRestorePassMinGapMs) return 0;
    lastRestorePassTick_ = now;

    ExeNameCache exes;   // Shared by every search in the pass.
    const int restored = TryRestorePending(exes) + TryRebindOrphans(exes);
    if (restored > 0) {
        MarkDirty();
        manager_.Refresh();
        UpdateForegroundHook();
    }
    return restored;
}

int App::TryRestorePending(ExeNameCache& exes) {
    Transition transition(*this);
    // By id, found again after each Create, which can pump messages.
    std::vector<uint32_t> ids;
    for (const auto& p : pending_) ids.push_back(p.id);
    const auto find = [this](uint32_t id) {
        return std::find_if(pending_.begin(), pending_.end(),
                            [id](const Pending& p) { return p.id == id; });
    };
    int restored = 0;
    for (const uint32_t id : ids) {
        auto it = find(id);
        if (it == pending_.end()) continue;
        HWND target = nullptr;
        if (it->state.enabled && it->state.source == SourceKind::Window) {
            // The group's window if shown, else a search outside other groups'.
            target = TargetOfGroup(it->state.group, nullptr);
            if (!target) target = FindMatchingWindow(it->state, TargetsOfOtherGroups(it->state.group), &exes);
            if (!target) continue;   // Kept for the next attempt.
        }
        // Saved disabled: listed now, bound when switched on.
        auto mirror = std::make_unique<Mirror>(id);
        const MirrorState state = it->state;
        if (!mirror->Create(state, target, hwnd_)) continue;
        it = find(id);
        if (it == pending_.end()) {   // Forgotten meanwhile.
            mirror->Destroy();
            continue;
        }
        pending_.erase(it);
        streaming_.Attach(*mirror);
        mirrors_.push_back(std::move(mirror));
        ++restored;
    }
    return restored;
}

void App::RestoreSaved() {
    for (MirrorState& state : LoadMirrorStates()) {
        pending_.push_back(Pending{ nextId_++, std::move(state) });
    }
    if (pending_.empty()) {
        // Nothing to bring back: show the manager, unless relaunched.
        if (!relaunched_) manager_.Open(this);
        return;
    }

    ExeNameCache exes;
    const int restored = TryRestorePending(exes);
    if (restored > 0) ShowBalloon(L"Restored " + Plural(restored, L"mirror", L"mirrors") + L".");

    EnsureRestoreTimer();
    MarkDirty();
}

LRESULT App::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    // Explorer restarted, dropping every tray icon.
    static const UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (msg == taskbarCreated && taskbarCreated != 0) {
        trayAdded_ = false;
        CreateTrayIcon();
        return 0;
    }

    if (transitions_ > 0 && StartsOrStopsCapture(msg, wp)) {
        Defer(msg, wp, lp);
        return 0;
    }

    switch (msg) {
    case WM_RVM_NEW_MIRROR:
        NewMirror();
        return 0;

    case WM_RVM_SHOW_MANAGER:
        manager_.Open(this);
        return 0;

    case WM_DISPLAYCHANGE:
        RestartDesktops();
        break;

    case WM_RVM_MIRROR_CLOSED:
        CloseMirror(static_cast<uint32_t>(wp));
        return 0;

    case WM_RVM_STATE_CHANGED:
        MarkDirty();
        manager_.Refresh();
        return 0;

    case WM_RVM_MIRROR_ORPHANED:
        // Its source closed, or a desktop restart failed and it already waits.
        if (Mirror* m = FindMirror(static_cast<uint32_t>(wp)); m && m->Enabled() && !m->Orphaned()) {
            Transition transition(*this);
            m->Orphan();
        }
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

    case WM_RVM_MIRROR_IDLE:
        SetTimer(hwnd_, kTimerIdle, kIdleGraceMs, nullptr);
        return 0;

    case WM_RVM_MIRROR_RESTART:
        if (Mirror* m = FindMirror(static_cast<uint32_t>(wp))) {
            Transition transition(*this);
            m->RestartCapture();
        }
        return 0;

    case WM_RVM_STREAM_WANT_FRAME:
        if (Mirror* m = FindMirror(static_cast<uint32_t>(wp)); m && !m->Sleeping()) {
            m->RepushFrame();
        } else if (m && streaming_.Watched(m->Id())) {
            Transition transition(*this);
            m->RestartCapture();
            m->RepushFrame();   // A minimized window's capture sends nothing at first.
        }
        return 0;

#if RVM_LOGIN_SERVICE
    case WM_WTSSESSION_CHANGE:
        UpdateHandoff();
        return 0;
#endif

    case WM_TIMER:
#if RVM_LOGIN_SERVICE
        if (wp == kTimerHandoff) {
            UpdateHandoff();
            return 0;
        }
#endif
        if (wp == kTimerSave) {
            SaveNow();
        } else if (wp == kTimerIdle) {
            KillTimer(hwnd_, kTimerIdle);
            SleepIdleMirrors();
        } else if (wp == kTimerRestore) {
            KillTimer(hwnd_, kTimerRestore);
            restoreTimerActive_ = false;

            RunRestorePass();
            if (AnythingWaiting()) {
                restorePeriodMs_ = (std::min)(restorePeriodMs_ * 2, kRestoreMaxPeriodMs);
                restoreTimerActive_ = true;
                SetTimer(hwnd_, kTimerRestore, restorePeriodMs_, nullptr);
            }
            UpdateForegroundHook();
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
        KillTimer(hwnd_, kTimerIdle);
#if RVM_LOGIN_SERVICE
        KillTimer(hwnd_, kTimerHandoff);
        WatchSessionChanges(hwnd_, false);
#endif
        manager_.Destroy();
        // Retired, not deleted: a nested loop may still be inside a Mirror.
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

    RestoreSaved();

    Streaming::Hooks hooks;
    hooks.window  = hwnd_;
    hooks.mirrors = &mirrors_;
    hooks.changed = [this] { manager_.Refresh(); };
    hooks.notify  = [this](const std::wstring& text) { ShowBalloon(text); };
#if RVM_LOGIN_SERVICE
    // Paused at first: the service's helper may still hold the port, or this
    // session may not be the console's. UpdateHandoff serves when it can.
    streaming_.SetPaused(true);
    streaming_.Start(std::move(hooks));
    WatchSessionChanges(hwnd_, true);
    UpdateHandoff();
#else
    streaming_.Start(std::move(hooks));
#endif

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        retired_.clear();   // Only here, never in a nested loop: see retired_.
    }
    retired_.clear();

    if (foregroundHook_) UnhookWinEvent(std::exchange(foregroundHook_, nullptr));
    g_appWindow = nullptr;
    UnregisterHotKey(hwnd_, kHotkeyNewMirror);
    UnregisterHotKey(hwnd_, kHotkeyCloseAll);
    RemoveTrayIcon();
    return static_cast<int>(msg.wParam);
}

}  // namespace rvm
