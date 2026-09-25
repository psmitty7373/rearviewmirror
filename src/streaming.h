#pragma once
#include "common.h"

namespace rvm {

class Mirror;

// Everything the app does for streaming, behind one seam. streaming.cpp is the
// real thing; streaming_off.cpp, compiled instead when the build leaves
// streaming out (RVM_STREAMING=OFF), does nothing and reports it unavailable.
// Nothing else in the app needs to know which one it got.
class Streaming {
public:
    // What streaming needs from the app.
    struct Hooks {
        // The app window. A mirror's picture is requested by posting
        // WM_RVM_STREAM_WANT_FRAME here with its id.
        HWND window = nullptr;
        // Every mirror, for the list clients see. Owned by the app, which
        // outlives this.
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

    // Mirrors were added, removed, switched or resized: republish the list.
    void MirrorsChanged();

    bool   Running() const;
    size_t Clients() const;

    // The settings dialog, where serving is started and stopped.
    void ShowSettings();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace rvm
