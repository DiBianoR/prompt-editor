#include "Text.h"

#include <algorithm>
#include <regex>
#include <stdexcept>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>

namespace pe {

namespace {

bool isContinuation(unsigned char c) { return (c & 0xC0) == 0x80; }

// Decodes the code point at i and returns its byte length (1 for invalid bytes).
size_t decode(const std::string& s, size_t i, char32_t& cp) {
    const auto c = static_cast<unsigned char>(s[i]);
    size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
    if (i + len > s.size()) len = 1;
    if (len == 1) {
        cp = c;
        return 1;
    }
    cp = c & (0x7F >> len);
    for (size_t k = 1; k < len; ++k) {
        const auto cc = static_cast<unsigned char>(s[i + k]);
        if (!isContinuation(cc)) {
            cp = c;
            return 1;
        }
        cp = (cp << 6) | (cc & 0x3F);
    }
    return len;
}

bool isSpace(char32_t cp) {
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f' || cp == '\v' ||
           cp == 0xA0 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x202F || cp == 0x3000;
}

// ASCII replacement for typographic characters, or nullptr to keep the original.
const char* fold(char32_t cp) {
    switch (cp) {
        case 0x2018: case 0x2019: case 0x201A: case 0x201B: case 0x2032: return "'";
        case 0x201C: case 0x201D: case 0x201E: case 0x201F: case 0x2033: return "\"";
        case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015:
        case 0x2212: return "-";
        case 0x2026: return "...";
        default: return nullptr;
    }
}

bool isAsciiSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

bool isStructureLine(const std::string& line) {
    static const std::regex structure(R"(^(#+ .*|[-*+]|\d+[.)])$)");
    return std::regex_match(line, structure);
}

}  // namespace

size_t NormalizedText::fromOriginal(size_t offset) const {
    return std::lower_bound(origStart.begin(), origStart.end(), offset) - origStart.begin();
}

NormalizedText normalize(const std::string& s) {
    NormalizedText n;
    n.text.reserve(s.size());
    n.origStart.reserve(s.size());
    n.origEnd.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        char32_t cp;
        const size_t len = decode(s, i, cp);
        if (isSpace(cp)) {
            size_t j = i + len;
            while (j < s.size()) {
                char32_t next;
                const size_t nlen = decode(s, j, next);
                if (!isSpace(next)) break;
                j += nlen;
            }
            n.text.push_back(' ');
            n.origStart.push_back(i);
            n.origEnd.push_back(j);
            i = j;
            continue;
        }
        if (const char* folded = fold(cp)) {
            for (const char* p = folded; *p; ++p) {
                n.text.push_back(*p);
                n.origStart.push_back(i);
                n.origEnd.push_back(i + len);
            }
        } else {
            for (size_t k = 0; k < len; ++k) {
                n.text.push_back(s[i + k]);
                n.origStart.push_back(i);
                n.origEnd.push_back(i + len);
            }
        }
        i += len;
    }
    return n;
}

std::string normalizedKey(const std::string& s) {
    std::string t = normalize(s).text;
    const size_t a = t.find_first_not_of(' ');
    if (a == std::string::npos) return {};
    return t.substr(a, t.find_last_not_of(' ') - a + 1);
}

std::string contextBefore(const std::string& doc, size_t offset, size_t chars) {
    offset = std::min(offset, doc.size());
    size_t start = offset;
    for (size_t n = 0; n < chars && start > 0; ++n) {
        --start;
        while (start > 0 && isContinuation(static_cast<unsigned char>(doc[start]))) --start;
    }
    return doc.substr(start, offset - start);
}

std::string contextAfter(const std::string& doc, size_t offset, size_t chars) {
    offset = std::min(offset, doc.size());
    size_t end = offset;
    for (size_t n = 0; n < chars && end < doc.size(); ++n) {
        char32_t cp;
        end += decode(doc, end, cp);
    }
    return doc.substr(offset, end - offset);
}

std::vector<Gap> findUnindexed(const std::string& doc, std::vector<std::pair<size_t, size_t>> covered) {
    std::sort(covered.begin(), covered.end());
    covered.emplace_back(doc.size(), doc.size());
    std::vector<Gap> gaps;
    size_t pos = 0;
    for (const auto& [start, end] : covered) {
        if (start > pos) {
            // Split the gap into lines, then drop structural lines at either end.
            struct Line { size_t a, b; };
            std::vector<Line> lines;
            size_t a = pos;
            while (a < start) {
                size_t b = doc.find('\n', a);
                if (b == std::string::npos || b > start) b = start;
                size_t ta = a, tb = b;
                while (ta < tb && isAsciiSpace(doc[ta])) ++ta;
                while (tb > ta && isAsciiSpace(doc[tb - 1])) --tb;
                if (tb > ta) lines.push_back({ta, tb});
                a = b + 1;
            }
            auto structural = [&](const Line& l) { return isStructureLine(doc.substr(l.a, l.b - l.a)); };
            size_t first = 0, last = lines.size();
            while (first < last && structural(lines[first])) ++first;
            while (last > first && structural(lines[last - 1])) --last;
            if (first < last) {
                const size_t ga = lines[first].a, gb = lines[last - 1].b;
                gaps.push_back({ga, gb, doc.substr(ga, gb - ga)});
            }
        }
        pos = std::max(pos, end);
    }
    return gaps;
}

size_t countChars(const std::string& s) {
    size_t n = 0;
    for (char c : s) n += !isContinuation(static_cast<unsigned char>(c));
    return n;
}

std::array<size_t, kTierCount + 1> coverageByTier(const std::string& doc, const std::vector<TierSpan>& spans) {
    std::array<size_t, kTierCount + 1> counts{};
    if (spans.empty()) return counts;
    // Rank per byte: tiers 1..5 as themselves, unrated as 6 (below any tier), 0xFF uncovered.
    constexpr unsigned char kNone = 0xFF, kUnrated = kTierCount + 1;
    std::vector<unsigned char> best(doc.size(), kNone);
    for (const TierSpan& s : spans) {
        const unsigned char rank = s.tier >= 1 && s.tier <= kTierCount ? static_cast<unsigned char>(s.tier) : kUnrated;
        for (size_t b = std::min(s.start, doc.size()); b < std::min(s.end, doc.size()); ++b)
            best[b] = std::min(best[b], rank);
    }
    for (size_t b = 0; b < doc.size(); ++b)
        if (best[b] != kNone && !isContinuation(static_cast<unsigned char>(doc[b])))
            ++counts[best[b] == kUnrated ? 0 : best[b]];
    return counts;
}

std::string sha256Hex(const std::string& bytes) {
    unsigned char digest[32];
    NTSTATUS status = BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
                                 reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
                                 static_cast<ULONG>(bytes.size()), digest, sizeof digest);
    if (status != 0) throw std::runtime_error("BCryptHash failed");
    static const char hex[] = "0123456789abcdef";
    std::string out;
    for (unsigned char b : digest) {
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 15]);
    }
    return out;
}

}  // namespace pe
