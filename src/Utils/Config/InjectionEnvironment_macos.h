#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

namespace MacOSInjection {

// Remove only this image, not Steam's loader/overlay or other injected libraries.
// Call after steamclient loads: clearing this in the constructor would also
// strip injection from the bootstrapper's exec into the actual Steam client.
inline bool RemoveOwnLibraryFromEnvironment(const char* imagePath) {
    const char* raw = std::getenv("DYLD_INSERT_LIBRARIES");
    if (!raw || !imagePath || !*imagePath) return true;

    const std::string original(raw);
    std::error_code ec;
    const auto ownPath = std::filesystem::weakly_canonical(imagePath, ec);
    const bool ownPathValid = !ec;
    std::string filtered;
    bool removed = false;
    bool keptEntry = false;
    for (std::size_t begin = 0;;) {
        const auto end = original.find(':', begin);
        const auto entry = std::string_view(original).substr(
            begin, end == std::string::npos ? end : end - begin);
        bool matches = entry == imagePath;
        if (!matches && !entry.empty() && ownPathValid) {
            const auto candidate = std::filesystem::weakly_canonical(entry, ec);
            matches = !ec && candidate == ownPath;
        }
        if (matches) {
            removed = true;
        } else {
            if (keptEntry) filtered += ':';
            filtered += entry;
            keptEntry = true;
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    if (!removed) return true;
    return filtered.empty()
        ? ::unsetenv("DYLD_INSERT_LIBRARIES") == 0
        : ::setenv("DYLD_INSERT_LIBRARIES", filtered.c_str(), 1) == 0;
}

} // namespace MacOSInjection
