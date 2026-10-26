#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "Index.h"
#include "Text.h"

namespace pe {

// Turns the ranges an LLM wrote into exact spans of the document, forgiving the
// usual slips: line-number prefixes copied into a quote, curly vs straight
// quotes, collapsed whitespace, a quote shortened with "...", a word or two
// changed, and boundaries that cut a word in half. Range forms:
//   {"lines": "12-14"}                        whole lines (list markers left out)
//   {"quote": "text", "line": 12}             a passage; "line" is an optional hint
//   {"from": "start", "to": "end", "line": 12}
// Offsets are UTF-8 byte offsets. Status is Exact, Normalized, Fuzzy or Ambiguous.

struct Span {
    size_t start = 0;
    size_t end = 0;
    AnchorStatus status = AnchorStatus::Exact;
    int tier = 0;  // from the range's "tier", 1..5; 0 = not given
};

class ResolveError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class SourceDocument {
public:
    explicit SourceDocument(std::string text);

    const std::string& text() const { return text_; }
    const NormalizedText& normalized() const { return norm_; }
    size_t lineCount() const { return lineStarts_.size(); }
    // [start, end) of 1-based line n, without its line ending.
    std::pair<size_t, size_t> lineSpan(size_t n) const;
    size_t lineOf(size_t offset) const;
    // The document with right-aligned line numbers ("12| text"), for prompts;
    // optionally only lines first..last.
    std::string numbered(size_t first = 1, size_t last = 0) const;

private:
    std::string text_;
    std::vector<size_t> lineStarts_;
    NormalizedText norm_;
};

// Strips what LLMs add around quotes: line-number prefixes, wrapping quote
// marks or backticks, surrounding whitespace.
std::string cleanQuote(const std::string& s);

// Throws ResolveError when the range cannot be matched.
Span resolveRange(const SourceDocument& doc, const nlohmann::json& item);

// Sorts spans and merges ones that overlap or are separated only by whitespace;
// a merged span keeps the most load-bearing (lowest) tier of its parts.
std::vector<Span> mergeSpans(std::vector<Span> spans, const std::string& text);

AnchorStatus worse(AnchorStatus a, AnchorStatus b);

}  // namespace pe
