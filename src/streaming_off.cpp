#include "streaming.h"

// Built instead of streaming.cpp when RVM_STREAMING=OFF.

namespace rvm {

struct Streaming::Impl {};

Streaming::Streaming() = default;

Streaming::~Streaming() = default;

bool Streaming::Available() {
    return false;
}

void Streaming::Start(Hooks) {}

void Streaming::Shutdown() {}

void Streaming::Attach(Mirror&) {}

void Streaming::MirrorsChanged() {}

bool Streaming::Watched(uint32_t) const {
    return false;
}

bool Streaming::SetPaused(bool) {
    return true;
}

bool Streaming::Running() const {
    return false;
}

size_t Streaming::Clients() const {
    return 0;
}

void Streaming::ShowSettings() {}

}  // namespace rvm
