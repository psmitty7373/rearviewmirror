#include "streaming.h"
#include "mirror.h"
#include "stream_server.h"

namespace rvm {

namespace {

bool SameList(const std::vector<MirrorInfo>& a, const std::vector<MirrorInfo>& b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const MirrorInfo& x, const MirrorInfo& y) {
        return x.id == y.id && x.width == y.width && x.height == y.height &&
               x.controllable == y.controllable && x.name == y.name;
    });
}

}  // namespace

struct Streaming::Impl {
    Hooks          hooks;
    StreamServer   server;
    StreamSettings settings;
    bool           paused = false;
    std::vector<MirrorInfo> published;

    void Changed() {
        if (hooks.changed) hooks.changed();
    }
    void Notify(const std::wstring& text) {
        if (hooks.notify) hooks.notify(text);
    }

    // `always` after a restart: the server's clients are new.
    void PushMirrorList(bool always) {
        if (!hooks.mirrors) return;
        std::vector<MirrorInfo> list;
        for (const auto& m : *hooks.mirrors) {
            if (!m->Enabled() || m->Orphaned()) continue;
            const SIZE native = m->NativeSize();
            MirrorInfo info{ m->Id(), m->DisplayName(),
                             static_cast<UINT>((std::max)(native.cx, 0L)),
                             static_cast<UINT>((std::max)(native.cy, 0L)) };
#if RVM_REMOTE_CONTROL
            info.controllable = m->IsFullDesktop();
#endif
            list.push_back(std::move(info));
        }
        if (!always && SameList(list, published)) return;
        published = list;
        server.SetMirrorList(std::move(list));
    }

    // Stops the server, and starts it again if the settings say it should run
    // and it is not paused. False if it should run but could not.
    bool Apply() {
        server.Stop();
        Changed();
        if (!settings.enabled || paused) return true;
        // The server asks on its network thread; the mirror lives on the app's.
        server.SetFrameRequester([hwnd = hooks.window](uint32_t mirrorId) {
            PostMessageW(hwnd, WM_RVM_STREAM_WANT_FRAME, mirrorId, 0);
        });
        if (!server.Start(settings)) return false;
        PushMirrorList(/*always=*/true);
        Changed();
        return true;
    }
};

Streaming::Streaming() : impl_(std::make_unique<Impl>()) {}

Streaming::~Streaming() = default;

bool Streaming::Available() {
    return true;
}

void Streaming::Start(Hooks hooks) {
    impl_->hooks = std::move(hooks);
    impl_->settings = LoadStreamSettings();
    const bool lockedKey = impl_->settings.key.empty() && !impl_->settings.lockedKey.empty();
    if (lockedKey && impl_->settings.enabled) {
        impl_->Notify(L"Streaming is off: the saved key could not be decrypted by this Windows "
                      L"account. Open Streaming and enter it again.");
        return;
    }
    if (!impl_->Apply()) {
        impl_->Notify(L"Streaming could not start on UDP port " +
                      std::to_wstring(impl_->settings.port) + L". Is another program using it?");
    }
}

bool Streaming::SetPaused(bool paused) {
    Impl& s = *impl_;
    if (s.paused == paused && (paused || s.server.Running())) return true;
    s.paused = paused;
    // With no usable key there is nothing to retry; Start said why.
    if (!paused && (!s.settings.enabled || s.settings.key.empty())) return true;
    return s.Apply();
}

void Streaming::Shutdown() {
    impl_->server.Stop();
}

// The streaming tee. The sink outlives every mirror: the server is destroyed
// with the app, after all of them.
void Streaming::Attach(Mirror& mirror) {
    const uint32_t id = mirror.Id();
    StreamServer* server = &impl_->server;
    mirror.SetFrameSink(
        [server, id](ID3D11Texture2D* cache, const RECT& crop) { server->SubmitFrame(id, cache, crop); },
        [server, id] { return server->Watched(id); });
}

void Streaming::MirrorsChanged() {
    impl_->PushMirrorList(/*always=*/false);
}

bool Streaming::Watched(uint32_t mirrorId) const {
    return impl_->server.Watched(mirrorId);
}

bool Streaming::Running() const {
    return impl_->server.Running();
}

size_t Streaming::Clients() const {
    return impl_->server.ClientCount();
}

void Streaming::ShowSettings() {
    Impl& s = *impl_;
    StreamControl control;
    control.start = [&s](const StreamSettings& edited, std::wstring& error) {
        s.settings = edited;
        s.settings.enabled = true;
        const bool ok = s.Apply();
        if (!ok) {
            s.settings.enabled = false;
            error = L"Streaming could not start on UDP port " + std::to_wstring(edited.port) +
                    L". Is another program using it?";
        }
        SaveStreamSettings(s.settings);
        return ok;
    };
    control.stop = [&s] {
        s.settings.enabled = false;
        SaveStreamSettings(s.settings);
        s.Apply();
    };
    control.running = [&s] { return s.server.Running(); };
    control.status = [&s] {
        if (!s.server.Running()) return std::wstring(L"Stopped");
        return L"Running on UDP port " + std::to_wstring(s.settings.port) + L"  ·  " +
               Plural(static_cast<int>(s.server.ClientCount()), L"client", L"clients") +
               L" connected";
    };

    StreamSettings edited = s.settings;
    if (!ShowStreamSettingsDialog(s.hooks.window, edited, control)) return;

    // OK: keep the fields. A running server picks up changed ones by restarting;
    // its clients reconnect by themselves.
    const StreamSettings before = s.settings;
    s.settings = edited;
    SaveStreamSettings(s.settings);
    const bool changed = before.port != edited.port || before.key != edited.key ||
                         before.bitrateKbps != edited.bitrateKbps || before.fps != edited.fps ||
                         before.preset != edited.preset;
    if (s.server.Running() && changed && !s.Apply()) {
        s.Notify(L"Streaming could not restart on UDP port " + std::to_wstring(s.settings.port) +
                 L". Is another program using it?");
        s.settings.enabled = false;
        SaveStreamSettings(s.settings);
    }
}

}  // namespace rvm
