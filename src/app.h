#pragma once
#include "manager.h"
#include "mirror.h"
#include "streaming.h"

namespace rvm {

// Owns the tray presence, the hotkeys, the saved-mirror file and every mirror.
class App : public WindowHost {
public:
    int Run(bool relaunched = false);

    size_t  MirrorCount() const { return mirrors_.size(); }
    Mirror* MirrorAt(size_t index);
    Mirror* FindMirror(uint32_t id);
    void    CloseMirror(uint32_t id);
    void    RequestNewMirror();

    // For the manager's Streaming button.
    bool   StreamingAvailable() const { return Streaming::Available(); }
    Streaming::State StreamingState() const { return streaming_.CurrentState(); }
    bool   StreamingOn() const { return streaming_.On(); }
    size_t StreamClients() const { return streaming_.Clients(); }
    void   ShowStreamSettings() { streaming_.ShowSettings(); }

    // Switching a mirror on binds it under the same rules as a restore.
    // Ignored, as no failure, while a capture is starting or stopping.
    bool   SetMirrorEnabled(Mirror& mirror, bool on);

    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);

private:
    bool CreateOwnerWindow();
    void CreateTrayIcon();
    void RemoveTrayIcon();
    void ShowTrayMenu();
    void ShowBalloon(const std::wstring& text);

    void NewMirror();
    void NewDesktopMirror();
    bool CreateMirror(Mirror& mirror, const MirrorState& state, HWND target);
    void CloseAll();
    void ConfirmCloseAll();
    void OnDeviceLost();
    void RetireMirror(size_t index);

    // Saves are coalesced on a short timer.
    void MarkDirty();
    void SaveNow();

    // Capture starts and stops pump messages, and nesting them deadlocks WGC:
    // each runs in a Transition, and messages that would start or stop one are
    // re-posted when the outermost ends.
    struct Transition {
        explicit Transition(App& owner) : app(owner) { ++app.transitions_; }
        ~Transition();
        App& app;
    };
    struct Deferred { UINT msg; WPARAM wp; LPARAM lp; };
    void Defer(UINT msg, WPARAM wp, LPARAM lp);
    int  transitions_ = 0;
    std::vector<Deferred> deferred_;

    // Desktop mirrors start over after a display change.
    void RestartDesktops();

#if RVM_LOGIN_SERVICE
    // Lends the streaming port to the sign-in service (see login_handoff.h).
    void UpdateHandoff();
    bool handedOff_ = false;
    int  resumeTries_ = 0;
#endif

    void RestoreSaved();
    int  TryRestorePending(ExeNameCache& exes);
    int  TryRebindOrphans(ExeNameCache& exes);
    int  RunRestorePass();
    bool AnythingWaiting() const;
    void EnsureRestoreTimer();
    void UpdateForegroundHook();
    void SleepIdleMirrors();
    // Mirrors of one window share a group (see MirrorState::group).
    std::vector<HWND> TargetsOfOtherGroups(uint32_t group) const;
    HWND TargetOfGroup(uint32_t group, const Mirror* except) const;
    uint32_t GroupForWindow(HWND target) const;
    uint32_t NewGroup() const;

    // Before the mirrors, so destroyed after them: they feed it frames.
    Streaming streaming_;

    std::vector<std::unique_ptr<Mirror>> mirrors_;

    // Torn-down mirrors awaiting deletion: a nested loop (context menu, region
    // selection) may still be inside one. The outer loop frees them.
    std::vector<std::unique_ptr<Mirror>> retired_;

    ManagerWindow manager_;

    // Saved mirrors whose source has not turned up yet, with the id each will
    // get. Orphaned live mirrors wait on the same timer.
    struct Pending { uint32_t id; MirrorState state; };
    std::vector<Pending> pending_;
    UINT restorePeriodMs_ = 2000;
    bool restoreTimerActive_ = false;
    ULONGLONG lastRestorePassTick_ = 0;

    // A reopened app takes the foreground: the cue to rebind before the next poll.
    HWINEVENTHOOK foregroundHook_ = nullptr;

    uint32_t nextId_ = 1;
    HICON iconLarge_ = nullptr;
    HICON iconSmall_ = nullptr;
    bool  trayAdded_ = false;
    bool  dirty_ = false;
    bool  relaunched_ = false;          // Started to replace a process whose device was lost.
    bool  deviceLostHandled_ = false;
    bool  selecting_ = false;           // A pick or region selection is running.
    ULONGLONG startedMs_ = 0;
};

}  // namespace rvm
