#include "Resolve.h"

#include <algorithm>
#include <cctype>
#include <regex>

#include "Anchor.h"

namespace pe {

namespace {

constexpr size_t kFuzzyMinLength = 12;

int rank(AnchorStatus s) {
    switch (s) {
        case AnchorStatus::Exact: return 0;
        case AnchorStatus::Moved: return 0;
        case AnchorStatus::Normalized: return 1;
        case AnchorStatus::Ambiguous: return 2;
        case AnchorStatus::Fuzzy: return 3;
        case AnchorStatus::Orphaned: return 4;
    }
    return 4;
}

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

// Bytes of multi-byte UTF-8 characters count as word characters, so a span is
// never extended or cut in the middle of one.
bool isWordByte(char c) { return static_cast<unsigned char>(c) >= 0x80 || std::isalnum(static_cast<unsigned char>(c)); }

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isSpace(s[a])) ++a;
    while (b > a && isSpace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

bool startsWith(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }
bool endsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

size_t countOf(const std::string& s, const std::string& p) {
    size_t n = 0;
    for (size_t i = s.find(p); i != std::string::npos; i = s.find(p, i + p.size())) ++n;
    return n;
}

std::vector<size_t> findAll(const std::string& text, const std::string& needle, size_t after) {
    std::vector<size_t> out;
    if (needle.empty()) return out;
    for (size_t i = text.find(needle, after); i != std::string::npos; i = text.find(needle, i + 1)) out.push_back(i);
    return out;
}

size_t absDiff(size_t a, size_t b) { return a > b ? a - b : b - a; }

// Chooses among occurrences: nearest the hint line, else the first (ambiguous).
size_t pick(const SourceDocument& doc, const std::vector<size_t>& starts, size_t hintLine, bool& ambiguous) {
    ambiguous = false;
    if (starts.size() == 1) return starts[0];
    if (hintLine) {
        return *std::min_element(starts.begin(), starts.end(), [&](size_t a, size_t b) {
            return absDiff(doc.lineOf(a), hintLine) < absDiff(doc.lineOf(b), hintLine);
        });
    }
    ambiguous = true;
    return starts[0];
}

// Normalized span [ns, ne) back to original offsets, trimmed of collapsed spaces.
bool originalSpan(const NormalizedText& n, size_t ns, size_t ne, size_t& start, size_t& end) {
    while (ns < ne && n.text[ns] == ' ') ++ns;
    while (ne > ns && n.text[ne - 1] == ' ') --ne;
    if (ns == ne) return false;
    start = n.origStart[ns];
    end = n.origEnd[ne - 1];
    return true;
}

void snapWords(const std::string& t, size_t& start, size_t& end) {
    while (start > 0 && start < t.size() && isWordByte(t[start - 1]) && isWordByte(t[start])) --start;
    while (end > 0 && end < t.size() && isWordByte(t[end - 1]) && isWordByte(t[end])) ++end;
}

Span findText(const SourceDocument& doc, const std::string& quote, size_t hintLine, size_t after = 0) {
    const std::string q = cleanQuote(quote);
    if (q.empty()) throw ResolveError("empty quote");
    bool ambiguous;
    if (auto starts = findAll(doc.text(), q, after); !starts.empty()) {
        const size_t s = pick(doc, starts, hintLine, ambiguous);
        return {s, s + q.size(), ambiguous ? AnchorStatus::Ambiguous : AnchorStatus::Exact};
    }
    const NormalizedText& n = doc.normalized();
    const std::string key = normalizedKey(q);
    const size_t nafter = n.fromOriginal(after);
    if (auto nstarts = findAll(n.text, key, nafter); !nstarts.empty()) {
        std::vector<size_t> orig;
        for (size_t s : nstarts) orig.push_back(n.origStart[s]);
        const size_t s = pick(doc, orig, hintLine, ambiguous);
        const size_t ns = nstarts[std::find(orig.begin(), orig.end(), s) - orig.begin()];
        Span span;
        if (originalSpan(n, ns, ns + key.size(), span.start, span.end)) {
            span.status = ambiguous ? AnchorStatus::Ambiguous : AnchorStatus::Normalized;
            return span;
        }
    }
    if (key.size() >= kFuzzyMinLength) {
        const size_t near = hintLine ? n.fromOriginal(doc.lineSpan(std::min(hintLine, doc.lineCount())).first) : nafter;
        size_t ns, ne;
        Span span;
        if (locateApproximately(n, key, near, ns, ne) && originalSpan(n, ns, ne, span.start, span.end) &&
            span.start >= after) {
            span.status = AnchorStatus::Fuzzy;
            return span;
        }
    }
    throw ResolveError("not found in the document: \"" + q.substr(0, 80) + "\"");
}

size_t hintOf(const nlohmann::json& item) {
    auto it = item.find("line");
    if (it == item.end()) return 0;
    if (it->is_number_integer() && it->get<long long>() > 0) return static_cast<size_t>(it->get<long long>());
    if (it->is_string()) {
        const std::string s = it->get<std::string>();
        if (!s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
            return std::stoul(s);
    }
    return 0;
}

std::pair<size_t, size_t> parseLines(const nlohmann::json& v, const SourceDocument& doc) {
    size_t a = 0, b = 0;
    if (v.is_array() && !v.empty() && v.front().is_number_integer() && v.back().is_number_integer()) {
        a = v.front().get<size_t>();
        b = v.back().get<size_t>();
    } else if (v.is_number_integer()) {
        a = b = v.get<size_t>();
    } else if (v.is_string()) {
        static const std::regex re(R"(^\s*L?(\d+)\s*(?:(?:-|\xE2\x80\x93|to|\.\.)\s*L?(\d+))?\s*$)");
        std::smatch m;
        const std::string s = v.get<std::string>();
        if (!std::regex_match(s, m, re)) throw ResolveError("bad line range: " + s);
        a = std::stoul(m[1].str());
        b = m[2].matched ? std::stoul(m[2].str()) : a;
    } else {
        throw ResolveError("bad line range: " + v.dump());
    }
    if (a > b) std::swap(a, b);
    if (a < 1 || b > doc.lineCount())
        throw ResolveError("lines " + std::to_string(a) + "-" + std::to_string(b) + " outside the document (1-" +
                           std::to_string(doc.lineCount()) + ")");
    return {a, b};
}

std::string jsonString(const nlohmann::json& v) { return v.is_string() ? v.get<std::string>() : v.dump(); }

}  // namespace

AnchorStatus worse(AnchorStatus a, AnchorStatus b) { return rank(a) >= rank(b) ? a : b; }

SourceDocument::SourceDocument(std::string text) : text_(std::move(text)), norm_(normalize(text_)) {
    lineStarts_.push_back(0);
    for (size_t i = 0; i < text_.size(); ++i)
        if (text_[i] == '\n') lineStarts_.push_back(i + 1);
}

std::pair<size_t, size_t> SourceDocument::lineSpan(size_t n) const {
    const size_t start = lineStarts_[n - 1];
    size_t end = n < lineStarts_.size() ? lineStarts_[n] - 1 : text_.size();
    if (end > start && text_[end - 1] == '\r') --end;
    return {start, end};
}

size_t SourceDocument::lineOf(size_t offset) const {
    return std::upper_bound(lineStarts_.begin(), lineStarts_.end(), offset) - lineStarts_.begin();
}

std::string SourceDocument::numbered(size_t first, size_t last) const {
    const size_t width = std::to_string(lineCount()).size();
    first = std::max<size_t>(first, 1);
    last = last == 0 ? lineCount() : std::min(last, lineCount());
    std::string out;
    for (size_t n = first; n <= last; ++n) {
        const std::string num = std::to_string(n);
        out.append(width - num.size(), ' ').append(num).append("| ");
        const auto [a, b] = lineSpan(n);
        out.append(text_, a, b - a);
        if (n < last) out.push_back('\n');
    }
    return out;
}

std::string cleanQuote(const std::string& s) {
    // Line-number prefixes such as " 12| " or "L12: " at the start of each line.
    static const std::regex prefix(R"(^\s*L?\d+\s*[|:]\s?)");
    std::string out;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t nl = s.find('\n', pos);
        if (nl == std::string::npos) nl = s.size();
        out += std::regex_replace(s.substr(pos, nl - pos), prefix, "", std::regex_constants::format_first_only);
        if (nl < s.size()) out.push_back('\n');
        pos = nl + 1;
    }
    out = trim(out);
    static const std::pair<const char*, const char*> wraps[] = {
        {"\"", "\""}, {"'", "'"}, {"`", "`"}, {"\xE2\x80\x9C", "\xE2\x80\x9D"}};
    for (const auto& [open, close] : wraps) {
        const std::string a = open, b = close;
        const size_t expected = a == b ? 2 : 1;
        if (out.size() > a.size() + b.size() && startsWith(out, a) && endsWith(out, b) && countOf(out, a) == expected)
            out = trim(out.substr(a.size(), out.size() - a.size() - b.size()));
    }
    return out;
}

namespace {

int tierOf(const nlohmann::json& item) {
    auto it = item.find("tier");
    if (it == item.end()) return 0;
    if (it->is_number_integer()) return clampTier(it->get<long long>());
    if (it->is_string()) {
        const std::string s = it->get<std::string>();
        if (s.size() == 1 && s[0] >= '1' && s[0] <= '5') return s[0] - '0';
    }
    return 0;
}

Span resolveRangeUntiered(const SourceDocument& doc, const nlohmann::json& item);

}  // namespace

Span resolveRange(const SourceDocument& doc, const nlohmann::json& item) {
    Span span = resolveRangeUntiered(doc, item);
    if (item.is_object()) {
        span.tier = tierOf(item);
        if (auto n = item.find("note"); n != item.end() && n->is_string()) span.note = trim(n->get<std::string>());
    }
    return span;
}

namespace {

Span resolveRangeUntiered(const SourceDocument& doc, const nlohmann::json& item) {
    if (!item.is_object()) throw ResolveError("not a range object: " + item.dump());
    const size_t hint = hintOf(item);
    const std::string& t = doc.text();

    if (auto it = item.find("lines"); it != item.end()) {
        const auto [a, b] = parseLines(*it, doc);
        size_t start = doc.lineSpan(a).first, end = doc.lineSpan(b).second;
        // Leave out a leading list marker: "- ", "* ", "+ ", "3. ", "3) ".
        size_t p = start;
        while (p < end && (t[p] == ' ' || t[p] == '\t')) ++p;
        size_t q = p;
        if (q < end && (t[q] == '-' || t[q] == '*' || t[q] == '+')) {
            ++q;
        } else {
            while (q < end && std::isdigit(static_cast<unsigned char>(t[q]))) ++q;
            if (q > p && q < end && (t[q] == '.' || t[q] == ')')) ++q; else q = p;
        }
        if (q > p && q < end && (t[q] == ' ' || t[q] == '\t')) start = q;
        while (start < end && isSpace(t[start])) ++start;
        if (start >= end) throw ResolveError("lines " + std::to_string(a) + "-" + std::to_string(b) + " are blank");
        return {start, end, AnchorStatus::Exact};
    }

    if (auto it = item.find("quote"); it != item.end()) {
        const std::string q = cleanQuote(jsonString(*it));
        // A quote shortened with "..." is treated as from/to on its two ends.
        static const std::regex ellipsis(R"(\s*(?:\.\.\.|\xE2\x80\xA6)\s*)");
        std::vector<std::string> parts;
        for (std::sregex_token_iterator i(q.begin(), q.end(), ellipsis, -1), e; i != e; ++i)
            if (!trim(*i).empty()) parts.push_back(*i);
        const bool literal = t.find(q) != std::string::npos ||
                             doc.normalized().text.find(normalizedKey(q)) != std::string::npos;
        if (parts.size() >= 2 && !literal) {
            nlohmann::json ft = {{"from", parts.front()}, {"to", parts.back()}};
            if (hint) ft["line"] = hint;
            return resolveRange(doc, ft);
        }
        Span span = findText(doc, q, hint);
        snapWords(t, span.start, span.end);
        return span;
    }

    auto from = item.find("from"), to = item.find("to");
    if (from != item.end() && to != item.end()) {
        const Span a = findText(doc, jsonString(*from), hint);
        const Span b = findText(doc, jsonString(*to), doc.lineOf(a.start), a.start);
        if (b.end < a.end) throw ResolveError("'to' ends before 'from': " + cleanQuote(jsonString(*to)).substr(0, 60));
        Span span{a.start, std::max(a.end, b.end), worse(a.status, b.status)};
        snapWords(t, span.start, span.end);
        return span;
    }
    throw ResolveError("range needs \"lines\", \"quote\", or \"from\" and \"to\": " + item.dump());
}

}  // namespace

std::vector<Span> mergeSpans(std::vector<Span> spans, const std::string& text) {
    std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) {
        return a.start != b.start ? a.start < b.start : a.end < b.end;
    });
    std::vector<Span> out;
    for (const Span& s : spans) {
        if (!out.empty()) {
            Span& last = out.back();
            const bool between = s.start <= last.end ||
                                 trim(text.substr(last.end, s.start - last.end)).empty();
            if (between) {
                last.end = std::max(last.end, s.end);
                last.status = worse(last.status, s.status);
                if (s.tier && (!last.tier || s.tier < last.tier)) last.tier = s.tier;
                if (!s.note.empty() && last.note.find(s.note) == std::string::npos)
                    last.note += (last.note.empty() ? "" : "\n") + s.note;
                continue;
            }
        }
        out.push_back(s);
    }
    return out;
}

}  // namespace pe
