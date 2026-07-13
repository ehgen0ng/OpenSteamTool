#include "VehCommon.h"

#include <dobby.h>

#include <mutex>
#include <unordered_map>

// macOS backend for VehCommon (mirrors the Windows VehCommon.cpp).
//
// Windows arms a 0xCC soft-breakpoint at a function's first byte and reads
// the trapped CPU state from a Vectored Exception Handler; onHit is the one
// dispatch point shared by every use — CaptureRcxTo (ARM_CAPTURE_C) and any
// custom interceptor (ARM_INT3_C, e.g. OnSpawnProcessHit) alike. arm64 has no
// int3/VEH equivalent, so here Arm() uses Dobby's instrument to intercept the
// target on entry, wraps the raw register context in a Trap::Context, and
// calls site.onHit exactly like the Windows dispatcher does — so both macros
// share the same onHit-based semantics on both platforms instead of only
// covering the narrow "capture this" case.
//
// Dobby's instrument fires on every call to the target, which already
// matches Windows' persistent=true re-arm behavior; for one-shot
// (persistent=false) sites such as ARM_CAPTURE, re-firing on later calls is
// harmless (the capture is idempotent).
namespace VehCommon {
namespace {

std::mutex g_mutex;
std::unordered_map<void*, Int3Site> g_sites;

void InstrumentThunk(void* address, DobbyRegisterContext* ctx) {
    Int3Site site{};
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_sites.find(address);
        if (it == g_sites.end()) return;
        site = it->second;
    }
    if (!site.onHit) return;
    OSTPlatform::Trap::Context trapCtx(static_cast<void*>(ctx));
    site.onHit(trapCtx, site);
}

}  // namespace

void Arm(Int3Site site) {
    if (!site.target) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_sites[static_cast<void*>(site.target)] = site;
    }
    DobbyInstrument(static_cast<void*>(site.target), InstrumentThunk);
}

bool HasSites() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return !g_sites.empty();
}

// No VEH dispatch on macOS — the instrument callback drives onHit directly.
bool OnBreakpoint(OSTPlatform::Trap::Context&) { return false; }
bool OnSingleStep(OSTPlatform::Trap::Context&) { return false; }

void DisarmAll() {
    std::lock_guard<std::mutex> lock(g_mutex);
    // Dobby instruments stay installed; re-firing onHit on a cleared site map
    // just means InstrumentThunk finds no entry and returns.
    g_sites.clear();
}

void RemoveHandler() {}  // no shared VEH handler on macOS

}  // namespace VehCommon
