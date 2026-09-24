#pragma once
#include "overlay.h"

namespace rvm {

struct PickResult {
    HWND window = nullptr;   // The window clicked, if one was.
    bool desktop = false;    // The whole desktop instead: D, or a click on the wallpaper or taskbar.
};

// Full-screen "click the window you want" mode. Highlights whatever top-level
// window is under the cursor and returns it on click. The wallpaper and the
// taskbar stand for the whole desktop, as does pressing D.
// Neither is set if the user cancels with Esc or a right-click.
PickResult PickSource();

}  // namespace rvm
