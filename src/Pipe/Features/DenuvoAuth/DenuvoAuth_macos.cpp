#include "Pipe/Features/DenuvoAuth/DenuvoAuth.h"

// macOS excludes Denuvo detection/authorization: the real feature (DenuvoAuth.cpp
// + ProtectionScan.cpp) is PE/Windows-specific (it scans PE sections and the OEP
// signature of .exe/.dll images), and Steam-on-macOS titles don't drive the same
// Denuvo pipe-authorization flow. These stubs supply the two symbols the shared
// Pipe layer odr-uses — PipeManager::OnHandshake calls Apply(), and (once ported)
// Hooks_IPC_ISteamUser calls IsAuthorizedPipe() — so the mac build links. Both are
// inert: no per-handshake Denuvo work, and no pipe is ever the authorization pipe.
namespace PipeManager::DenuvoAuth {

void Apply(const PipeContext&) {}

bool IsAuthorizedPipe(const CPipeClient*) {
    return false;
}

} // namespace PipeManager::DenuvoAuth
