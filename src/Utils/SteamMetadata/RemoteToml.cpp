#include "RemoteToml.h"
#include "OSTPlatform/include/Http.h"
#include "OSTPlatform/include/Thread.h"
#include "Utils/Config/Config.h"
#include "Utils/Config/RuntimePaths.h"
#include "Utils/Logging/Log.h"
#include "Utils/SteamMetadata/SteamDiagnostics.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

namespace RemoteToml {

namespace {
    constexpr const char* kDefaultTemplates[] = {
        "https://raw.githubusercontent.com/OpenSteam001/steam-monitor/"
        "{channel}/{component}/{sha256}.toml",
        "https://cdn.jsdelivr.net/gh/OpenSteam001/steam-monitor@"
        "{channel}/{component}/{sha256}.toml",
        "https://raw.githubusercontent.com/madoiscool/steam-monitor/"
        "{channel}/{component}/{sha256}.toml",
        "https://cdn.jsdelivr.net/gh/madoiscool/steam-monitor@"
        "{channel}/{component}/{sha256}.toml",
        "https://git.lua.tools/luatools/steam-monitor/raw/branch/"
        "{channel}/{component}/{sha256}.toml",
    };

    struct Source {
        std::string url;
        bool preferred;
    };

    struct PendingFetch {
        std::mutex mutex;
        std::condition_variable ready;
        size_t remaining = 0;
        bool preferredPending = false;
        bool closed = false;
        std::string preferredBody;
        std::string firstFallbackBody;

        void Complete(bool preferred, std::string body)
        {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (closed) return;
                --remaining;
                if (preferred) {
                    preferredPending = false;
                    preferredBody = std::move(body);
                } else if (firstFallbackBody.empty() && !body.empty()) {
                    firstFallbackBody = std::move(body);
                }
            }
            ready.notify_one();
        }
    };

    static bool HasPlaceholder(std::string_view text, std::string_view placeholder)
    {
        return text.find(placeholder) != std::string_view::npos;
    }

    static bool IsValidTemplate(std::string_view urlTemplate)
    {
        return HasPlaceholder(urlTemplate, "{channel}") &&
               HasPlaceholder(urlTemplate, "{component}") &&
               HasPlaceholder(urlTemplate, "{sha256}");
    }

    static void ReplaceAll(std::string& text,
                           std::string_view from,
                           std::string_view to)
    {
        size_t pos = 0;
        while ((pos = text.find(from, pos)) != std::string::npos) {
            text.replace(pos, from.size(), to);
            pos += to.size();
        }
    }

    static std::string ExpandTemplate(std::string urlTemplate,
                                      const Request& request,
                                      std::string_view sha256)
    {
        ReplaceAll(urlTemplate, "{channel}", request.channel);
        ReplaceAll(urlTemplate, "{component}", request.component);
        ReplaceAll(urlTemplate, "{sha256}", sha256);
        return urlTemplate;
    }

    static std::vector<Source> BuildSources(const Request& request,
                                          std::string_view sha256)
    {
        std::vector<Source> sources;
        const auto append = [&](const std::string& urlTemplate, bool preferred) {
            std::string url = ExpandTemplate(urlTemplate, request, sha256);
            if (std::none_of(sources.begin(), sources.end(), [&](const Source& source) {
                    return source.url == url;
                })) {
                sources.push_back({ std::move(url), preferred });
            }
        };
        const std::string remoteUrlTemplate = Config::GetRemoteUrlTemplate();
        if (!remoteUrlTemplate.empty()) {
            if (IsValidTemplate(remoteUrlTemplate)) {
                append(remoteUrlTemplate, true);
            } else {
                LOG_WARN("RemoteToml: remote.url_template must contain "
                         "{channel}, {component}, and {sha256}; ignoring custom source");
            }
        }
        for (const auto* urlTemplate : kDefaultTemplates) append(urlTemplate, false);
        return sources;
    }

    static bool IsMetadataToml(std::string_view body)
    {
        try {
            const auto root = toml::parse(body);
            // Both metadata channels contain named tables. Detailed pattern / IPC
            // validation remains in their respective loaders.
            for (const auto& [key, node] : root) {
                if (const auto* table = node.as_table(); table && !table->empty())
                    return true;
            }
        } catch (const toml::parse_error&) {
        }
        return false;
    }

