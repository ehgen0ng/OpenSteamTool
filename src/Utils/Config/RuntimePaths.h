#pragma once

#include <filesystem>
#include <system_error>
#include <vector>

namespace RuntimePaths {

    enum class TransferOutcome {
        SourceMissing,
        AlreadyCurrent,
        Migrated,
        RemovedDuplicate,
        Conflict,
        Failed,
    };

    struct TransferResult {
        std::filesystem::path source;
        std::filesystem::path destination;
        TransferOutcome outcome = TransferOutcome::SourceMissing;
        std::error_code error;
    };

    struct MigrationReport {
        std::vector<TransferResult> files;
    };

    struct FileResolution {
        std::filesystem::path path;
        TransferResult transfer;
    };

    // Per-user OpenSteamTool data root.
    //   macOS:  ~/Library/Application Support/OpenSteamTool
    //   Windows: %LOCALAPPDATA%\OpenSteamTool
    // OST_DATA_DIR overrides the platform default for the whole process.
    const std::filesystem::path& DataRoot();

    std::filesystem::path ConfigFile();
    std::filesystem::path LogDirectory();
    std::filesystem::path LuaDirectory();
    std::filesystem::path CacheDirectory();
    std::filesystem::path BinaryDirectory();
    std::filesystem::path PluginDirectory();

    // Creates the platform layout. The config file itself is not generated.
    // Returns an empty error_code on success.
    std::error_code EnsureDataDirectories();

    const char* ToString(TransferOutcome outcome);

    // Moves one file without risking source data: copy to a destination-side
    // temporary file, compare every byte, publish atomically, then remove the
    // source. Existing identical destinations only cause source cleanup;
    // conflicting destinations are never overwritten.
    TransferResult MoveFileVerified(const std::filesystem::path& source,
                                    const std::filesystem::path& destination);

    // Migrates when possible, then selects the new file or the preserved
    // legacy file as a fallback.
    FileResolution MigrateAndSelectFile(const std::filesystem::path& source,
                                        const std::filesystem::path& destination);

    // Migrates legacy Steam-root config, the Lua file tree, logs, Windows
    // metadata cache and the default CloudRedirect plugin. Only known
    // OpenSteamTool paths are touched. Empty legacy directories are removed;
    // unrelated files and Steam parents remain.
    MigrationReport MigrateLegacyUserFiles(const std::filesystem::path& steamRoot);

} // namespace RuntimePaths
