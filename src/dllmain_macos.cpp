#include "OSTPlatform/include/Hash.h"
#include "OSTPlatform/include/Thread.h"
#include "Utils/Logging/Log.h"
#include "dllmain.h"
#include "Hook/HookManager.h"
#include "Utils/Config/ConfigFileWatcher.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/Config/LuaFileWatcher.h"
#include "Utils/Config/RuntimePaths.h"
#include "Utils/Config/InjectionEnvironment_macos.h"
#include "Utils/CloudRedirect/CloudRedirectHost.h"
#include "Utils/SteamMetadata/IPCLoader.h"
#include "Utils/SteamMetadata/PatternLoader.h"
#include "Utils/SteamMetadata/SteamDiagnostics.h"

#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <dlfcn.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

std::atomic_bool g_started{false};

enum class SteamImageKind {
    None,
    SteamClient,
    SteamUi,
};

struct SteamImageInfo {
    std::string path;
    const mach_header* base{};
    intptr_t slide{};
    std::string uuid;
    std::string sha256;
};

struct SteamRuntimeContext {
    std::optional<SteamImageInfo> steamClient;
    std::optional<SteamImageInfo> steamUi;
    bool readyReported{};
};

std::mutex g_runtimeMutex;
std::unordered_set<std::string> g_seenImages;
SteamRuntimeContext g_runtime;

std::mutex g_globalInitMutex;
std::condition_variable g_globalInitCv;
bool g_globalInitComplete{};

std::filesystem::path MainExecutablePath() {
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    if (size == 0) return {};

    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return {};
    }

    buffer.resize(std::strlen(buffer.c_str()));

    std::error_code ec;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(buffer, ec);
    return ec ? std::filesystem::path(buffer) : canonical;
}

bool IsSteamMainProcess() {
    return MainExecutablePath().filename() == "steam_osx";
}

std::string ImagePath(const mach_header* header) {
    const uint32_t count = _dyld_image_count();
    for (uint32_t i = 0; i < count; ++i) {
        if (_dyld_get_image_header(i) == header) {
            const char* name = _dyld_get_image_name(i);
            return name ? std::string(name) : std::string{};
        }
    }
    return {};
}

std::string ImageUuid(const mach_header_64* header) {
    if (!header || header->magic != MH_MAGIC_64) return {};

    auto* command = reinterpret_cast<const load_command*>(header + 1);
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        if (command->cmd == LC_UUID) {
            auto* uuid = reinterpret_cast<const uuid_command*>(command);
            char text[37]{};
            std::snprintf(text,
                          sizeof(text),
                          "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                          uuid->uuid[0],
                          uuid->uuid[1],
                          uuid->uuid[2],
                          uuid->uuid[3],
                          uuid->uuid[4],
                          uuid->uuid[5],
                          uuid->uuid[6],
                          uuid->uuid[7],
                          uuid->uuid[8],
                          uuid->uuid[9],
                          uuid->uuid[10],
                          uuid->uuid[11],
                          uuid->uuid[12],
                          uuid->uuid[13],
                          uuid->uuid[14],
                          uuid->uuid[15]);
            return text;
        }
        command = reinterpret_cast<const load_command*>(
            reinterpret_cast<const uint8_t*>(command) + command->cmdsize);
    }
    return {};
}

SteamImageKind SteamImageKindForPath(const std::filesystem::path& path) {
    const std::string name = path.filename().string();
    if (name == "steamclient.dylib") return SteamImageKind::SteamClient;
    if (name == "steamui.dylib") return SteamImageKind::SteamUi;
    return SteamImageKind::None;
}

void LogImageInfo(const char* label, const SteamImageInfo& image) {
    LOG_DEBUG("{} path={} base={} slide=0x{:X} uuid={} sha256={}",
              label,
              image.path,
              static_cast<const void*>(image.base),
              static_cast<uintptr_t>(image.slide),
              image.uuid,
              image.sha256);
}

void StoreRuntimeImage(SteamImageKind kind, const SteamImageInfo& image) {
    std::lock_guard lock(g_runtimeMutex);
    if (kind == SteamImageKind::SteamClient && !g_runtime.steamClient) {
        g_runtime.steamClient = image;
    } else if (kind == SteamImageKind::SteamUi && !g_runtime.steamUi) {
        g_runtime.steamUi = image;
    }
}

