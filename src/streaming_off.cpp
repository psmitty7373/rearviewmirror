#include "streaming.h"

// Built instead of streaming.cpp when streaming is left out
// (RVM_STREAMING=OFF): mirrors only, nothing on the network.

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

bool Streaming::Running() const {
    return false;
}

size_t Streaming::Clients() const {
    return 0;
}

void Streaming::ShowSettings() {}

}  // namespace rvm
