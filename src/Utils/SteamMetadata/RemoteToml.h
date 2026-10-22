#pragma once
#include <string>

namespace RemoteToml {

    struct Request {
        std::string channel;    // "pattern" or "ipc"
        std::string component;  // "steamclient" or "steamui"
        std::string dllPath;
    };

    struct Result {
        bool        ok        = false;
        bool        fromCache = false;
        std::string body;
        std::string sha256;
        bool        fromEmbedded = false;
    };

    // Exact-SHA embedded metadata first, then concurrent remote sources
    // (custom URL preferred), then the exact SHA cache.
    Result Fetch(const Request& request);

} // namespace RemoteToml
