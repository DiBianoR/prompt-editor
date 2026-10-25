#include "Anchor.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "Text.h"

namespace pe {

namespace {

constexpr size_t kMinFuzzyLength = 12;     // shorter quotes are too easy to mismatch
constexpr size_t kFullPatternLimit = 400;  // longer quotes are matched by head and tail
constexpr size_t kPieceLength = 160;
constexpr double kMaxErrorRate = 0.3;

struct Candidate {
    size_t start;
    size_t end;
};

size_t absDiff(size_t a, size_t b) { return a > b ? a - b : b - a; }

std::vector<Candidate> findAll(const std::string& text, const std::string& needle) {
    std::vector<Candidate> out;
    if (needle.empty()) return out;
    for (size_t i = text.find(needle); i != std::string::npos; i = text.find(needle, i + 1))
        out.push_back({i, i + needle.size()});
    return out;
}

// How many bytes of context agree on each side of a candidate.
size_t contextScore(const std::string& text, const Candidate& c, const std::string& prefix,
                    const std::string& suffix) {
    size_t score = 0;
    while (score < prefix.size() && score < c.start &&
           text[c.start - 1 - score] == prefix[prefix.size() - 1 - score])
        ++score;
    size_t after = 0;
    while (after < suffix.size() && c.end + after < text.size() && text[c.end + after] == suffix[after])
        ++after;
    return score + after;
}

struct Choice {
    Candidate candidate;
    bool ambiguous;
};

// Best context wins; ties go to the candidate nearest the stored position.
Choice choose(const std::vector<Candidate>& cands, const std::string& text, const std::string& prefix,
              const std::string& suffix, size_t near) {
    size_t bestScore = 0;
    std::vector<size_t> tied;
    for (size_t i = 0; i < cands.size(); ++i) {
        const size_t s = contextScore(text, cands[i], prefix, suffix);
        if (tied.empty() || s > bestScore) {
            bestScore = s;
            tied.assign(1, i);
        } else if (s == bestScore) {
            tied.push_back(i);
        }
    }
    size_t pick = tied[0];
    for (size_t i : tied)
        if (absDiff(cands[i].start, near) < absDiff(cands[pick].start, near)) pick = i;
    return {cands[pick], tied.size() > 1};
}

// Start of the best approximate match of pat that ends exactly at `end`.
size_t anchoredStart(const std::string& text, size_t end, const std::string& pat, int maxDist) {
    const size_t m = pat.size();
    const size_t w = std::min(end, m + static_cast<size_t>(maxDist));
    // Edit distance of reversed pat against text read backwards from end.
    std::vector<int> prev(w + 1), cur(w + 1);
    for (size_t j = 0; j <= w; ++j) prev[j] = static_cast<int>(j);
    for (size_t i = 1; i <= m; ++i) {
        cur[0] = static_cast<int>(i);
        const char pc = pat[m - i];
        for (size_t j = 1; j <= w; ++j) {
            const int sub = prev[j - 1] + (pc != text[end - j]);
            cur[j] = std::min({sub, prev[j] + 1, cur[j - 1] + 1});
        }
        std::swap(prev, cur);
    }
    size_t best = 0;
    for (size_t j = 1; j <= w; ++j)
        if (prev[j] < prev[best] || (prev[j] == prev[best] && absDiff(j, m) < absDiff(best, m))) best = j;
    return end - best;
}

// Approximate occurrences of pat in text with at most maxDist edits (Sellers' algorithm).
// Only occurrences at the best distance found are returned.
std::vector<Candidate> approxFind(const std::string& text, const std::string& pat, int maxDist) {
    const size_t m = pat.size();
    std::vector<int> prev(m + 1), cur(m + 1);
    for (size_t i = 0; i <= m; ++i) prev[i] = static_cast<int>(i);
    int best = maxDist + 1;
    std::vector<size_t> ends;
    for (size_t j = 1; j <= text.size(); ++j) {
        cur[0] = 0;
        const char tc = text[j - 1];
        for (size_t i = 1; i <= m; ++i) {
            const int sub = prev[i - 1] + (pat[i - 1] != tc);
            cur[i] = std::min({sub, prev[i] + 1, cur[i - 1] + 1});
        }
        const int d = cur[m];
        if (d < best) {
            best = d;
            ends.clear();
        }
        // Ends close together belong to the same occurrence; keep the first.
        if (d == best && (ends.empty() || j - ends.back() > m / 2)) ends.push_back(j);
        std::swap(prev, cur);
    }
    std::vector<Candidate> out;
    if (best > maxDist) return out;
    for (size_t e : ends) out.push_back({anchoredStart(text, e, pat, maxDist), e});
    return out;
}

int maxEdits(size_t length) { return static_cast<int>(length * kMaxErrorRate); }

// Approximate match in normalized space, or false.
bool fuzzyMatch(const NormalizedText& n, const std::string& key, const std::string& prefix,
                const std::string& suffix, size_t near, Choice& out) {
    const size_t m = key.size();
    if (m < kMinFuzzyLength) return false;
    if (m <= kFullPatternLimit) {
        const auto cands = approxFind(n.text, key, maxEdits(m));
        if (cands.empty()) return false;
        out = choose(cands, n.text, prefix, suffix, near);
        return true;
    }
    // Long ranges: both boundaries must survive; the middle may change freely.
    const auto heads = approxFind(n.text, key.substr(0, kPieceLength), maxEdits(kPieceLength));
    const auto tails = approxFind(n.text, key.substr(m - kPieceLength), maxEdits(kPieceLength));
    bool found = false;
    size_t bestCost = 0;
    for (const Candidate& h : heads)
        for (const Candidate& t : tails) {
            if (t.end <= h.start) continue;
            const size_t span = t.end - h.start;
            if (span * 2 < m || span > m * 2) continue;
            const size_t cost = absDiff(span, m) * 4 + absDiff(h.start, near);
            if (!found || cost < bestCost) {
                found = true;
                bestCost = cost;
                out = {{h.start, t.end}, false};
            }
        }
    return found;
}

void anchorRangeIn(Range& r, const std::string& doc, const NormalizedText& n) {
    if (r.exact.empty()) {
        r.status = AnchorStatus::Orphaned;
        return;
    }
    if (r.end <= doc.size() && r.end - r.start == r.exact.size() &&
        doc.compare(r.start, r.exact.size(), r.exact) == 0) {
        r.status = AnchorStatus::Exact;
        return;
    }
    if (auto cands = findAll(doc, r.exact); !cands.empty()) {
        const Choice c = choose(cands, doc, r.prefix, r.suffix, r.start);
        r.start = c.candidate.start;
        r.end = c.candidate.end;
        r.status = c.ambiguous ? AnchorStatus::Ambiguous : AnchorStatus::Moved;
        return;
    }

    const std::string key = normalizedKey(r.exact);
    const std::string prefix = normalize(r.prefix).text;
    const std::string suffix = normalize(r.suffix).text;
    const size_t near = n.fromOriginal(r.start);
    Choice c{};
    AnchorStatus status;
    if (auto cands = findAll(n.text, key); !cands.empty()) {
        c = choose(cands, n.text, prefix, suffix, near);
        status = c.ambiguous ? AnchorStatus::Ambiguous : AnchorStatus::Normalized;
    } else if (fuzzyMatch(n, key, prefix, suffix, near, c)) {
        status = c.ambiguous ? AnchorStatus::Ambiguous : AnchorStatus::Fuzzy;
    } else {
        r.status = AnchorStatus::Orphaned;
        r.start = r.end = std::min(r.start, doc.size());
        return;
    }
    // Approximate matches can start or end on a collapsed space; trim it.
    size_t ns = c.candidate.start, ne = c.candidate.end;
    while (ns < ne && n.text[ns] == ' ') ++ns;
    while (ne > ns && n.text[ne - 1] == ' ') --ne;
    if (ns == ne) {
        r.status = AnchorStatus::Orphaned;
        return;
    }
    r.start = n.origStart[ns];
    r.end = n.origEnd[ne - 1];
    r.status = status;
}

}  // namespace

bool locateApproximately(const NormalizedText& n, const std::string& key, size_t near, size_t& start,
                         size_t& end) {
    Choice c{};
    if (!fuzzyMatch(n, key, std::string(), std::string(), near, c)) return false;
    start = c.candidate.start;
    end = c.candidate.end;
    return true;
}

int AnchorSummary::total() const {
    int t = 0;
    for (int c : counts) t += c;
    return t;
}

void anchorRange(Range& range, const std::string& doc) {
    anchorRangeIn(range, doc, normalize(doc));
}

AnchorSummary anchorIndex(Index& index, const std::string& doc) {
    AnchorSummary summary;
    summary.hashMatched = !index.documentHash.empty() && index.documentHash == "sha256:" + sha256Hex(doc);
    const NormalizedText n = summary.hashMatched ? NormalizedText{} : normalize(doc);
    index.forEachRange([&](Range& r) {
        if (summary.hashMatched) {
            if (r.end > doc.size()) r.status = AnchorStatus::Orphaned;
        } else {
            anchorRangeIn(r, doc, n);
        }
        if (r.status == AnchorStatus::Orphaned) r.start = r.end = std::min(r.start, doc.size());
        ++summary.counts[static_cast<int>(r.status)];
    });
    return summary;
}

void refreshFromDocument(Index& index, const std::string& doc) {
    std::vector<std::pair<size_t, size_t>> covered;
    index.forEachRange([&](Range& r) {
        if (r.status == AnchorStatus::Orphaned) return;  // keep the lost text for review
        r.end = std::min(r.end, doc.size());
        r.start = std::min(r.start, r.end);
        if (r.start == r.end) {
            r.status = AnchorStatus::Orphaned;
            return;
        }
        r.exact = doc.substr(r.start, r.end - r.start);
        r.prefix = contextBefore(doc, r.start);
        r.suffix = contextAfter(doc, r.end);
        if (!needsReview(r.status)) r.status = AnchorStatus::Exact;
        covered.emplace_back(r.start, r.end);
    });
    index.unindexed = findUnindexed(doc, std::move(covered));
    index.documentHash = "sha256:" + sha256Hex(doc);
}

}  // namespace pe
