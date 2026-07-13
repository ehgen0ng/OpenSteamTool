#include "include/SteamCredentialStore.h"

#include "include/Encoding.h"
#include "include/Log.h"
#include "include/Numbers.h"

#include <unistd.h>

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// macOS Steam credential backend. Windows reads/writes the Steam registry
// (HKCU\Software\Valve\Steam\Apps\<id>\{SteamID,AppTicket,ETicket} and
// ActiveProcess\{ActiveUser,Universe}); on macOS Steam mirrors that exact tree to
// ~/Library/Application Support/Steam/registry.vdf (a text VDF file). So this
// backend parses/writes registry.vdf rather than using the Keychain.
//
// SteamID/ActiveUser/Universe are values the real macOS Steam client itself
// writes to registry.vdf, so those getters read Steam's own data. AppTicket/
// ETicket are different: macOS Steam does not persist ownership tickets to
// registry.vdf or elsewhere under
// ~/Library/Application Support/Steam — it obtains them at runtime over IPC
// instead. So Apps/<id>/{AppTicket,ETicket} is OpenSteamTool's own leaf, not
// Steam's: the only writer is OpenSteamTool itself (Lua setappticket/
// seteticket, see LuaConfig.cpp; REG_BINARY on Windows, hex-encoded text here),
// and the getters just read that back for the IPC/netpacket handlers and the
// AppTicket forge path (Hooks_Decryption) to consume.
namespace OSTPlatform::SteamCredentialStore {
namespace {

bool IEquals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// A VDF node is either a leaf (holds `value`) or an object (holds ordered
// `children`). Order is preserved so a parse→edit→serialize round-trip does not
// reshuffle the keys Steam itself wrote.
struct VdfNode {
    bool isObject = false;
    std::string value;
    std::vector<std::pair<std::string, VdfNode>> children;

    const VdfNode* FindChild(std::string_view key) const {
        for (const auto& [k, v] : children) {
            if (IEquals(k, key)) return &v;
        }
        return nullptr;
    }

    VdfNode* FindOrAddObject(std::string_view key) {
        for (auto& [k, v] : children) {
            if (IEquals(k, key)) {
                v.isObject = true;
                return &v;
            }
        }
        VdfNode child;
        child.isObject = true;
        children.emplace_back(std::string(key), std::move(child));
        return &children.back().second;
    }

    void SetLeaf(std::string_view key, std::string val) {
        for (auto& [k, v] : children) {
            if (IEquals(k, key)) {
                v.isObject = false;
                v.value = std::move(val);
                v.children.clear();
                return;
            }
        }
        VdfNode leaf;
        leaf.value = std::move(val);
        children.emplace_back(std::string(key), std::move(leaf));
    }
};

// Minimal recursive VDF parser: quoted (or bare) string tokens, `{`/`}` nesting,
// `//` line comments, and the standard \\ \" \t \n \r escapes.
class VdfParser {
public:
    explicit VdfParser(std::string_view text) : s_(text) {}

    bool ParseDocument(VdfNode& root) {
        root.isObject = true;
        return ParsePairs(root, /*expectCloseBrace=*/false);
    }

private:
    void SkipWhitespaceAndComments() {
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
            } else if (c == '/' && pos_ + 1 < s_.size() && s_[pos_ + 1] == '/') {
                pos_ += 2;
                while (pos_ < s_.size() && s_[pos_] != '\n') ++pos_;
            } else {
                break;
            }
        }
    }

    bool ReadQuoted(std::string& out) {
        out.clear();
        ++pos_;  // opening quote
        while (pos_ < s_.size()) {
            const char c = s_[pos_++];
            if (c == '"') return true;
            if (c == '\\' && pos_ < s_.size()) {
                const char e = s_[pos_++];
                switch (e) {
                    case 'n': out.push_back('\n'); break;
                    case 't': out.push_back('\t'); break;
                    case 'r': out.push_back('\r'); break;
                    case '\\': out.push_back('\\'); break;
                    case '"': out.push_back('"'); break;
                    default: out.push_back(e); break;
                }
            } else {
                out.push_back(c);
            }
        }
        return false;  // unterminated
    }

