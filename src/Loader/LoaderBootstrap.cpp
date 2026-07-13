#include "LoaderBootstrap.h"

#include "../Utils/Config/RuntimePaths.h"

#include <windows.h>

#include <cwchar>
#include <filesystem>
#include <string>

namespace LoaderBootstrap {
namespace {

std::filesystem::path MainExecutablePath() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return {};
    buffer.resize(length);
    return std::filesystem::path(buffer);
}

void ReportMigrationIssue(const RuntimePaths::TransferResult& result) {
    if (result.outcome != RuntimePaths::TransferOutcome::Conflict &&
        result.outcome != RuntimePaths::TransferOutcome::Failed) {
        return;
    }

    std::wstring message = L"OpenSteamTool: could not migrate ";
    message += result.source.wstring();
    message += L" to ";
    message += result.destination.wstring();
    message += result.outcome == RuntimePaths::TransferOutcome::Conflict
        ? L" because the destination differs; both files were preserved.\n"
        : L"; the source file was preserved.\n";
    OutputDebugStringW(message.c_str());
}

} // namespace

bool LoadOpenSteamToolForSteamProcess() {
    const std::filesystem::path executable = MainExecutablePath();
    if (executable.empty()) return true;
    if (_wcsicmp(executable.filename().c_str(), L"steam.exe") != 0) return true;

    const std::filesystem::path legacyLibrary =
        executable.parent_path() / L"OpenSteamTool.dll";
    const std::filesystem::path binaryDirectory = RuntimePaths::BinaryDirectory();
    const std::filesystem::path library = binaryDirectory.empty()
        ? std::filesystem::path{}
        : binaryDirectory / L"OpenSteamTool.dll";

    const auto resolution = RuntimePaths::MigrateAndSelectFile(legacyLibrary, library);
    ReportMigrationIssue(resolution.transfer);
    return !resolution.path.empty() &&
           LoadLibraryW(resolution.path.c_str()) != nullptr;
}

} // namespace LoaderBootstrap
