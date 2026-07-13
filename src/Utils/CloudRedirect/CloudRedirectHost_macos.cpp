#include "Utils/CloudRedirect/CloudRedirectHost.h"

#include "Utils/Config/Config.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/Logging/Log.h"

#include <atomic>
#include <mutex>
#include <unordered_set>
#include <vector>

// macOS-native CloudRedirectHost backend.
//
// Windows hosts CloudRedirect's prebuilt cloud_redirect.dll at runtime and
// drives it through the CR_* exports (see CloudRedirectHost.cpp — 11 exports:
// CR_InitCloudSave, CR_HandleCloudRpc, CR_SetApps, CR_IsApp, CR_Shutdown
// required; CR_EnableStatsSync, CR_SetAccountId, CR_NotifyAppRunning,
// CR_NotifyStatsStored, CR_GetAchievements, CR_InstallVtableHooks optional).
// That DLL is a Windows PE and is not portable, so macOS re-implements the
// same CloudRedirectHost.h surface natively instead of loading anything.
//
// Initialize()/SyncAppSet()/IsActive()/IsApp() already mirror the Windows
// entry points' observable behaviour: gated on [cloud].enabled, tracking the
// same addappid() redirected-app set from LuaConfig. The actual achievement
// storage/sync backend is NOT implemented yet — GetAchievements() keeps the
// "no CR data" fallback (returns 0, same as the Windows DLL when it has
// nothing for that appid) and HandleCloudRpc() keeps the "not handled"
// fallback (caller chains to the original RPC path) until that backend
// exists. SetAccountId()/NotifyAppRunning() do record the inputs a future
// backend will need (scoping stored achievement state per Steam account,
// knowing which redirected app is currently running), since Windows already
// feeds them from the same call sites (Hooks_NetPacket.cpp).
namespace CloudRedirectHost {
namespace {

std::mutex g_mutex;
std::atomic<bool> g_active{false};
std::unordered_set<uint32_t> g_redirectedApps;
uint32_t g_accountId = 0;

void RefreshRedirectedApps() {
    std::vector<AppId_t> depots = LuaConfig::GetAllDepotIds();
    std::lock_guard lock(g_mutex);
    g_redirectedApps = std::unordered_set<uint32_t>(depots.begin(), depots.end());
}

} // namespace

void Initialize(const char* /*steamInstallPath*/) {
    const Config::CloudSettings cloud = Config::GetCloudSettings();
    if (!cloud.enabled) {
        LOG_INFO("CloudRedirect: [cloud].enabled is false, cloud save redirection disabled");
        return;
    }

    if (g_active.exchange(true)) return;
    LOG_INFO("CloudRedirect: enabled (native macOS backend; achievement/cloud-save storage not implemented yet)");

    RefreshRedirectedApps();
}

void SyncAppSet() {
    if (!g_active.load(std::memory_order_acquire)) return;
    RefreshRedirectedApps();
    LOG_DEBUG("CloudRedirect: re-synced redirected app set");
}

bool IsActive() {
    return g_active.load(std::memory_order_acquire);
}

bool IsApp(uint32_t appId) {
    if (!g_active.load(std::memory_order_acquire)) return false;
    std::lock_guard lock(g_mutex);
    return g_redirectedApps.count(appId) != 0;
}

bool HandleCloudRpc(const char* /*method*/, uint32_t /*appId*/, uint32_t /*accountId*/,
                    const uint8_t* /*reqBody*/, uint32_t /*reqLen*/,
                    uint8_t* /*respBuf*/, uint32_t /*respMaxLen*/,
                    uint32_t* /*respLen*/, int32_t* /*eresult*/) {
    // TODO: no native cloud-save sync backend yet — not handled, caller
    // chains to the original RPC path (same fallback as Windows without CR).
    return false;
}

void SetAccountId(uint32_t accountId) {
    if (!g_active.load(std::memory_order_acquire)) return;
    g_accountId = accountId;
}

void NotifyAppRunning(uint32_t /*appId*/, bool /*running*/) {
    // TODO: wire into the achievement/cloud-save backend once it exists.
}

void NotifyStatsStored(uint32_t /*appId*/) {
    // TODO: wire into the achievement/cloud-save backend once it exists.
}

uint32_t GetAchievements(uint32_t /*appId*/, AchievementBlock* /*out*/, uint32_t /*maxBlocks*/) {
    // TODO: no native achievement storage backend yet — 0 blocks written.
    return 0;
}

void Shutdown() {
    if (!g_active.exchange(false)) return;
    std::lock_guard lock(g_mutex);
    g_redirectedApps.clear();
    g_accountId = 0;
    LOG_INFO("CloudRedirect: shut down");
}

namespace {
// macOS has no DLL_PROCESS_DETACH equivalent (see LuaFileWatcher_macos.cpp);
// this guard's destructor is the process-exit analogue of Windows dllmain.cpp
// calling CloudRedirectHost::Shutdown() on DLL_PROCESS_DETACH.
struct ExitGuard final {
    ~ExitGuard() { Shutdown(); }
};
ExitGuard g_exitGuard;
} // namespace

}
