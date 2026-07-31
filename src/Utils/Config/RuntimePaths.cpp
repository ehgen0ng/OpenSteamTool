#include "RuntimePaths.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace RuntimePaths {
namespace {

std::filesystem::path ResolveDataRoot() {
#if defined(_WIN32)
    if (const wchar_t* overrideDir = _wgetenv(L"OST_DATA_DIR");
        overrideDir && overrideDir[0]) {
        return std::filesystem::path(overrideDir);
    }

    if (const wchar_t* localAppData = _wgetenv(L"LOCALAPPDATA");
        localAppData && localAppData[0]) {
        return std::filesystem::path(localAppData) / L"OpenSteamTool";
    }
#elif defined(__APPLE__)
    if (const char* overrideDir = std::getenv("OST_DATA_DIR");
        overrideDir && overrideDir[0]) {
        return std::filesystem::path(overrideDir);
    }

    if (const char* home = std::getenv("HOME"); home && home[0]) {
        return std::filesystem::path(home) /
               "Library" / "Application Support" / "OpenSteamTool";
    }
#endif
    return {};
}

bool FilesEqual(const std::filesystem::path& lhs,
                const std::filesystem::path& rhs,
                std::error_code& ec) {
    ec.clear();
    const auto lhsSize = std::filesystem::file_size(lhs, ec);
    if (ec) return false;
    const auto rhsSize = std::filesystem::file_size(rhs, ec);
    if (ec || lhsSize != rhsSize) return false;

    std::ifstream lhsFile(lhs, std::ios::binary);
    std::ifstream rhsFile(rhs, std::ios::binary);
    if (!lhsFile || !rhsFile) {
        ec = std::make_error_code(std::errc::io_error);
        return false;
    }

    std::array<char, 64 * 1024> lhsBuffer{};
    std::array<char, 64 * 1024> rhsBuffer{};
    while (lhsFile && rhsFile) {
        lhsFile.read(lhsBuffer.data(), static_cast<std::streamsize>(lhsBuffer.size()));
        rhsFile.read(rhsBuffer.data(), static_cast<std::streamsize>(rhsBuffer.size()));
        const auto lhsRead = lhsFile.gcount();
        const auto rhsRead = rhsFile.gcount();
        if (lhsRead != rhsRead ||
            !std::equal(lhsBuffer.begin(), lhsBuffer.begin() + lhsRead,
                        rhsBuffer.begin())) {
            return false;
        }
    }
    return lhsFile.eof() && rhsFile.eof();
}

std::filesystem::path TemporaryPathFor(const std::filesystem::path& destination) {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto thread = std::hash<std::thread::id>{}(std::this_thread::get_id());
    std::filesystem::path temporary = destination;
    temporary += ".migrating." + std::to_string(nonce) + "." + std::to_string(thread);
    return temporary;
}

void AddIfRelevant(MigrationReport& report, TransferResult result) {
    if (result.outcome != TransferOutcome::SourceMissing &&
        result.outcome != TransferOutcome::AlreadyCurrent) {
        report.files.push_back(std::move(result));
    }
}

void MigrateFlatFiles(const std::filesystem::path& sourceDirectory,
                      const std::filesystem::path& destinationDirectory,
                      std::string_view extension,
                      MigrationReport& report) {
    std::error_code ec;
    if (std::filesystem::is_symlink(sourceDirectory, ec) || ec) return;
    if (!std::filesystem::is_directory(sourceDirectory, ec) || ec) return;

    std::filesystem::directory_iterator it(sourceDirectory, ec);
    const std::filesystem::directory_iterator end;
    while (!ec && it != end) {
        const auto& entry = *it;
        std::error_code entryEc;
        const bool isSymlink = entry.is_symlink(entryEc);
        if (!entryEc && !isSymlink && entry.is_regular_file(entryEc) && !entryEc &&
            entry.path().extension() == extension) {
            AddIfRelevant(report, MoveFileVerified(
                entry.path(), destinationDirectory / entry.path().filename()));
        }
        it.increment(ec);
    }

    if (ec) {
        report.files.push_back({sourceDirectory, destinationDirectory,
                                TransferOutcome::Failed, ec});
    }
}

void MigrateTree(const std::filesystem::path& sourceDirectory,
                 const std::filesystem::path& destinationDirectory,
                 std::string_view extension,
                 MigrationReport& report) {
    std::error_code ec;
    if (std::filesystem::is_symlink(sourceDirectory, ec) || ec) return;
    if (!std::filesystem::is_directory(sourceDirectory, ec) || ec) return;

    std::filesystem::recursive_directory_iterator it(sourceDirectory, ec);
    const std::filesystem::recursive_directory_iterator end;
    while (!ec && it != end) {
        const auto& entry = *it;
        std::error_code entryEc;
        const bool isSymlink = entry.is_symlink(entryEc);
        if (!entryEc && !isSymlink && entry.is_regular_file(entryEc) && !entryEc &&
            (extension.empty() || entry.path().extension() == extension)) {
            const auto relative = std::filesystem::relative(entry.path(), sourceDirectory, entryEc);
            if (!entryEc) {
                AddIfRelevant(report, MoveFileVerified(
                    entry.path(), destinationDirectory / relative));
            } else {
                report.files.push_back({entry.path(), destinationDirectory,
                                        TransferOutcome::Failed, entryEc});
            }
        }
        it.increment(ec);
    }

    if (ec) {
        report.files.push_back({sourceDirectory, destinationDirectory,
                                TransferOutcome::Failed, ec});
    }
}

void RemoveEmptyTree(const std::filesystem::path& root) {
    std::error_code ec;
    if (std::filesystem::is_symlink(root, ec) || ec) return;
    if (!std::filesystem::is_directory(root, ec) || ec) return;

    std::vector<std::filesystem::path> directories;
    std::filesystem::recursive_directory_iterator it(root, ec);
    const std::filesystem::recursive_directory_iterator end;
    while (!ec && it != end) {
        std::error_code entryEc;
        const bool isSymlink = it->is_symlink(entryEc);
        if (!entryEc && !isSymlink && it->is_directory(entryEc) && !entryEc) {
            directories.push_back(it->path());
        }
        it.increment(ec);
    }

    std::sort(directories.begin(), directories.end(),
              [](const auto& lhs, const auto& rhs) {
                  return lhs.native().size() > rhs.native().size();
              });
    for (const auto& directory : directories) {
        std::filesystem::remove(directory, ec);
        ec.clear();
    }
    std::filesystem::remove(root, ec);
}

} // namespace