    void ReadBare(std::string& out) {
        out.clear();
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '{' || c == '}') break;
            out.push_back(c);
            ++pos_;
        }
    }

    bool ReadToken(std::string& out) {
        if (pos_ >= s_.size()) return false;
        if (s_[pos_] == '"') return ReadQuoted(out);
        ReadBare(out);
        return !out.empty();
    }

    bool ParsePairs(VdfNode& parent, bool expectCloseBrace) {
        while (true) {
            SkipWhitespaceAndComments();
            if (pos_ >= s_.size()) return !expectCloseBrace;  // EOF valid only at top level
            if (s_[pos_] == '}') {
                if (!expectCloseBrace) return false;
                ++pos_;
                return true;
            }

            std::string key;
            if (!ReadToken(key)) return false;

            SkipWhitespaceAndComments();
            if (pos_ >= s_.size()) return false;

            if (s_[pos_] == '{') {
                ++pos_;
                VdfNode child;
                child.isObject = true;
                if (!ParsePairs(child, /*expectCloseBrace=*/true)) return false;
                parent.children.emplace_back(std::move(key), std::move(child));
            } else {
                std::string value;
                if (!ReadToken(value)) return false;
                VdfNode leaf;
                leaf.value = std::move(value);
                parent.children.emplace_back(std::move(key), std::move(leaf));
            }
        }
    }

    std::string_view s_;
    size_t pos_ = 0;
};

void AppendEscaped(const std::string& in, std::string& out) {
    for (const char c : in) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default: out.push_back(c); break;
        }
    }
}

void Serialize(const VdfNode& node, std::string& out, int depth) {
    const std::string indent(static_cast<size_t>(depth), '\t');
    for (const auto& [key, child] : node.children) {
        out += indent;
        out.push_back('"');
        AppendEscaped(key, out);
        out.push_back('"');
        if (child.isObject) {
            out.push_back('\n');
            out += indent;
            out += "{\n";
            Serialize(child, out, depth + 1);
            out += indent;
            out += "}\n";
        } else {
            out += "\t\t\"";
            AppendEscaped(child.value, out);
            out += "\"\n";
        }
    }
}

std::filesystem::path RegistryPath() {
    const char* home = std::getenv("HOME");
    if (!home || !*home) return {};
    return std::filesystem::path(home) / "Library" / "Application Support" / "Steam" / "registry.vdf";
}

bool ReadWholeFile(const std::filesystem::path& path, std::string& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::ostringstream buffer;
    buffer << file.rdbuf();
    out = buffer.str();
    return true;
}

bool WriteFileAtomic(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::path tmp = path;
    tmp += "." + std::to_string(::getpid()) + ".tmp";

    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!file.good()) {
            file.close();
            std::error_code rmEc;
            std::filesystem::remove(tmp, rmEc);
            return false;
        }
    }

    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);  // atomic within the same directory
    if (ec) {
        std::error_code rmEc;
        std::filesystem::remove(tmp, rmEc);
        return false;
    }
    return true;
}

// Read+parse registry.vdf. Distinguishes "no file yet" (NotFound) from a present
// but unreadable/malformed file (Failed).
Status LoadRegistry(VdfNode& root) {
    const std::filesystem::path path = RegistryPath();
    if (path.empty()) return Status::Failed;

    std::string text;
    if (!ReadWholeFile(path, text)) {
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            OSTP_LOG_WARN("SteamCredentialStore: registry.vdf present but unreadable path={}", path.string());
            return Status::Failed;
        }
        OSTP_LOG_DEBUG("SteamCredentialStore: registry.vdf not present yet path={}", path.string());
        return Status::NotFound;
    }

    if (!VdfParser(text).ParseDocument(root)) {
        OSTP_LOG_WARN("SteamCredentialStore: failed to parse registry.vdf path={}", path.string());
        return Status::Failed;
    }
    return Status::Ok;
}

// Navigate Registry/HKCU/Software/Valve/Steam.
const VdfNode* SteamSubtree(const VdfNode& root) {
    const VdfNode* node = &root;
    for (const char* key : {"Registry", "HKCU", "Software", "Valve", "Steam"}) {
        node = node->FindChild(key);
        if (!node) return nullptr;
    }
    return node;
}

const std::string* LeafValue(const VdfNode* node) {
    return (node && !node->isObject) ? &node->value : nullptr;
}

