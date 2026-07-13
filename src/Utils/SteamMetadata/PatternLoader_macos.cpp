// Signatures are loaded from root-level steam-monitor TOML embedded at build
// time and bound to an exact (component, SHA-256) pair. Unknown builds fail
// closed instead of scanning with stale bytes.

#include "PatternLoader.h"

#include "include/ByteSearch.h"
#include "include/Hash.h"
#include "include/Numbers.h"
#include "SteamMonitorData.gen.h"
#include "Utils/Logging/Log.h"
#include "Utils/SteamMetadata/SteamDiagnostics.h"
#include "Utils/Support/FnvHash.h"

#include <mach-o/loader.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <toml++/toml.hpp>

namespace {

// Local equivalent of a Windows hash-keyed RVA/signature entry.
struct ManualSignature {
    std::string          name;
    uintptr_t            rva = 0;
    std::vector<uint8_t> bytes;
};

// Cached Mach-O section ranges (runtime addresses) for a loaded module.
struct ModuleInfo {
    const uint8_t* imageBase = nullptr;
    const uint8_t* textStart = nullptr;
    size_t         textSize  = 0;
    const char*    cstrStart = nullptr;
    size_t         cstrSize  = 0;
    bool           valid     = false;
    bool           metadataLoaded = false;
    // Exact on-disk build identity and the signature table bound to it. An
    // unknown hash leaves the vector empty so manual lookup fails closed.
    std::string                  sha256;
    std::vector<ManualSignature> manualSignatures;
};

std::unordered_map<OSTPlatform::DynamicLibrary::ModuleHandle, ModuleInfo> g_modules;
std::unordered_set<OSTPlatform::DynamicLibrary::ModuleHandle>             g_failed;
std::vector<std::string>                                                 g_missing;

// Parse a loaded mach_header_64: compute the slide from __TEXT and record the
// runtime ranges needed by signature and approved anchor lookup.
ModuleInfo ParseModule(const mach_header_64* header) {
    ModuleInfo info;
    if (!header || header->magic != MH_MAGIC_64) return info;
    info.imageBase = reinterpret_cast<const uint8_t*>(header);

    // First find __TEXT's vmaddr to derive the slide (runtime = static + slide).
    uintptr_t slide = 0;
    bool haveSlide = false;
    const auto* cmd = reinterpret_cast<const load_command*>(header + 1);
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        if (cmd->cmd == LC_SEGMENT_64) {
            auto* seg = reinterpret_cast<const segment_command_64*>(cmd);
            if (std::strcmp(seg->segname, "__TEXT") == 0) {
                slide = reinterpret_cast<uintptr_t>(header) - static_cast<uintptr_t>(seg->vmaddr);
                haveSlide = true;
                break;
            }
        }
        cmd = reinterpret_cast<const load_command*>(
            reinterpret_cast<const uint8_t*>(cmd) + cmd->cmdsize);
    }
    if (!haveSlide) return info;

    // Second pass: locate the __text and __cstring sections.
    cmd = reinterpret_cast<const load_command*>(header + 1);
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        if (cmd->cmd == LC_SEGMENT_64) {
            auto* seg = reinterpret_cast<const segment_command_64*>(cmd);
            auto* sect = reinterpret_cast<const section_64*>(seg + 1);
            for (uint32_t s = 0; s < seg->nsects; ++s) {
                if (std::strcmp(sect[s].sectname, "__text") == 0) {
                    info.textStart = reinterpret_cast<const uint8_t*>(sect[s].addr + slide);
                    info.textSize  = static_cast<size_t>(sect[s].size);
                } else if (std::strcmp(sect[s].sectname, "__cstring") == 0) {
                    info.cstrStart = reinterpret_cast<const char*>(sect[s].addr + slide);
                    info.cstrSize  = static_cast<size_t>(sect[s].size);
                }
            }
        }
        cmd = reinterpret_cast<const load_command*>(
            reinterpret_cast<const uint8_t*>(cmd) + cmd->cmdsize);
    }

    info.valid = info.textStart != nullptr;
    return info;
}