const std::filesystem::path& DataRoot() {
    static const std::filesystem::path root = ResolveDataRoot();
    return root;
}

std::filesystem::path ConfigFile() {
    if (DataRoot().empty()) return {};
    return DataRoot() / "opensteamtool.toml";
}

std::filesystem::path LogDirectory() {
    if (DataRoot().empty()) return {};
    return DataRoot() / "logs";
}

std::filesystem::path LuaDirectory() {
    if (DataRoot().empty()) return {};
    return DataRoot() / "lua";
}

std::filesystem::path CacheDirectory() {
    if (DataRoot().empty()) return {};
    return DataRoot() / "cache";
}

std::filesystem::path BinaryDirectory() {
    if (DataRoot().empty()) return {};
    return DataRoot() / "bin";
}

std::filesystem::path PluginDirectory() {
    if (DataRoot().empty()) return {};
    return DataRoot() / "plugins";
}

std::error_code EnsureDataDirectories() {
    if (DataRoot().empty()) {
        return std::make_error_code(std::errc::no_such_file_or_directory);
    }

    std::error_code ec;
    const std::array commonDirectories{
        DataRoot(),
        LogDirectory(),
        LuaDirectory(),
        PluginDirectory(),
    };
    for (const auto& directory : commonDirectories) {
        std::filesystem::create_directories(directory, ec);
        if (ec) return ec;
    }

#if defined(_WIN32)
    const std::array windowsDirectories{
        CacheDirectory(),
        BinaryDirectory(),
    };
    for (const auto& directory : windowsDirectories) {
        std::filesystem::create_directories(directory, ec);
        if (ec) return ec;
    }
#endif
    return {};
}

const char* ToString(TransferOutcome outcome) {
    switch (outcome) {
    case TransferOutcome::SourceMissing:    return "source-missing";
    case TransferOutcome::AlreadyCurrent:   return "already-current";
    case TransferOutcome::Migrated:         return "migrated";
    case TransferOutcome::RemovedDuplicate: return "removed-duplicate";
    case TransferOutcome::Conflict:         return "conflict";
    case TransferOutcome::Failed:           return "failed";
    }
    return "unknown";
}