// AppTicket/ETicket are REG_BINARY on Windows; registry.vdf only has quoted
// text, so the blob is hex-encoded into the leaf value.
std::string BytesToHex(const std::vector<uint8_t>& data) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (const uint8_t byte : data) {
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 0x0F]);
    }
    return out;
}

std::optional<std::vector<uint8_t>> HexToBytes(std::string_view hex) {
    if (hex.empty() || hex.size() % 2 != 0) return std::nullopt;

    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        const auto byte = Numbers::ParseHexUInt8(hex.substr(i, 2));
        if (!byte) return std::nullopt;
        out.push_back(*byte);
    }
    return out;
}

// Shared Apps/<id>/<leafKey> accessors backing GetAppTicket/GetETicket and
// WriteAppTicket/WriteETicket below — same tree GetSteamId/WriteSteamId use.
Status ReadAppLeaf(uint32_t appId, const char* leafKey, std::string& outValue) {
    VdfNode root;
    if (const Status s = LoadRegistry(root); s != Status::Ok) return s;

    const VdfNode* steam = SteamSubtree(root);
    if (!steam) return Status::NotFound;
    const VdfNode* apps = steam->FindChild("Apps");
    if (!apps) return Status::NotFound;
    const VdfNode* app = apps->FindChild(std::to_string(appId));
    if (!app) return Status::NotFound;

    const std::string* raw = LeafValue(app->FindChild(leafKey));
    if (!raw) return Status::NotFound;

    outValue = *raw;
    return Status::Ok;
}

Status WriteAppLeaf(uint32_t appId, const char* leafKey, const std::string& value) {
    VdfNode root;
    // A missing file is fine — start a fresh tree; a malformed one is a hard error
    // (overwriting it would clobber Steam's own keys).
    if (const Status s = LoadRegistry(root); s == Status::Failed) return Status::Failed;

    VdfNode* node = &root;
    for (const char* key : {"Registry", "HKCU", "Software", "Valve", "Steam", "Apps"}) {
        node = node->FindOrAddObject(key);
    }
    VdfNode* app = node->FindOrAddObject(std::to_string(appId));
    app->SetLeaf(leafKey, value);

    std::string out;
    Serialize(root, out, 0);
    return WriteFileAtomic(RegistryPath(), out) ? Status::Ok : Status::Failed;
}

} // namespace

const char* ToString(Status status) {
    switch (status) {
    case Status::Ok: return "Ok";
    case Status::NotFound: return "NotFound";
    case Status::Unsupported: return "Unsupported";
    case Status::Failed: return "Failed";
    }
    return "Unknown";
}

Status GetSteamId(uint32_t appId, uint64_t& steamId) {
    VdfNode root;
    if (const Status s = LoadRegistry(root); s != Status::Ok) return s;

    const VdfNode* steam = SteamSubtree(root);
    if (!steam) return Status::NotFound;
    const VdfNode* apps = steam->FindChild("Apps");
    if (!apps) return Status::NotFound;
    const VdfNode* app = apps->FindChild(std::to_string(appId));
    if (!app) return Status::NotFound;

    const std::string* raw = LeafValue(app->FindChild("SteamID"));
    if (!raw) return Status::NotFound;

    const auto parsed = Numbers::ParseUInt64(*raw);
    if (!parsed || *parsed == 0) {
        OSTP_LOG_DEBUG("SteamCredentialStore: SteamID invalid/empty for appid={}", appId);
        return Status::NotFound;
    }

    steamId = *parsed;
    OSTP_LOG_DEBUG("SteamCredentialStore: read SteamID for appid={} steamid={}", appId, steamId);
    return Status::Ok;
}

Status WriteSteamId(uint32_t appId, uint64_t steamId) {
    VdfNode root;
    // A missing file is fine — start a fresh tree; a malformed one is a hard error
    // (overwriting it would clobber Steam's own keys).
    if (const Status s = LoadRegistry(root); s == Status::Failed) return Status::Failed;

    VdfNode* node = &root;
    for (const char* key : {"Registry", "HKCU", "Software", "Valve", "Steam", "Apps"}) {
        node = node->FindOrAddObject(key);
    }
    VdfNode* app = node->FindOrAddObject(std::to_string(appId));
    app->SetLeaf("SteamID", std::to_string(steamId));

    std::string out;
    Serialize(root, out, 0);
    if (!WriteFileAtomic(RegistryPath(), out)) {
        OSTP_LOG_WARN("SteamCredentialStore: failed to write SteamID for appid={}", appId);
        return Status::Failed;
    }
    OSTP_LOG_INFO("SteamCredentialStore: wrote SteamID for appid={} steamid={}", appId, steamId);
    return Status::Ok;
}

