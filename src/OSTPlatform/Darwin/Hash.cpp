#include "include/Hash.h"

#include <CommonCrypto/CommonDigest.h>

#include <array>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace OSTPlatform::Hash {

std::string Sha256OfFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};

    CC_SHA256_CTX ctx{};
    CC_SHA256_Init(&ctx);

    std::array<char, 64 * 1024> buffer{};
    while (file) {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = file.gcount();
        if (got > 0) {
            CC_SHA256_Update(&ctx, buffer.data(), static_cast<CC_LONG>(got));
        }
    }

    std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> digest{};
    CC_SHA256_Final(digest.data(), &ctx);

    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (unsigned char byte : digest) {
        out << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return out.str();
}

} // namespace OSTPlatform::Hash