TransferResult MoveFileVerified(const std::filesystem::path& source,
                                const std::filesystem::path& destination) {
    TransferResult result{
        source, destination, TransferOutcome::SourceMissing, {}};
    std::error_code ec;

    if (source.empty() || destination.empty()) {
        result.outcome = TransferOutcome::Failed;
        result.error = std::make_error_code(std::errc::invalid_argument);
        return result;
    }
    if (source.lexically_normal() == destination.lexically_normal()) {
        result.outcome = TransferOutcome::AlreadyCurrent;
        return result;
    }

    if (std::filesystem::is_symlink(source, ec)) {
        result.outcome = TransferOutcome::Failed;
        result.error = std::make_error_code(std::errc::operation_not_supported);
        return result;
    }
    if (ec) {
        result.outcome = TransferOutcome::Failed;
        result.error = ec;
        return result;
    }

    if (!std::filesystem::exists(source, ec)) {
        if (ec) {
            result.outcome = TransferOutcome::Failed;
            result.error = ec;
        }
        return result;
    }
    if (!std::filesystem::is_regular_file(source, ec) || ec) {
        result.outcome = TransferOutcome::Failed;
        result.error = ec ? ec : std::make_error_code(std::errc::invalid_argument);
        return result;
    }

    if (std::filesystem::exists(destination, ec)) {
        if (ec) {
            result.outcome = TransferOutcome::Failed;
            result.error = ec;
            return result;
        }

        const bool equal = FilesEqual(source, destination, ec);
        if (ec) {
            result.outcome = TransferOutcome::Failed;
            result.error = ec;
            return result;
        }
        if (!equal) {
            result.outcome = TransferOutcome::Conflict;
            return result;
        }

        std::filesystem::remove(source, ec);
        result.outcome = ec ? TransferOutcome::Failed : TransferOutcome::RemovedDuplicate;
        result.error = ec;
        return result;
    }
    if (ec) {
        result.outcome = TransferOutcome::Failed;
        result.error = ec;
        return result;
    }

    std::filesystem::create_directories(destination.parent_path(), ec);
    if (ec) {
        result.outcome = TransferOutcome::Failed;
        result.error = ec;
        return result;
    }

    const std::filesystem::path temporary = TemporaryPathFor(destination);
    std::filesystem::copy_file(source, temporary,
                               std::filesystem::copy_options::none, ec);
    if (ec) {
        std::error_code removeEc;
        std::filesystem::remove(temporary, removeEc);
        result.outcome = TransferOutcome::Failed;
        result.error = ec;
        return result;
    }

    if (!FilesEqual(source, temporary, ec) || ec) {
        std::error_code removeEc;
        std::filesystem::remove(temporary, removeEc);
        result.outcome = TransferOutcome::Failed;
        result.error = ec ? ec : std::make_error_code(std::errc::io_error);
        return result;
    }

    bool removedDuplicate = false;
    std::filesystem::rename(temporary, destination, ec);
    if (ec) {
        std::error_code destinationEc;
        const bool destinationExists = std::filesystem::exists(destination, destinationEc);
        const bool destinationEqual = destinationExists && !destinationEc &&
                                      FilesEqual(source, destination, destinationEc) &&
                                      !destinationEc;
        std::error_code removeEc;
        std::filesystem::remove(temporary, removeEc);
        if (!destinationEqual) {
            result.outcome = destinationExists ? TransferOutcome::Conflict
                                                : TransferOutcome::Failed;
            result.error = destinationEc ? destinationEc : ec;
            return result;
        }
        removedDuplicate = true;
    }

    std::filesystem::remove(source, ec);
    result.outcome = ec ? TransferOutcome::Failed
                        : (removedDuplicate ? TransferOutcome::RemovedDuplicate
                                            : TransferOutcome::Migrated);
    result.error = ec;
    return result;
}

FileResolution MigrateAndSelectFile(const std::filesystem::path& source,
                                    const std::filesystem::path& destination) {
    FileResolution resolution;
    resolution.transfer = MoveFileVerified(source, destination);

    std::error_code ec;
    if (!destination.empty() &&
        std::filesystem::is_regular_file(destination, ec) && !ec) {
        resolution.path = destination;
        return resolution;
    }
    ec.clear();
    if (!source.empty() && std::filesystem::is_regular_file(source, ec) && !ec) {
        resolution.path = source;
        return resolution;
    }
    resolution.path = destination;
    return resolution;
}

MigrationReport MigrateLegacyUserFiles(const std::filesystem::path& steamRoot) {
    MigrationReport report;
    if (steamRoot.empty() || DataRoot().empty()) return report;

    AddIfRelevant(report, MoveFileVerified(
        steamRoot / "opensteamtool.toml", ConfigFile()));

    const std::filesystem::path legacyLua = steamRoot / "config" / "lua";
    MigrateTree(legacyLua, LuaDirectory(), {}, report);

    const std::filesystem::path legacyData = steamRoot / "opensteamtool";
    MigrateFlatFiles(legacyData, LogDirectory() / "legacy", ".log", report);
    MigrateTree(legacyData / "pattern", CacheDirectory() / "pattern", ".toml", report);
    MigrateTree(legacyData / "ipc", CacheDirectory() / "ipc", ".toml", report);

#if defined(_WIN32)
    AddIfRelevant(report, MoveFileVerified(
        steamRoot / L"cloud_redirect.dll",
        PluginDirectory() / L"cloud_redirect.dll"));
#endif

    RemoveEmptyTree(legacyLua);
    RemoveEmptyTree(legacyData);
    return report;
}

} // namespace RuntimePaths