Status GetActiveUser(uint32_t& accountId, std::wstring& universe) {
    VdfNode root;
    if (const Status s = LoadRegistry(root); s != Status::Ok) return s;

    const VdfNode* steam = SteamSubtree(root);
    if (!steam) return Status::NotFound;
    const VdfNode* active = steam->FindChild("ActiveProcess");
    if (!active) return Status::NotFound;

    const std::string* rawUser = LeafValue(active->FindChild("ActiveUser"));
    if (!rawUser) return Status::NotFound;
    const auto parsedUser = Numbers::ParseUInt32(*rawUser);
    if (!parsedUser || *parsedUser == 0) {
        OSTP_LOG_DEBUG("SteamCredentialStore: ActiveUser invalid/zero");
        return Status::NotFound;
    }

    const std::string* rawUniverse = LeafValue(active->FindChild("Universe"));
    if (!rawUniverse || rawUniverse->empty()) return Status::NotFound;

    accountId = *parsedUser;
    universe = Encoding::Utf8ToWide(*rawUniverse);
    OSTP_LOG_DEBUG("SteamCredentialStore: read ActiveUser accountid={} universe={}", accountId, *rawUniverse);
    return Status::Ok;
}

// ── AppTicket / ETicket ──────────────────────────────────────────────────────
// Unlike SteamID/ActiveUser, nothing on macOS reads a Steam-authored ticket
// here — the only writer is OpenSteamTool itself (Lua setAppTicket/setETicket,
// see LuaConfig.cpp), so this pair only needs to round-trip its own writes, not
// match a real Steam on-disk format. A leaf that fails hex-decode (e.g. a stray
// key Steam itself owns) reads back as NotFound rather than throwing.

Status GetAppTicket(uint32_t appId, std::vector<uint8_t>& ticket) {
    std::string hex;
    if (const Status s = ReadAppLeaf(appId, "AppTicket", hex); s != Status::Ok) return s;

    auto bytes = HexToBytes(hex);
    if (!bytes) {
        OSTP_LOG_WARN("SteamCredentialStore: AppTicket leaf not valid hex for appid={}", appId);
        return Status::NotFound;
    }

    ticket = std::move(*bytes);
    OSTP_LOG_DEBUG("SteamCredentialStore: read AppTicket for appid={} ({} bytes)", appId, ticket.size());
    return Status::Ok;
}

Status WriteAppTicket(uint32_t appId, const std::vector<uint8_t>& data) {
    if (WriteAppLeaf(appId, "AppTicket", BytesToHex(data)) != Status::Ok) {
        OSTP_LOG_WARN("SteamCredentialStore: failed to write AppTicket for appid={}", appId);
        return Status::Failed;
    }
    OSTP_LOG_INFO("SteamCredentialStore: wrote AppTicket for appid={} ({} bytes)", appId, data.size());
    return Status::Ok;
}

Status GetETicket(uint32_t appId, std::vector<uint8_t>& ticket) {
    std::string hex;
    if (const Status s = ReadAppLeaf(appId, "ETicket", hex); s != Status::Ok) return s;

    auto bytes = HexToBytes(hex);
    if (!bytes) {
        OSTP_LOG_WARN("SteamCredentialStore: ETicket leaf not valid hex for appid={}", appId);
        return Status::NotFound;
    }

    ticket = std::move(*bytes);
    OSTP_LOG_DEBUG("SteamCredentialStore: read ETicket for appid={} ({} bytes)", appId, ticket.size());
    return Status::Ok;
}

Status WriteETicket(uint32_t appId, const std::vector<uint8_t>& data) {
    if (WriteAppLeaf(appId, "ETicket", BytesToHex(data)) != Status::Ok) {
        OSTP_LOG_WARN("SteamCredentialStore: failed to write ETicket for appid={}", appId);
        return Status::Failed;
    }
    OSTP_LOG_INFO("SteamCredentialStore: wrote ETicket for appid={} ({} bytes)", appId, data.size());
    return Status::Ok;
}

} // namespace OSTPlatform::SteamCredentialStore
