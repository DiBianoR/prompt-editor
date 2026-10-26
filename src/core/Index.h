#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace pe {

// How a range was located in the current document text.
enum class AnchorStatus {
    Exact,       // stored offsets still hold the stored text
    Moved,       // text found verbatim somewhere else
    Normalized,  // found after normalizing quotes, dashes and whitespace
    Fuzzy,       // approximate match: the text was edited, verify it
    Ambiguous,   // several equally good matches, nearest one chosen
    Orphaned,    // not found, or deleted while editing
};
constexpr int kAnchorStatusCount = 6;

const char* toString(AnchorStatus status);
AnchorStatus statusFromString(const std::string& s);
// Fuzzy, ambiguous and orphaned ranges need a human or the LLM to look at them.
bool needsReview(AnchorStatus status);

struct Range {
    size_t start = 0;  // UTF-8 byte offsets into the document, [start, end)
    size_t end = 0;
    std::string exact;
    std::string prefix;
    std::string suffix;
    bool touched = false;  // edited since the last LLM refresh
    AnchorStatus status = AnchorStatus::Exact;
    int tier = 0;  // how load-bearing: 1 core ... 5 incidental; 0 = not rated
    std::string note;  // what the model's findings said about this passage
};

constexpr int kTierCount = 5;
// "core", "high precedence", "standard", "supporting", "incidental"; "" for 0.
const char* tierName(int tier);
int clampTier(long long tier);  // 1..5, or 0 when out of range

struct Topic {
    std::string id;
    std::string name;
    std::string color;  // "#RRGGBB"; subtopics inherit their topic's color
    std::string note;
    std::vector<Range> ranges;
    std::vector<Topic> subtopics;  // one level only
    // The model's findings for this topic, if any: relative to the document's
    // folder (see the app's documentFolder), or an absolute path in older indexes.
    std::string analysis;
};

// Document text that no topic claims.
struct Gap {
    size_t start = 0;
    size_t end = 0;
    std::string exact;
};

struct Index {
    int version = 1;
    std::string id;  // random, given when first built; finds the document's files whatever it is called
    std::string document;
    std::string documentHash;  // "sha256:<hex>" of the file the offsets refer to
    std::vector<Topic> topics;
    std::vector<Gap> unindexed;

    template <class F> void forEachRange(F&& f) {
        for (Topic& t : topics) {
            for (Range& r : t.ranges) f(r);
            for (Topic& s : t.subtopics)
                for (Range& r : s.ranges) f(r);
        }
    }
};

// Throws std::runtime_error on malformed input.
Index parseIndex(const std::string& json);
std::string serializeIndex(const Index& index);

}  // namespace pe
