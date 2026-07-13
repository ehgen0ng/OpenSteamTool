#include "include/Encoding.h"

namespace OSTPlatform::Encoding {

std::string WideToUtf8(std::wstring_view value) {
    std::string out;
    for (wchar_t wc : value) {
        const uint32_t cp = static_cast<uint32_t>(wc);
        if (cp <= 0x7F) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

std::wstring Utf8ToWide(std::string_view value) {
    std::wstring out;
    for (size_t i = 0; i < value.size();) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        uint32_t cp = 0;
        size_t len = 0;

        if ((c & 0x80) == 0) {
            cp = c;
            len = 1;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < value.size()) {
            cp = ((c & 0x1F) << 6) | (static_cast<unsigned char>(value[i + 1]) & 0x3F);
            len = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < value.size()) {
            cp = ((c & 0x0F) << 12) |
                 ((static_cast<unsigned char>(value[i + 1]) & 0x3F) << 6) |
                 (static_cast<unsigned char>(value[i + 2]) & 0x3F);
            len = 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < value.size()) {
            cp = ((c & 0x07) << 18) |
                 ((static_cast<unsigned char>(value[i + 1]) & 0x3F) << 12) |
                 ((static_cast<unsigned char>(value[i + 2]) & 0x3F) << 6) |
                 (static_cast<unsigned char>(value[i + 3]) & 0x3F);
            len = 4;
        } else {
            cp = '?';
            len = 1;
        }

        out.push_back(static_cast<wchar_t>(cp));
        i += len;
    }
    return out;
}

} // namespace OSTPlatform::Encoding
