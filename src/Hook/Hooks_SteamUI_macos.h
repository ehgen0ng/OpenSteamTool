#pragma once

#include "Hooks_SteamUI.h"

// macOS-only extension implemented by Hooks_SteamUI_macos.cpp. The grant is
// queued because steamui cache updates must run on its UI thread.

namespace Hooks_SteamUI {
    // Queues an appId to have its steamui-side CSteamApp cache entry marked as owned.
    void QueueGrant(AppId_t appId);
}
