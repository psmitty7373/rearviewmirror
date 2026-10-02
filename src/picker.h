#pragma once
#include "overlay.h"

namespace rvm {

struct PickResult {
    HWND window = nullptr;   // The window clicked, if one was.
    bool desktop = false;    // The whole desktop instead: D, or a click on the wallpaper or taskbar.
};

// "Click the window you want" mode: highlights the top-level window under the
// cursor and returns it on click. Neither field is set if cancelled (Esc or a
// right-click).
PickResult PickSource();

}  // namespace rvm