const ModuleInfo* GetModule(OSTPlatform::DynamicLibrary::ModuleHandle module) {
    auto it = g_modules.find(module);
    if (it != g_modules.end()) return &it->second;
    ModuleInfo info = ParseModule(static_cast<const mach_header_64*>(module));
    auto [ins, _] = g_modules.emplace(module, info);
    return &ins->second;
}

bool IsApprovedStringAnchor(const char* funcName) {
    return std::strcmp(funcName, "BuildDepotDependency") == 0 ||
           std::strcmp(funcName, "CheckAppOwnership") == 0;
}

const char* FindCString(const ModuleInfo& info, const char* needle) {
    if (!info.cstrStart || info.cstrSize == 0) return nullptr;

    const size_t len = std::strlen(needle);
    const char* p = info.cstrStart;
    const char* end = info.cstrStart + info.cstrSize;
    while (p + len < end) {
        const size_t candidateCount = static_cast<size_t>(end - p) - len;
        const char* hit = static_cast<const char*>(
            std::memchr(p, needle[0], candidateCount));
        if (!hit) break;

        const bool atStart = (hit == info.cstrStart) || (hit[-1] == '\0');
        if (atStart && std::memcmp(hit, needle, len) == 0 && hit[len] == '\0') {
            return hit;
        }
        p = hit + 1;
    }
    return nullptr;
}

void* LocateFunctionReferencing(const ModuleInfo& info, uintptr_t target) {
    const uint32_t* code = reinterpret_cast<const uint32_t*>(info.textStart);
    const size_t n = info.textSize / 4;

    uintptr_t adrpPage[32] = {0};
    bool adrpValid[32] = {false};
    uintptr_t funcEntry = reinterpret_cast<uintptr_t>(info.textStart);
    bool atBoundary = true;

    for (size_t i = 0; i < n; ++i) {
        const uintptr_t pc = reinterpret_cast<uintptr_t>(code + i);
        const uint32_t instr = code[i];

        if (atBoundary) {
            funcEntry = pc;
            atBoundary = false;
        }

        if ((instr & 0x9F000000u) == 0x90000000u) {
            const int rd = instr & 0x1F;
            int64_t imm = ((instr >> 29) & 0x3) |
                          (((instr >> 5) & 0x7FFFF) << 2);
            imm = (imm << 43) >> 43;
            adrpPage[rd] = (pc & ~0xFFFULL) +
                           (static_cast<uintptr_t>(imm) << 12);
            adrpValid[rd] = true;
        } else if ((instr & 0xFFC00000u) == 0x91000000u) {
            const int rn = (instr >> 5) & 0x1F;
            const uint32_t imm12 = (instr >> 10) & 0xFFF;
            if (adrpValid[rn] && adrpPage[rn] + imm12 == target) {
                return reinterpret_cast<void*>(funcEntry);
            }
        }

        if (instr == 0xD65F03C0u ||
            (instr & 0xFC000000u) == 0x14000000u) {
            atBoundary = true;
        }
    }
    return nullptr;
}

void* FindApprovedStringAnchor(const ModuleInfo& info, const char* funcName) {
    if (!info.metadataLoaded || !IsApprovedStringAnchor(funcName)) return nullptr;

    const char* anchor = FindCString(info, funcName);
    if (!anchor) return nullptr;
    return LocateFunctionReferencing(info, reinterpret_cast<uintptr_t>(anchor));
}

// ── embedded signature metadata ────────────────────────────────────────────

