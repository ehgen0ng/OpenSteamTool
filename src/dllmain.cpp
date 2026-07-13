#include "dllmain.h"
#include "Hook/HookManager.h"
#include "Pipe/Features/Injection/Injection.h"
#include "Utils/Config/ConfigFileWatcher.h"
#include "Utils/Config/LuaFileWatcher.h"
#include "Utils/Config/RuntimePaths.h"
#include "Utils/CloudRedirect/CloudRedirectHost.h"
#include "Utils/SteamMetadata/IPCLoader.h"
#include "Utils/SteamMetadata/PatternLoader.h"
#include "Utils/SteamMetadata/SteamDiagnostics.h"
#include "OSTPlatform/include/DynamicLibrary.h"
#include "OSTPlatform/include/Thread.h"

#include <windows.h>

// prepare key runtime paths.
bool InitializeSteamComponents()
{
    const std::string steamInstallPath = OSTPlatform::DynamicLibrary::GetCurrentDirectoryPath();
    const std::string luaDir = RuntimePaths::LuaDirectory().string();
    const std::string configPath = RuntimePaths::ConfigFile().string();
    if (steamInstallPath.empty() || luaDir.empty() || configPath.empty()) {
        return false;
    }
    sprintf_s(SteamInstallPath, kRuntimePathCapacity, "%s", steamInstallPath.c_str());
    sprintf_s(SteamclientPath, kRuntimePathCapacity, "%s\\steamclient64.dll",  SteamInstallPath);
    sprintf_s(SteamUIPath,     kRuntimePathCapacity, "%s\\steamui.dll",        SteamInstallPath);
    sprintf_s(DiversionPath,   kRuntimePathCapacity, "%s\\bin\\diversion.dll", SteamInstallPath);
    if (sprintf_s(LuaDir, kRuntimePathCapacity, "%s", luaDir.c_str()) < 0 ||
        sprintf_s(ConfigPath, kRuntimePathCapacity, "%s", configPath.c_str()) < 0) {
        return false;
    }
    
    client_hModule = OSTPlatform::DynamicLibrary::Load(SteamclientPath);
    if (!client_hModule) {
        LOG_ERROR("Load steamclient64.dll failed: {} (err={})",
                  SteamclientPath, OSTPlatform::DynamicLibrary::GetLastErrorCode());
        return false;
    }
    LOG_INFO("Loaded steamclient64.dll from {}", SteamclientPath);
    
    ui_hModule = OSTPlatform::DynamicLibrary::Load(SteamUIPath);
    if(!ui_hModule) {
        LOG_ERROR("Load failed for steamui.dll: err={}", OSTPlatform::DynamicLibrary::GetLastErrorCode());
        return false;
    }
    return true;
}

// All initialisation that touches the filesystem, loads modules, scans
// memory, or installs detours runs here on a worker thread — we MUST NOT do
// any of that from inside DllMain (loader lock).
static uint32_t InitThread(OSTPlatform::DynamicLibrary::ModuleHandle selfModule) {
    if (RuntimePaths::EnsureDataDirectories()) return 1;
    Log::Init(selfModule);
    LOG_INFO("OpenSteamTool init thread started");

    if (!InitializeSteamComponents()) {
        LOG_ERROR("InitializeSteamComponents failed");
        return 1;
    }

    const auto migration = RuntimePaths::MigrateLegacyUserFiles(SteamInstallPath);
    for (const auto& file : migration.files) {
        if (file.outcome == RuntimePaths::TransferOutcome::Conflict ||
            file.outcome == RuntimePaths::TransferOutcome::Failed) {
            LOG_WARN("Migration {}: {} -> {} ({})", RuntimePaths::ToString(file.outcome),
                     file.source.string(), file.destination.string(), file.error.message());
        }
    }

    Config::Load(ConfigPath);
    Log::InitModules();
    Log::InstallPlatformLogSink();
    PipeManager::Injection::MigrateConfiguredLibraries();
    SteamDiagnostics::Initialize(SteamclientPath, SteamUIPath);

    // Load pattern files for steamclient64.dll and steamui.dll.
    // Each call computes the SHA-256 of the DLL on disk, checks the local
    // cache, and downloads from GitHub if needed.  Both calls are synchronous
    // but run on this worker thread, never under the loader lock.
    PatternLoader::Load(ui_hModule, SteamUIPath, "steamui");
    PatternLoader::Load(client_hModule, SteamclientPath, "steamclient");

    // IPC method metadata (funcHash, fencepost, argc, ...)
    IPCLoader::Load(SteamclientPath);

    std::vector<std::string> watchDirs = Config::GetLuaPaths();
    watchDirs.push_back(std::string(LuaDir));
    for (const auto& dir : watchDirs)
        LuaConfig::ParseDirectory(dir);

    LuaFileWatcher::Start(watchDirs);
    ConfigFileWatcher::Start(ConfigPath, LuaDir);

    SteamUI::CoreHook();
    SteamClient::CoreHook();

    // Surface any functions that FindPattern() could not locate.
    PatternLoader::ReportMissingFunctions();

    // Optional Steam Cloud save redirection (CloudRedirect). No-op unless
    // [cloud].enabled is set and cloud_redirect.dll is present.
    CloudRedirectHost::Initialize(SteamInstallPath);

    LOG_INFO("OpenSteamTool init complete");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, PVOID pvReserved)
{
    if (dwReason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        // Hand off all real work to a worker thread to avoid running file I/O,
        // module loading and detour transactions under the loader lock.
        OSTPlatform::Thread::StartDetached([module = reinterpret_cast<OSTPlatform::DynamicLibrary::ModuleHandle>(hModule)] {
            return InitThread(module);
        });
    }
    else if (dwReason == DLL_PROCESS_DETACH)
    {
        ConfigFileWatcher::Stop();
        LuaFileWatcher::Stop();
        SteamUI::CoreUnhook();
        SteamClient::CoreUnhook();
        CloudRedirectHost::Shutdown();
    }

    return TRUE;
}
