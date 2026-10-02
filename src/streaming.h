#pragma once
#include "common.h"

namespace rvm {

class Mirror;

// The app's streaming seam: streaming.cpp, or streaming_off.cpp in a build
// without streaming (RVM_STREAMING=OFF).
class Streaming {
public:
    // What streaming needs from the app.
    struct Hooks {
        // The app window. A mirror's picture is requested by posting
        // WM_RVM_STREAM_WANT_FRAME here with its id.
        HWND window = nullptr;
        // Every mirror, for the list clients see; outlives this.
        const std::vector<std::unique_ptr<Mirror>>* mirrors = nullptr;
        // Running state or client count changed: redraw what shows them.
        std::function<void()> changed;
        // A short message for the user, as a tray balloon.
        std::function<void(const std::wstring&)> notify;
    };

    Streaming();
    ~Streaming();
    Streaming(const Streaming&) = delete;
    Streaming& operator=(const Streaming&) = delete;

    // False in a build without streaming: nothing to show or offer.
    static bool Available();

    // Loads the saved settings and starts serving if it was on last time.
    void Start(Hooks hooks);

    // Stops serving. Before the mirrors it takes frames from are destroyed.
    void Shutdown();

    // A new mirror: from now on its frames can be streamed. Safe before Start.
    void Attach(Mirror& mirror);

    // Mirrors were added, removed, switched or resized: republish the list,
    // if what clients see of it changed.
    void MirrorsChanged();

    // Whether any client watches the mirror; any thread, cheap.
    bool Watched(uint32_t mirrorId) const;

    // Paused, nothing is served and the port is free (for the sign-in
    // service); settings are untouched. False if unpausing should serve but
    // could not, for the caller to retry.
    bool SetPaused(bool paused);

    bool   Running() const;
    size_t Clients() const;

    // Switched on here: serving, or would be if not handed off or blocked.
    bool On() const;
    // HandedOff: paused while the sign-in service streams this PC, which it
    // does whether or not this is on. Blocked: on, but the port is taken.
    enum class State { Off, Serving, HandedOff, Blocked };
    State CurrentState() const;

    // The settings dialog, where serving is started and stopped.
    void ShowSettings();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace rvm