    static std::string FetchSources(const std::vector<Source>& sources)
    {
        auto pending = std::make_shared<PendingFetch>();
        pending->remaining = sources.size();
        pending->preferredPending = !sources.empty() && sources.front().preferred;
        for (const auto& source : sources) {
            bool started = false;
            try {
                // Own every captured value: Fetch may return while slower HTTP
                // requests finish under their normal transport timeouts.
                started = OSTPlatform::Thread::StartDetached([pending, source] {
                    std::string body;
                    try {
                        auto http = OSTPlatform::Http::Execute(
                            L"GET", source.url.c_str(), nullptr, 0, nullptr);
                        if (http.ok && http.status == 200 && IsMetadataToml(http.body)) {
                            body = std::move(http.body);
                        } else {
                            LOG_WARN("RemoteToml: source failed or invalid TOML: {} (ok={} HTTP={})",
                                     source.url, http.ok, http.status);
                        }
                    } catch (...) {
                        LOG_WARN("RemoteToml: source request failed: {}", source.url);
                    }
                    pending->Complete(source.preferred, std::move(body));
                    return uint32_t{0};
                });
            } catch (...) {
                // Treat thread creation / capture failures like a failed source.
            }
            if (!started) {
                LOG_WARN("RemoteToml: could not start source request: {}", source.url);
                pending->Complete(source.preferred, {});
            }
        }

        std::unique_lock<std::mutex> lock(pending->mutex);
        pending->ready.wait(lock, [&] {
            return !pending->preferredBody.empty() ||
                   (!pending->preferredPending && !pending->firstFallbackBody.empty()) ||
                   pending->remaining == 0;
        });
        pending->closed = true;
        return !pending->preferredBody.empty()
            ? std::move(pending->preferredBody) : std::move(pending->firstFallbackBody);
    }
} // namespace

Result Fetch(const Request& request)
{
    namespace fs = std::filesystem;
    Result out;

    // 1. SHA-256 of the DLL.
    const auto hashStart = std::chrono::steady_clock::now();
    out.sha256 = SteamDiagnostics::Sha256Of(request.dllPath);
    const auto hashMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - hashStart).count();

    if (out.sha256.empty()) {
        LOG_WARN("RemoteToml({}/{}): Sha256OfFile failed for {} ({} ms)",
                 request.channel, request.component, request.dllPath, hashMs);
        return out;
    }
    LOG_INFO("RemoteToml({}/{}): sha256 = {} ({} ms)",
             request.channel, request.component, out.sha256, hashMs);

    // 2. Cache path & dir.
    fs::path cacheDir = RuntimePaths::CacheDirectory();
    if (!cacheDir.empty()) cacheDir /= fs::path(request.channel) / request.component;
    fs::path cachePath = cacheDir.empty()
        ? fs::path{}
        : cacheDir / (out.sha256 + ".toml");
    const std::string cachePathText = cachePath.string();

    std::error_code mkdirEc;
    if (!cacheDir.empty()) fs::create_directories(cacheDir, mkdirEc);
    if (mkdirEc) {
        LOG_WARN("RemoteToml({}/{}): could not create cache dir {} ({})",
                 request.channel, request.component, cacheDir.string(), mkdirEc.message());
    }

    // 3. Race all distinct sources; a configured source keeps priority until
    // it fails / reaches the existing HTTP timeout. Workers never write the cache.
    const auto sources = BuildSources(request, out.sha256);
    LOG_INFO("RemoteToml({}/{}): requesting {} sources concurrently",
             request.channel, request.component, sources.size());
    std::string body = FetchSources(sources);

    // 4. Remote OK → write cache, return body.
    if (!body.empty()) {
        std::ofstream ofs;
        if (!cachePath.empty()) ofs.open(cachePath, std::ios::binary);
        if (ofs) {
            ofs.write(body.data(), static_cast<std::streamsize>(body.size()));
            LOG_INFO("RemoteToml({}/{}): cached to {}",
                     request.channel, request.component, cachePathText);
        } else {
            LOG_WARN("RemoteToml({}/{}): could not open {} for writing",
                     request.channel, request.component, cachePathText);
        }
        out.body = std::move(body);
        out.ok = true;
        return out;
    }

    // 5. Remote failed → fall back to whatever is cached for this exact SHA.
    if (!cachePath.empty() && fs::exists(cachePath)) {
        LOG_WARN("RemoteToml({}/{}): remote sources failed or timed out; "
                 "falling back to local cache {}",
                 request.channel, request.component,
                 cachePathText);

        std::ifstream ifs(cachePath, std::ios::binary);
        if (ifs) {
            std::string buf((std::istreambuf_iterator<char>(ifs)),
                             std::istreambuf_iterator<char>());
            if (IsMetadataToml(buf)) {
                out.body = std::move(buf);
                out.ok = true;
                out.fromCache = true;
                return out;
            }
            LOG_WARN("RemoteToml({}/{}): cache file empty or invalid TOML: {}",
                     request.channel, request.component, cachePathText);
        } else {
            LOG_WARN("RemoteToml({}/{}): could not open cache file: {}",
                     request.channel, request.component, cachePathText);
        }
    }

    // 6. Total failure — caller handles popup / degraded mode.
    LOG_WARN("RemoteToml({}/{}): no source available",
             request.channel, request.component);
    return out;
}

} // namespace RemoteToml
