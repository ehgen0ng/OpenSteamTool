#include "include/DynamicLibrary.h"

#include "include/Log.h"

#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>

#include <cerrno>
#include <climits>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

namespace OSTPlatform::DynamicLibrary {
namespace {

uint32_t g_lastError = 0;

std::filesystem::path CanonicalPath(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
    return ec ? path : canonical;
}

std::filesystem::path ImagePath(uint32_t index) {
    const char* name = _dyld_get_image_name(index);
    return name ? CanonicalPath(name) : std::filesystem::path{};
}

const mach_header* FindImageByPath(const std::filesystem::path& path) {
    const auto target = CanonicalPath(path);
    const uint32_t count = _dyld_image_count();
    for (uint32_t i = 0; i < count; ++i) {
        if (ImagePath(i) == target) {
            return _dyld_get_image_header(i);
        }
    }
    return nullptr;
}

const mach_header* FindImageByName(std::string_view moduleName) {
    const uint32_t count = _dyld_image_count();
    for (uint32_t i = 0; i < count; ++i) {
        const auto path = ImagePath(i);
        if (path.filename().string() == moduleName) {
            return _dyld_get_image_header(i);
        }
    }
    return nullptr;
}

std::filesystem::path FindPathByHeader(const mach_header* header) {
    if (!header) return {};
    const uint32_t count = _dyld_image_count();
    for (uint32_t i = 0; i < count; ++i) {
        if (_dyld_get_image_header(i) == header) {
            return ImagePath(i);
        }
    }
    return {};
}

} // namespace

ModuleHandle Load(const std::filesystem::path& path) {
    const auto canonical = CanonicalPath(path);
    void* handle = dlopen(canonical.c_str(), RTLD_NOW);
    if (!handle) {
        g_lastError = static_cast<uint32_t>(errno);
        OSTP_LOG_WARN("dlopen('{}') failed: {}", canonical.string(), dlerror() ? dlerror() : "unknown");
        return nullptr;
    }

    if (const mach_header* header = FindImageByPath(canonical)) {
        return const_cast<mach_header*>(header);
    }
    return handle;
}

ModuleHandle GetLoaded(std::string_view moduleName) {
    return const_cast<mach_header*>(FindImageByName(moduleName));
}

void* GetSymbol(ModuleHandle module, const char* symbolName) {
    if (!symbolName) return nullptr;

    void* handle = RTLD_DEFAULT;
    if (module) {
        if (auto path = FindPathByHeader(static_cast<const mach_header*>(module)); !path.empty()) {
            if (void* noLoad = dlopen(path.c_str(), RTLD_NOW | RTLD_NOLOAD)) {
                handle = noLoad;
            }
        } else {
            handle = module;
        }
    }

    dlerror();
    void* symbol = dlsym(handle, symbolName);
    if (!symbol) {
        g_lastError = static_cast<uint32_t>(errno);
        OSTP_LOG_DEBUG("dlsym('{}') failed: {}", symbolName, dlerror() ? dlerror() : "unknown");
    }
    return symbol;
}

void* GetSymbol(ModuleHandle, uint16_t) {
    return nullptr;
}

uint32_t GetLastErrorCode() {
    return g_lastError;
}

std::string GetCurrentDirectoryPath() {
    std::error_code ec;
    auto current = std::filesystem::current_path(ec);
    return ec ? std::string{} : current.string();
}

std::string GetSystemDirectoryPath() {
    return "/usr/lib";
}

std::filesystem::path GetModuleDirectory(ModuleHandle module) {
    return FindPathByHeader(static_cast<const mach_header*>(module)).parent_path();
}

std::filesystem::path GetMainExecutablePath() {
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return {};
    }
    buffer.resize(std::strlen(buffer.c_str()));
    return CanonicalPath(buffer);
}

} // namespace OSTPlatform::DynamicLibrary
