#pragma once
#include "manager.h"
#include "mirror.h"
#include "stream_server.h"

namespace rvm {

// Owns the tray presence, the hotkeys, the saved-mirror file and every mirror.
class App : public WindowHost {
public:
    int Run(bool relaunched = false);

    // Used by the manager window to drive the mirrors it lists.
    size_t  MirrorCount() const { return mirrors_.size(); }
    Mirror* MirrorAt(size_t index);
    Mirror* FindMirror(uint32_t id);
    void    CloseMirror(uint32_t id);
    void    RequestNewMirror();

    // The streaming server, for the manager's header button, which opens the
    // settings dialog where it is started and stopped.
    bool   StreamingOn() const { return server_.Running(); }
    size_t StreamClients() const { return server_.ClientCount(); }
    void   ShowStreamSettings();

    // Switching a mirror on binds it under the same rules as a restore.
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
    void CloseAll();
    void ConfirmCloseAll();
    void OnDeviceLost();
    void RetireMirror(size_t index);

    // Saves are coalesced: a hotkey that touches every mirror would otherwise
    // rewrite the file once per mirror.
    void MarkDirty();
    void SaveNow();

    void RestoreSaved();
    int  TryRestorePending();
    int  TryRebindOrphans();
    int  RunRestorePass();
    bool AnythingWaiting() const;
    void EnsureRestoreTimer();
    // Mirrors of one window share a group (see MirrorState::group).
    std::vector<HWND> TargetsOfOtherGroups(uint32_t group) const;
    HWND TargetOfGroup(uint32_t group, const Mirror* except) const;
    uint32_t GroupForWindow(HWND target) const;
    uint32_t NewGroup() const;

    bool ApplyStreamSettings();
    void PushMirrorList();
    void AttachStream(Mirror& mirror);

    StreamServer   server_;
    StreamSettings streamSettings_;

    std::vector<std::unique_ptr<Mirror>> mirrors_;

    // Torn-down mirrors awaiting deletion. A context menu or a region selection
    // runs a nested message loop from inside a Mirror method, so the object has
    // to outlive any such call; the outer loop frees these between messages.
    std::vector<std::unique_ptr<Mirror>> retired_;

    ManagerWindow manager_;

    // Saved mirrors whose source has not turned up yet. They stay on disk, so a
    // mirror survives the app it watches not running. Live mirrors whose source
    // has since closed wait the same way, as orphans, on the same timer.
    std::vector<MirrorState> pending_;
    UINT restorePeriodMs_ = 2000;
    bool restoreTimerActive_ = false;
    ULONGLONG lastRestorePassTick_ = 0;

    // Reopening an app brings its window to the foreground, so that event is
    // the cue to rebind right away instead of waiting for the next poll.
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