int HexNibble(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool ParseSignature(std::string_view text, std::vector<uint8_t>& bytes) {
    bytes.clear();

    for (size_t i = 0; i < text.size();) {
        if (text[i] == ' ' || text[i] == '\t' || text[i] == ',') {
            ++i;
            continue;
        }
        if (i + 1 >= text.size()) return false;

        const int hi = HexNibble(text[i]);
        const int lo = HexNibble(text[i + 1]);
        if (hi < 0 || lo < 0) return false;

        bytes.push_back(static_cast<uint8_t>((hi << 4) | lo));
        i += 2;
    }

    return !bytes.empty();
}

std::vector<ManualSignature> ParsePatternData(std::string_view body,
                                              std::string_view component,
                                              std::string_view sha256) {
    toml::table root;
    try {
        root = toml::parse(body);
    } catch (const toml::parse_error& error) {
        LOG_WARN("PatternLoader: embedded TOML parse failed for {}/{}: {}",
                 component, sha256, error.description());
        return {};
    }

    std::vector<ManualSignature> signatures;
    signatures.reserve(root.size());

    for (auto& [rawKey, value] : root) {
        if (!value.is_table()) continue;
        const auto& table = *value.as_table();

        const auto name = table["name"].value<std::string>();
        const auto sig = table["sig"].value<std::string>();
        if (!name || !sig) {
            LOG_WARN("PatternLoader: embedded entry [{}] for {}/{} lacks name or sig",
                     rawKey.str(), component, sha256);
            continue;
        }

        const auto key = OSTPlatform::Numbers::ParseHexUInt32(
            std::string(rawKey.str()));
        const uint32_t expected = Fnv1aHash(name->c_str());
        if (!key || *key != expected) {
            LOG_WARN("PatternLoader: embedded entry [{}] for '{}' has an invalid FNV key",
                     rawKey.str(), *name);
            continue;
        }

        ManualSignature entry;
        entry.name = *name;
        if (const auto rvaText = table["rva"].value<std::string>()) {
            const auto rva = OSTPlatform::Numbers::ParseHexUInt64(*rvaText);
            if (!rva || *rva == 0 ||
                *rva > static_cast<uint64_t>(std::numeric_limits<uintptr_t>::max())) {
                LOG_WARN("PatternLoader: embedded RVA for '{}' is malformed: '{}'",
                         entry.name, *rvaText);
            } else {
                entry.rva = static_cast<uintptr_t>(*rva);
            }
        }
        if (!ParseSignature(*sig, entry.bytes)) {
            LOG_WARN("PatternLoader: embedded signature for '{}' is malformed", entry.name);
            continue;
        }
        signatures.push_back(std::move(entry));
    }

    return signatures;
}

void ShowUnsupportedPopup(const std::string& dllPath, const std::string& sha256) {
    const std::string dllName = std::filesystem::path(dllPath).filename().string();
    SteamDiagnostics::ShowWarning(
        "OpenSteamTool - Unsupported Steam Version",
        "OpenSteamTool: signature file not found for " + dllName + ".\n\n"
        "Hooks that depend on " + dllName + " are disabled for this session; "
        "other modules are unaffected.\n\n"
        "Install an OpenSteamTool build that supports this Steam version, then "
        "restart Steam.\n\n"
        "SHA-256:\n  " + (sha256.empty() ? "(hash failed)" : sha256) + "\n\n"
        "Report this at:\n"
        "https://github.com/OpenSteam001/OpenSteamTool/issues");
}

// Prefer a module-relative RVA and verify its bytes; fall back to scanning
// __text when the RVA is absent or stale.
void* FindManualSignature(const ModuleInfo& info, const char* funcName) {
    for (const auto& sig : info.manualSignatures) {
        if (sig.name != funcName) continue;

        const uintptr_t imageBase = reinterpret_cast<uintptr_t>(info.imageBase);
        const uintptr_t textStart = reinterpret_cast<uintptr_t>(info.textStart);
        const uintptr_t textEnd = textStart + info.textSize;

        if (sig.rva != 0 &&
            sig.rva <= std::numeric_limits<uintptr_t>::max() - imageBase) {
            const uintptr_t candidate = imageBase + sig.rva;
            const bool insideText = candidate >= textStart &&
                                    candidate <= textEnd &&
                                    sig.bytes.size() <= textEnd - candidate;
            if (insideText &&
                std::memcmp(reinterpret_cast<const void*>(candidate),
                            sig.bytes.data(), sig.bytes.size()) == 0) {
                LOG_DEBUG("PatternLoader: {} resolved via RVA 0x{:x}",
                          funcName, sig.rva);
                return reinterpret_cast<void*>(candidate);
            }

            LOG_WARN("PatternLoader: RVA 0x{:x} for '{}' is outside __text or "
                     "failed signature validation; scanning signature",
                     sig.rva, funcName);
        }

        const auto haystack = std::span<const uint8_t>(info.textStart, info.textSize);
        const auto pattern = std::span<const uint8_t>(sig.bytes);
        const auto off = OSTPlatform::ByteSearch::Find(haystack, pattern);
        if (!off) return nullptr;

        auto* address = const_cast<uint8_t*>(info.textStart) + *off;
        const uintptr_t actualRva = reinterpret_cast<uintptr_t>(address) - imageBase;
        LOG_DEBUG("PatternLoader: {} resolved via signature scan at RVA 0x{:x}",
                  funcName, actualRva);
        return address;
    }
    return nullptr;
}

} // namespace