void ReportRuntimeReady() {
    std::optional<SteamImageInfo> steamClient;
    std::optional<SteamImageInfo> steamUi;

    {
        std::lock_guard lock(g_runtimeMutex);
        if (g_runtime.readyReported || !g_runtime.steamClient || !g_runtime.steamUi) return;
        g_runtime.readyReported = true;
        steamClient = g_runtime.steamClient;
        steamUi = g_runtime.steamUi;
    }

    LOG_INFO("OpenSteamTool runtime ready");
    LogImageInfo("runtime steamclient", *steamClient);
    LogImageInfo("runtime steamui", *steamUi);
}

// macOS Steam root: ~/Library/Application Support/Steam (same root the
// SteamCredentialStore backend uses for registry.vdf). opensteamtool.toml and
// config/lua live directly under it, mirroring the Windows <steam>\ layout.
std::filesystem::path SteamRootDir() {
    const char* home = std::getenv("HOME");
    if (!home || !home[0]) return {};
    return std::filesystem::path(home) / "Library" / "Application Support" / "Steam";
}

// Best-effort steamui.dylib path, captured when that image loads; used only for
// the SteamDiagnostics version snapshot (cosmetic — popups). Guarded by g_runtimeMutex.
std::string g_steamUiPath;

// Full one-shot initialisation, mirroring the Windows dllmain InitThread order.
// Runs on a worker thread (never the dyld add-image callback) so its file I/O
// never touches the dyld lock. CloudRedirectHost::Initialize is called once
// both hook sets are installed (see the SteamUi branch of ReportImage below),
// mirroring dllmain.cpp's InitThread order (CoreHook → ReportMissingFunctions
// → CloudRedirectHost::Initialize).
std::once_flag g_globalInitOnce;

void RunGlobalInit(const SteamImageInfo& steamClient) {
    // The real client is now loaded. Keep the current image resident but do
    // not force games/helpers to load it (sandbox/architecture may differ).
    Dl_info ownImage{};
    const bool cleanedInjectionEnvironment =
        ::dladdr(&g_started, &ownImage) != 0 &&
        ownImage.dli_fname &&
        MacOSInjection::RemoveOwnLibraryFromEnvironment(ownImage.dli_fname);

    // Mirrors Windows dllmain.cpp's InitThread order: Log::Init (needs only
    // our own module handle, picks the log directory) before Config::Load,
    // Log::InitModules (needs Config for the log dir/level) right after.
    if (RuntimePaths::EnsureDataDirectories()) return;

    const std::filesystem::path steamRoot = SteamRootDir();
    const std::string configPath = RuntimePaths::ConfigFile().string();
    const std::string luaDir = RuntimePaths::LuaDirectory().string();
    const auto migration = RuntimePaths::MigrateLegacyUserFiles(steamRoot);

    Log::Init(OSTPlatform::DynamicLibrary::GetLoaded("libOpenSteamTool.dylib"));
    if (cleanedInjectionEnvironment) {
        LOG_INFO("Removed OpenSteamTool from child-process injection environment");
    } else {
        LOG_WARN("Could not remove OpenSteamTool from child-process injection environment");
    }
    for (const auto& file : migration.files) {
        if (file.outcome == RuntimePaths::TransferOutcome::Conflict ||
            file.outcome == RuntimePaths::TransferOutcome::Failed) {
            LOG_WARN("Migration {}: {} -> {} ({})", RuntimePaths::ToString(file.outcome),
                     file.source.string(), file.destination.string(), file.error.message());
        }
    }
#if defined(OPENSTEAMTOOL_DOBBY_DEBUG)
    LOG_INFO("Dobby debug logging enabled (disable with CMake flag -DDOBBY_DEBUG=OFF)");
#endif
    LOG_INFO("OpenSteamTool macOS init thread started");
    LOG_INFO("Loaded steamclient.dylib from {}", steamClient.path);
    LogImageInfo("steamclient", steamClient);

    Config::Load(configPath);
    Log::InitModules();
    Log::InstallPlatformLogSink();

    std::string steamUiPath;
    {
        std::lock_guard lock(g_runtimeMutex);
        steamUiPath = g_steamUiPath;
    }
    SteamDiagnostics::Initialize(steamClient.path, steamUiPath);

    // IPC method metadata (funcHash, fencepost, argc, ...) is selected from
    // build-time embedded steam-monitor data.
    IPCLoader::Load(steamClient.path);

    std::vector<std::string> watchDirs = Config::GetLuaPaths();
    if (!luaDir.empty()) watchDirs.push_back(luaDir);

    for (const auto& dir : watchDirs) LuaConfig::ParseDirectory(dir);

    // Retain the OST_LUA_FILE test hook: an explicit override file is parsed too,
    // which lets a smoke test drive LuaConfig without a full Steam config/lua dir.
    if (const char* luaFile = std::getenv("OST_LUA_FILE"); luaFile && luaFile[0]) {
        LuaConfig::ParseFile(luaFile);
    }
    LOG_INFO("Config+Lua loaded: {} depot(s), {} watch dir(s)",
             LuaConfig::GetAllDepotIds().size(), watchDirs.size());

    LuaFileWatcher::Start(watchDirs);
    if (!configPath.empty()) ConfigFileWatcher::Start(configPath, luaDir);

}

