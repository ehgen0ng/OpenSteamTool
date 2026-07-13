#include "include/Process.h"

#include "include/Encoding.h"
#include "include/Log.h"

#include <libproc.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <sys/proc_info.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

// macOS process backend, mirroring Windows/Process.cpp. The reliable, no-special-
// entitlement primitives are: proc_pidpath / proc_pidinfo (libproc) for a process
// the caller can see, KERN_PROCARGS2 (sysctl) for its environment, and the _dyld_*
// image list for the CURRENT process. Cross-process module enumeration needs
// task_for_pid (root/entitlement) and is intentionally degraded to empty.
namespace OSTPlatform::Process {
namespace {

// Creation time is encoded as microseconds since the Unix epoch. Both the
// producer (GetCreationTime) and the consumer (FormatCreationTime) live here, so
// this local convention replaces the Windows FILETIME with no external contract.
constexpr uint64_t kMicrosPerSecond = 1000000;

uint32_t ImageMappedSize(const mach_header_64* header) {
    if (!header) return 0;

    uint64_t minAddr = (std::numeric_limits<uint64_t>::max)();
    uint64_t maxEnd = 0;
    const auto* command = reinterpret_cast<const load_command*>(header + 1);
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        if (command->cmd == LC_SEGMENT_64) {
            const auto* segment = reinterpret_cast<const segment_command_64*>(command);
            // Skip file-less segments (notably __PAGEZERO, whose 4 GiB vmsize would
            // swamp the span); the mapped image is the extent of file-backed ones.
            if (segment->filesize > 0) {
                if (segment->vmaddr < minAddr) minAddr = segment->vmaddr;
                const uint64_t end = segment->vmaddr + segment->vmsize;
                if (end > maxEnd) maxEnd = end;
            }
        }
        command = reinterpret_cast<const load_command*>(
            reinterpret_cast<const uint8_t*>(command) + command->cmdsize);
    }

    if (maxEnd <= minAddr) return 0;
    const uint64_t span = maxEnd - minAddr;
    return span > (std::numeric_limits<uint32_t>::max)()
        ? (std::numeric_limits<uint32_t>::max)()
        : static_cast<uint32_t>(span);
}

} // namespace

std::optional<uint64_t> GetCreationTime(uint32_t pid) {
    struct proc_bsdinfo info{};
    const int got = proc_pidinfo(static_cast<int>(pid), PROC_PIDTBSDINFO, 0,
                                 &info, PROC_PIDTBSDINFO_SIZE);
    if (got != PROC_PIDTBSDINFO_SIZE) {
        OSTP_LOG_DEBUG("GetCreationTime(pid={}) proc_pidinfo failed (got={}, errno={})", pid, got, errno);
        return std::nullopt;
    }
    return static_cast<uint64_t>(info.pbi_start_tvsec) * kMicrosPerSecond +
           static_cast<uint64_t>(info.pbi_start_tvusec);
}

std::string FormatCreationTime(uint64_t fileTime) {
    if (fileTime == 0) return "-";

    const time_t seconds = static_cast<time_t>(fileTime / kMicrosPerSecond);
    const long millis = static_cast<long>((fileTime % kMicrosPerSecond) / 1000);

    struct tm local{};
    if (!localtime_r(&seconds, &local)) {
        char buffer[48];
        std::snprintf(buffer, sizeof(buffer), "micros=%llu", static_cast<unsigned long long>(fileTime));
        return buffer;
    }

    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
                  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min, local.tm_sec, millis);
    return buffer;
}

std::optional<std::string> GetImagePath(uint32_t pid) {
    char buffer[PROC_PIDPATHINFO_MAXSIZE];
    const int length = proc_pidpath(static_cast<int>(pid), buffer, sizeof(buffer));
    if (length <= 0) {
        OSTP_LOG_DEBUG("GetImagePath(pid={}) proc_pidpath failed (errno={})", pid, errno);
        return std::nullopt;
    }
    return std::string(buffer, static_cast<size_t>(length));
}

