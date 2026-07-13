#pragma once

namespace LoaderBootstrap {

    // Loads OpenSteamTool only for steam.exe. A legacy copy next to steam.exe
    // is migrated to the per-user data directory before it is loaded.
    bool LoadOpenSteamToolForSteamProcess();

} // namespace LoaderBootstrap
