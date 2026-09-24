#pragma once
#include "overlay.h"

namespace rvm {

// Full-screen "click the window you want" mode. Highlights whatever top-level
// window is under the cursor and returns it on click; nullptr if the user
// cancels with Esc or a right-click.
HWND PickWindow();

}  // namespace rvm