std::optional<std::string> GetEnvironmentVariableValue(uint32_t pid, std::wstring_view name) {
    if (name.empty()) return std::nullopt;
    const std::string needle = Encoding::WideToUtf8(name) + "=";

    int argmax = 0;
    size_t argmaxSize = sizeof(argmax);
    int argmaxMib[2] = {CTL_KERN, KERN_ARGMAX};
    if (sysctl(argmaxMib, 2, &argmax, &argmaxSize, nullptr, 0) != 0 || argmax <= 0) {
        OSTP_LOG_DEBUG("GetEnvironmentVariableValue: KERN_ARGMAX failed (errno={})", errno);
        return std::nullopt;
    }

    std::vector<char> buffer(static_cast<size_t>(argmax));
    int mib[3] = {CTL_KERN, KERN_PROCARGS2, static_cast<int>(pid)};
    size_t size = buffer.size();
    if (sysctl(mib, 3, buffer.data(), &size, nullptr, 0) != 0) {
        // Fails for processes owned by another user (EINVAL/EPERM); degrade quietly.
        OSTP_LOG_DEBUG("GetEnvironmentVariableValue: KERN_PROCARGS2(pid={}) failed (errno={})", pid, errno);
        return std::nullopt;
    }
    if (size < sizeof(int)) return std::nullopt;

    // Layout: [int argc][exec_path\0][NUL padding][argv[0]\0 ... argv[argc-1]\0]
    //         [env[0]\0 ... env[n]\0]. Walk past argc arguments to reach env.
    int argc = 0;
    std::memcpy(&argc, buffer.data(), sizeof(argc));

    const char* p = buffer.data() + sizeof(argc);
    const char* end = buffer.data() + size;

    const auto skipString = [&] { while (p < end && *p != '\0') ++p; };
    const auto skipNuls = [&] { while (p < end && *p == '\0') ++p; };

    skipString();  // exec path
    skipNuls();    // padding after exec path

    for (int i = 0; i < argc && p < end; ++i) {
        skipString();
        skipNuls();
    }

    while (p < end && *p != '\0') {
        const char* entryStart = p;
        skipString();
        const std::string_view entry(entryStart, static_cast<size_t>(p - entryStart));
        if (entry.size() >= needle.size() && entry.compare(0, needle.size(), needle) == 0) {
            return std::string(entry.substr(needle.size()));
        }
        skipNuls();
    }

    return std::nullopt;
}

std::vector<ModuleInfo> EnumerateModules(uint32_t pid) {
    std::vector<ModuleInfo> modules;

    const uint32_t self = static_cast<uint32_t>(getpid());
    if (pid != 0 && pid != self) {
        // task_for_pid + remote dyld image walk requires elevated entitlements; the
        // in-process injection model only ever needs the current process's images.
        OSTP_LOG_DEBUG("EnumerateModules(pid={}): cross-process enumeration unsupported (self={})", pid, self);
        return modules;
    }

    const uint32_t count = _dyld_image_count();
    modules.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const char* name = _dyld_get_image_name(i);
        const mach_header* mh = _dyld_get_image_header(i);
        if (!name || !mh) continue;

        const auto* header = reinterpret_cast<const mach_header_64*>(mh);
        if (header->magic != MH_MAGIC_64) continue;  // arm64/x86_64 only

        std::string path(name);
        if (path.empty()) continue;

        std::filesystem::path nativePath(path);
        modules.push_back(ModuleInfo{
            std::move(path),
            std::move(nativePath),
            ImageMappedSize(header),
            header->filetype == MH_EXECUTE,
        });
    }
    return modules;
}

bool IsSystemModulePath(const std::string& path) {
    // OS-supplied images live under /usr/lib and /System (dyld shared-cache entries
    // report these paths too). /Library is excluded: it also holds third-party code.
    static constexpr std::string_view kSystemPrefixes[] = {"/usr/lib/", "/System/"};
    for (const std::string_view prefix : kSystemPrefixes) {
        if (path.size() >= prefix.size() &&
            std::string_view(path).substr(0, prefix.size()) == prefix) {
            return true;
        }
    }
    return false;
}

} // namespace OSTPlatform::Process
