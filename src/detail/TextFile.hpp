#ifndef CODEAGENT_DETAIL_TEXT_FILE_HPP
#define CODEAGENT_DETAIL_TEXT_FILE_HPP

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

namespace codeagent {
namespace detail {

// True when content is UTF-8 text that the editor and Gemini can round-trip:
// well-formed UTF-8 (an optional byte-order mark included) with no NUL byte.
// A NUL byte is the usual sign of a binary file, and other encodings such as
// UTF-16 or Latin-1 fail the UTF-8 check, so neither is silently re-encoded.
inline bool isUtf8Text(const std::string& content) {
    std::size_t i = 0;
    while (i < content.size()) {
        const auto byte = static_cast<unsigned char>(content[i]);
        if (byte == 0) {
            return false;
        }
        if (byte < 0x80) {
            ++i;
            continue;
        }

        std::size_t length = 0;
        unsigned char low = 0x80;
        unsigned char high = 0xBF;
        if (byte >= 0xC2 && byte <= 0xDF) {
            length = 2;
        } else if (byte >= 0xE0 && byte <= 0xEF) {
            length = 3;
            if (byte == 0xE0) low = 0xA0;   // no overlong forms
            if (byte == 0xED) high = 0x9F;  // no UTF-16 surrogates
        } else if (byte >= 0xF0 && byte <= 0xF4) {
            length = 4;
            if (byte == 0xF0) low = 0x90;   // no overlong forms
            if (byte == 0xF4) high = 0x8F;  // nothing above U+10FFFF
        } else {
            return false;
        }

        if (content.size() - i < length) {
            return false;
        }
        for (std::size_t k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(content[i + k]);
            const unsigned char min = k == 1 ? low : 0x80;
            const unsigned char max = k == 1 ? high : 0xBF;
            if (next < min || next > max) {
                return false;
            }
        }
        i += length;
    }
    return true;
}

// True for environment files that typically hold secrets such as the Gemini
// API key: ".env" and ".env.<name>", but not templates like ".env.example".
// CodeAgent never opens, sends to Gemini, or writes these files.
inline bool isProtectedSecretFile(const std::filesystem::path& path) {
    std::string name = path.filename().string();
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (name == ".env") {
        return true;
    }
    if (name.rfind(".env.", 0) != 0) {
        return false;
    }
    for (const char* suffix : {".example", ".sample", ".template"}) {
        const std::string ending(suffix);
        if (name.size() >= ending.size() &&
            name.compare(name.size() - ending.size(), ending.size(), ending) == 0) {
            return false;
        }
    }
    return true;
}

} // namespace detail
} // namespace codeagent

#endif // CODEAGENT_DETAIL_TEXT_FILE_HPP
