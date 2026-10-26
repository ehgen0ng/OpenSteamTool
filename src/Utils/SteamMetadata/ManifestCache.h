#pragma once

#include <cstdint>

namespace ManifestCache {

// Best effort: populate Steam's depotcache before its own disk lookup. A miss
// leaves Steam free to continue through the existing request-code/CDN path.
bool EnsureCached(uint32_t depotId, uint64_t manifestGid) noexcept;

}
