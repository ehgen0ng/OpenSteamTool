#include "include/Detour.h"

#include <dobby.h>

#include <mutex>
#include <unordered_map>

// macOS inline-hook backend, mirroring the Detours-based Windows/Detour.cpp.
//
// Dobby applies each hook immediately and has no transaction concept, so the
// Begin/Commit calls are no-ops. Note Detours suspends all threads inside its
// transaction for atomic patching; Dobby does NOT — so callers must avoid
// hooking a hot function while other threads execute its prologue (patching a
// startup hot path such as CreateInterface crashes; low-frequency targets are
// safe).
namespace OSTPlatform::Detour {
namespace {

std::mutex g_mutex;
// Maps a detour (replacement) function to the ORIGINAL target address. Attach
// rewrites *target into Dobby's trampoline, so Detach can no longer read the
// original address from it — DobbyDestroy needs the original, so we remember it.
std::unordered_map<void*, void*> g_targets;

}  // namespace

bool BeginTransaction() { return true; }
bool CommitTransaction() { return true; }

bool Attach(void** target, void* detour) {
    if (!target || !*target || !detour) return false;

    void* address = *target;
    // On success *target is rewritten to a trampoline that calls the original,
    // matching Detours' DetourAttach semantics. Dobby's replace/origin params
    // are void*/void** (as used in the PoC), so no function-pointer typedef.
    if (DobbyHook(address, detour, target) != 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    g_targets[detour] = address;
    return true;
}

bool Detach(void** target, void* detour) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_targets.find(detour);
    if (it == g_targets.end()) return false;

    void* address = it->second;
    if (DobbyDestroy(address) != 0) return false;

    if (target) *target = address;  // restore the original pointer (Detours parity)
    g_targets.erase(it);
    return true;
}

}  // namespace OSTPlatform::Detour
