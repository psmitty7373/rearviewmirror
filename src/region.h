#pragma once
#include "overlay.h"

namespace rvm {

// Lets the user drag a sub-rectangle over `target`. `captureSize` is the size
// of the capture texture and `out` is written in those same pixels, so the
// renderer can crop with it directly. `initial` may be empty. False if
// cancelled, or if the target cannot be shown.
bool SelectRegion(HWND target, SIZE captureSize, RECT initial, RECT& out);

}  // namespace rvm
