#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "Index.h"

namespace pe {

// Number of characters (code points) of context stored on each side of a range.
constexpr size_t kContextChars = 32;

// Document text with typographic variants folded together: curly quotes become
// straight, dashes become '-', '…' becomes "...", and whitespace runs become one
// space. Each normalized byte remembers the original bytes it came from.
struct NormalizedText {
    std::string text;
    std::vector<size_t> origStart;  // per normalized byte
    std::vector<size_t> origEnd;

    // Normalized position of an original offset (first byte at or after it).
    size_t fromOriginal(size_t offset) const;
};

NormalizedText normalize(const std::string& s);
// Normalized form of a quote, with surrounding whitespace removed.
std::string normalizedKey(const std::string& s);

// Up to `chars` code points before / after a byte offset.
std::string contextBefore(const std::string& doc, size_t offset, size_t chars = kContextChars);
std::string contextAfter(const std::string& doc, size_t offset, size_t chars = kContextChars);

// Text outside the given [start, end) spans that is more than whitespace and
// markdown scaffolding (headings, bare bullets, list numbers).
std::vector<Gap> findUnindexed(const std::string& doc, std::vector<std::pair<size_t, size_t>> covered);

std::string sha256Hex(const std::string& bytes);

}  // namespace pe
