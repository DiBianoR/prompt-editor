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
};

struct Topic {
    std::string id;
    std::string name;
    std::string color;  // "#RRGGBB"; subtopics inherit their topic's color
    std::string note;
    std::vector<Range> ranges;
    std::vector<Topic> subtopics;  // one level only
};

// Document text that no topic claims.
struct Gap {
    size_t start = 0;
    size_t end = 0;
    std::string exact;
};

struct Index {
    int version = 1;
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
