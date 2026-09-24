#pragma once
#include "client_config.h"

namespace rvm {

// Modal. True if the user pressed Connect and `settings` was filled in.
bool ShowConnectDialog(HWND owner, ConnectSettings& settings);

}  // namespace rvm