void CompleteGlobalInit() {
    {
        std::lock_guard lock(g_globalInitMutex);
        g_globalInitComplete = true;
    }
    g_globalInitCv.notify_all();
}

void WaitForGlobalInit() {
    std::unique_lock lock(g_globalInitMutex);
    g_globalInitCv.wait(lock, [] { return g_globalInitComplete; });
}

void ReportImage(const mach_header* header, intptr_t slide) {
    const std::string pathText = ImagePath(header);
    if (pathText.empty()) return;

    const std::filesystem::path path(pathText);
    const SteamImageKind kind = SteamImageKindForPath(path);
    if (kind == SteamImageKind::None) return;

    {
        std::lock_guard lock(g_runtimeMutex);
        if (!g_seenImages.insert(pathText).second) return;
    }

    const SteamImageInfo image{
        pathText,
        header,
        slide,
        ImageUuid(reinterpret_cast<const mach_header_64*>(header)),
        OSTPlatform::Hash::Sha256OfFile(path),
    };

    StoreRuntimeImage(kind, image);

    // All real work runs on a worker thread, never inline here: on macOS our
    // constructor runs before steamclient.dylib, so this callback typically fires
    // from dyld's add-image path (under the dyld lock). Config/file I/O and the
    // Dobby detours must not run under that lock.
    // Each hook module resolves its targets against its own image — SteamClient
    // hooks use client_hModule (INSTALL_HOOK_C), SteamUI hooks use ui_hModule
    // (INSTALL_HOOK_U) — so the two paths are independent and order-agnostic.
    if (kind == SteamImageKind::SteamClient) {
        client_hModule = const_cast<mach_header*>(header);
        OSTPlatform::Thread::StartDetached([image]() -> uint32_t {
            std::call_once(g_globalInitOnce, [&] { RunGlobalInit(image); });
            PatternLoader::Load(client_hModule, image.path, "steamclient");
            SteamClient::CoreHook();
            LOG_INFO("SteamClient hooks installed (CoreHook)");
            CompleteGlobalInit();
            ReportRuntimeReady();
            return 0;
        });
    } else if (kind == SteamImageKind::SteamUi) {
        ui_hModule = const_cast<mach_header*>(header);
        {
            std::lock_guard lock(g_runtimeMutex);
            g_steamUiPath = image.path;
        }
        OSTPlatform::Thread::StartDetached([image]() -> uint32_t {
            WaitForGlobalInit();
            LOG_INFO("Loaded steamui.dylib from {}", image.path);
            LogImageInfo("steamui", image);
            PatternLoader::Load(ui_hModule, image.path, "steamui");
            SteamUI::CoreHook();
            PatternLoader::ReportMissingFunctions();
            LOG_INFO("SteamUI hooks installed (CoreHook)");

            // Optional Steam Cloud save/achievement redirection (CloudRedirect).
            // No-op unless [cloud].enabled is set and cloud_redirect.dylib loads.
            CloudRedirectHost::Initialize(SteamRootDir().string().c_str());

            ReportRuntimeReady();
            return 0;
        });
    }
}

void OnImageAdded(const mach_header* header, intptr_t slide) {
    ReportImage(header, slide);
}

uint32_t InitThread() {
    const uint32_t count = _dyld_image_count();
    for (uint32_t i = 0; i < count; ++i) {
        ReportImage(_dyld_get_image_header(i), _dyld_get_image_vmaddr_slide(i));
    }

    _dyld_register_func_for_add_image(OnImageAdded);
    return 0;
}

} // namespace

__attribute__((constructor))
static void OpenSteamToolMacOSMain() {
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) {
        return;
    }

    if (!IsSteamMainProcess()) {
        return;
    }

    OSTPlatform::Thread::StartDetached(InitThread);
}
