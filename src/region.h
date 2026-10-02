#pragma once
#include "overlay.h"

namespace rvm {

// Lets the user drag a sub-rectangle over `target`. `initial` (may be empty)
// and `out` are in the pixels of a capture `captureSize` big. False if
// cancelled, or if the target cannot be shown.
bool SelectRegion(HWND target, SIZE captureSize, RECT initial, RECT& out);

// The same over a rectangle of the screen, such as the whole desktop, whose
// capture texture is `captureSize`.
bool SelectScreenRegion(RECT bounds, SIZE captureSize, RECT initial, RECT& out);

}  // namespace rvm
