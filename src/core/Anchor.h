#pragma once

#include <array>
#include <string>

#include "Index.h"

namespace pe {

struct AnchorSummary {
    std::array<int, kAnchorStatusCount> counts{};
    bool hashMatched = false;  // offsets were trusted without matching

    int total() const;
    int count(AnchorStatus s) const { return counts[static_cast<int>(s)]; }
};

// Locates one range in doc and updates its start, end and status. Tries, in order:
// the stored offsets, a verbatim search, a normalized search, then approximate
// matching. Ties are broken by prefix/suffix context, then by distance from the
// stored offset.
void anchorRange(Range& range, const std::string& doc);

// Anchors every range. If the index's documentHash matches doc, the stored
// offsets are trusted as they are.
AnchorSummary anchorIndex(Index& index, const std::string& doc);

// Brings the index in line with doc before saving: re-reads exact text and
// context from the tracked offsets, recomputes unindexed text and the hash.
void refreshFromDocument(Index& index, const std::string& doc);

}  // namespace pe
