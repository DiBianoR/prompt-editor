#include "Tracker.h"

namespace pe {

void trackInsert(Index& index, size_t pos, size_t length) {
    index.forEachRange([&](Range& r) {
        if (pos <= r.start) {
            r.start += length;
            r.end += length;
        } else if (pos < r.end) {
            r.end += length;
            r.touched = true;
        }
    });
}

void trackDelete(Index& index, size_t pos, size_t length) {
    const size_t del = pos + length;
    auto shift = [&](size_t x) { return x <= pos ? x : x >= del ? x - length : pos; };
    index.forEachRange([&](Range& r) {
        const bool overlaps = pos < r.end && del > r.start;
        const bool hadText = r.end > r.start;
        r.start = shift(r.start);
        r.end = shift(r.end);
        if (overlaps) {
            r.touched = true;
            if (hadText && r.start == r.end) r.status = AnchorStatus::Orphaned;
        }
    });
}

}  // namespace pe
