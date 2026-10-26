#include "ManifestCache.h"

#include "OSTPlatform/include/DynamicLibrary.h"
#include "OSTPlatform/include/Http.h"
#include "Utils/Logging/Log.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace ManifestCache {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
constexpr uint32_t kMaxBodyBytes = 64u * 1024u * 1024u;
constexpr auto kMissTtl = std::chrono::minutes(10);
constexpr std::string_view kArchive = "https://manifest.luastools.xyz";

std::mutex g_mutex;
std::unordered_set<std::string> g_inFlight;
std::unordered_map<std::string, Clock::time_point> g_missing;
std::atomic<uint64_t> g_tempSequence{0};

struct FetchGuard {
    std::string key;
    bool acquired = false;

    explicit FetchGuard(std::string value) : key(std::move(value)) {
        std::lock_guard lock(g_mutex);
        const auto now = Clock::now();
        std::erase_if(g_missing, [now](const auto& entry) {
            return now - entry.second >= kMissTtl;
        });
        if (!g_missing.contains(key)) acquired = g_inFlight.insert(key).second;
    }
    ~FetchGuard() {
        if (acquired) {
            std::lock_guard lock(g_mutex);
            g_inFlight.erase(key);
        }
    }
};

fs::path DepotCacheDirectory() {
#if defined(__APPLE__)
    // The executable is inside Steam.AppBundle, but depotcache is in the data
    // root, alongside steamapps (the same location used by wuhu and Steam).
    const char* home = std::getenv("HOME");
    if (!home || !home[0]) return {};
    return fs::path(home) / "Library" / "Application Support" / "Steam" / "depotcache";
#else
    const auto exe = OSTPlatform::DynamicLibrary::GetMainExecutablePath();
    return exe.empty() ? fs::path{} : exe.parent_path() / "depotcache";
#endif
}

uint32_t ReadU32(std::string_view bytes, size_t offset) {
    const auto* p = reinterpret_cast<const unsigned char*>(bytes.data() + offset);
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// Validate the length-delimited protobuf envelope, including all three sections
// and the final marker. Steam still validates the protobuf, GID and signature.
// Format: SteamRE/SteamKit, SteamKit2/Types/DepotManifest.cs.
bool LooksLikeManifest(std::string_view bytes) {
    if (bytes.size() < 28 || bytes.size() > kMaxBodyBytes ||
        ReadU32(bytes, 0) != 0x71F617D0u) return false;

    unsigned sections = 0;
    size_t offset = 0;
    while (bytes.size() - offset >= 4) {
        const uint32_t magic = ReadU32(bytes, offset);
        offset += 4;
        if (magic == 0x32C415ABu) return sections == 7 && offset == bytes.size();

        unsigned bit = 0;
        switch (magic) {
        case 0x71F617D0u: bit = 1; break;
        case 0x1F4812BEu: bit = 2; break;
        case 0x1B81B817u: bit = 4; break;
        default: return false;
        }
        if ((sections & bit) || bytes.size() - offset < 4) return false;
        const uint32_t length = ReadU32(bytes, offset);
        offset += 4;
        if (length > bytes.size() - offset) return false;
        if (bit == 2 && length == 0) return false;
        sections |= bit;
        offset += length;
    }
    return false;
}

bool IsCached(const fs::path& path) {
    std::error_code ec;
    // Leave existing Steam caches (including legacy formats) to Steam itself.
    return fs::is_regular_file(path, ec) && !ec;
}

bool WriteAtomic(const fs::path& destination, std::string_view body) {
    std::error_code ec;
    fs::create_directories(destination.parent_path(), ec);
    if (ec) return false;

    fs::path temporary = destination;
    temporary += std::format(".{}.{}.tmp", Clock::now().time_since_epoch().count(),
                             g_tempSequence.fetch_add(1));
    struct Cleanup {
        fs::path path;
        ~Cleanup() { std::error_code ignored; fs::remove(path, ignored); }
    } cleanup{temporary};

    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(body.data(), static_cast<std::streamsize>(body.size()));
    output.close();
    if (!output) return false;

    // A concurrent Steam download may already have filled this cache entry.
    if (IsCached(destination)) return true;
    fs::rename(temporary, destination, ec);
    return !ec || IsCached(destination);
}

bool EnsureCachedAt(const fs::path& directory, uint32_t depotId, uint64_t manifestGid) {
    if (directory.empty() || !depotId || !manifestGid || manifestGid == UINT64_MAX)
        return false;

    const std::string key = std::format("{}_{}", depotId, manifestGid);
    const fs::path destination = directory / (key + ".manifest");
    if (IsCached(destination)) return true;

    FetchGuard guard(key);
    if (!guard.acquired) return IsCached(destination);
    if (IsCached(destination)) return true;

    const std::string url = std::format("{}/m/{}/{}", kArchive, depotId, manifestGid);
    // This runs before Steam's disk check. Keep network phases short; a slow
    // archive must fall back rather than hold up Steam's download indefinitely.
    const auto response = OSTPlatform::Http::Execute(
        L"GET", url.c_str(), nullptr, 0, nullptr,
        1000, 2000, 2000, 5000, kMaxBodyBytes);

    if (!response.ok || response.status != 200) {
        if (response.ok && response.status == 404) {
            std::lock_guard lock(g_mutex);
            // Bound memory even during a large library refresh.
            if (g_missing.size() >= 1024) g_missing.clear();
            g_missing[key] = Clock::now();
        }
        LOG_MANIFEST_DEBUG("ManifestCache: fallback depot={} gid={} status={}",
                           depotId, manifestGid, response.status);
        return false;
    }
    if (!LooksLikeManifest(response.body)) {
        LOG_MANIFEST_WARN("ManifestCache: invalid archive body depot={} gid={} bytes={}",
                          depotId, manifestGid, response.body.size());
        return false;
    }
    if (!WriteAtomic(destination, response.body)) {
        LOG_MANIFEST_WARN("ManifestCache: cannot write {}", destination.string());
        return false;
    }

    LOG_MANIFEST_INFO("ManifestCache: cached depot={} gid={} bytes={}",
                      depotId, manifestGid, response.body.size());
    return true;
}

} // namespace

bool EnsureCached(uint32_t depotId, uint64_t manifestGid) noexcept {
    try {
        return EnsureCachedAt(DepotCacheDirectory(), depotId, manifestGid);
    } catch (...) {
        // File/network/allocation failures must never escape into Steam.
        return false;
    }
}

} // namespace ManifestCache