namespace PatternLoader {

bool Load(OSTPlatform::DynamicLibrary::ModuleHandle module,
          const std::string& dllPath, const std::string& component) {
    const ModuleInfo* info = GetModule(module);
    if (!info || !info->valid) {
        LOG_WARN("PatternLoader: failed to parse Mach-O sections for '{}'", component);
        g_failed.insert(module);
        return false;
    }

    auto it = g_modules.find(module);
    if (it == g_modules.end() || dllPath.empty()) return true;

    it->second.manualSignatures.clear();
    it->second.metadataLoaded = false;
    it->second.sha256 = OSTPlatform::Hash::Sha256OfFile(dllPath);
    const auto* embedded = SteamMonitorData::Find(
        "pattern", component, it->second.sha256);

    if (!embedded) {
        LOG_WARN("PatternLoader: no embedded metadata matches '{}' hash {}",
                 component, it->second.sha256);
        ShowUnsupportedPopup(dllPath, it->second.sha256);
        g_failed.insert(module);
        return false;
    }

    it->second.manualSignatures = ParsePatternData(
        embedded->body, component, it->second.sha256);
    if (it->second.manualSignatures.empty()) {
        LOG_WARN("PatternLoader: embedded metadata for {}/{} contains no valid signatures",
                 component, it->second.sha256);
        ShowUnsupportedPopup(dllPath, it->second.sha256);
        g_failed.insert(module);
        return false;
    }

    it->second.metadataLoaded = true;
    LOG_INFO("PatternLoader: loaded {} embedded signatures for {} ({})",
             it->second.manualSignatures.size(), component, it->second.sha256);
    return true;
}

// Windows has a standalone RepeatedField<uint32>::Add(), but AppleClang always
// inlines it into every caller here, leaving no addressable function body.
// Never resolvable, so it's excluded from the missing-signature report instead
// of nagging the user to file an unfixable issue every launch.
bool IsKnownArchitecturalGap(const char* funcName) {
    return std::strcmp(funcName, "RepeatedFieldUint32_Add") == 0;
}

void* FindPattern(OSTPlatform::DynamicLibrary::ModuleHandle module, const char* funcName) {
    if (g_failed.count(module)) return nullptr;
    if (IsKnownArchitecturalGap(funcName)) {
        LOG_DEBUG("PatternLoader: '{}' has no addressable form on macOS (inlined by compiler), skipping",
                  funcName);
        return nullptr;
    }

    const ModuleInfo* info = GetModule(module);
    if (!info || !info->valid) {
        LOG_WARN("PatternLoader: FindPattern called for invalid module ('{}')", funcName);
        g_missing.emplace_back(funcName);
        return nullptr;
    }

    if (void* manual = FindManualSignature(*info, funcName)) {
        return manual;
    }

    if (void* anchor = FindApprovedStringAnchor(*info, funcName)) {
        LOG_DEBUG("PatternLoader: {} resolved via approved string anchor", funcName);
        return anchor;
    }

    LOG_WARN("PatternLoader: no valid signature for '{}'", funcName);
    g_missing.emplace_back(funcName);
    return nullptr;
}

void ReportMissingFunctions() {
    if (g_missing.empty()) return;

    std::string list;
    for (const auto& name : g_missing) {
        list += "  - " + name + "\n";
    }
    g_missing.clear();

    SteamDiagnostics::ShowWarning(
        "OpenSteamTool - Missing Signatures",
        "OpenSteamTool: some functions could not be located.\n\n"
        "The following functions were not found in the signature file:\n" +
        list +
        "\nHooks for these functions are disabled for this session.\n\n"
        "Please report this at:\n"
        "https://github.com/OpenSteam001/OpenSteamTool/issues");
}

} // namespace PatternLoader
