"""Turns the ranges an LLM wrote into exact spans of the document.

LLMs quote well but not perfectly. This module accepts three range forms and
forgives the usual slips: line-number prefixes copied into a quote, curly vs
straight quotes, collapsed whitespace, a quote shortened with "...", a word
or two changed, and boundaries that cut a word in half.

    {"lines": "12-14"}                       whole lines (list markers left out)
    {"quote": "text", "line": 12}            a passage; "line" is an optional hint
    {"from": "start", "to": "end", "line": 12}
    any form may add "tier": 1-5 (how load-bearing the passage is for its topic)

Each resolved span carries a status: "exact", "normalized" or "fuzzy"
(approximate: verify it), plus "ambiguous" when the text occurs more than
once and no hint picked one.
"""

import difflib
import re

FUZZY_MIN_RATIO = 0.8
FUZZY_MIN_LENGTH = 12

_FOLD = {
    "‘": "'", "’": "'", "‚": "'", "‛": "'", "′": "'",
    "“": '"', "”": '"', "„": '"', "‟": '"', "″": '"',
    "‐": "-", "‑": "-", "‒": "-", "–": "-", "—": "-",
    "―": "-", "−": "-", "…": "...",
}
_LINE_PREFIX = re.compile(r"^\s*(?:L?\d+\s*[|:]\s?)", re.MULTILINE)
_LIST_MARKER = re.compile(r"[ \t]*(?:[-*+]|\d+[.)])[ \t]+")
_ELLIPSIS = re.compile(r"\s*(?:\.\.\.|…)\s*")
_STATUS_RANK = {"exact": 0, "normalized": 1, "ambiguous": 2, "fuzzy": 3}


class ResolveError(Exception):
    pass


def worse(a, b):
    return a if _STATUS_RANK[a] >= _STATUS_RANK[b] else b


class Normalized:
    """Text with typographic variants folded and whitespace runs collapsed to one
    space; orig[i] is the original offset of normalized character i."""

    def __init__(self, text):
        out, orig, orig_end = [], [], []
        i, n = 0, len(text)
        while i < n:
            ch = text[i]
            if ch.isspace():
                j = i
                while j < n and text[j].isspace():
                    j += 1
                out.append(" ")
                orig.append(i)
                orig_end.append(j)
                i = j
                continue
            for c in _FOLD.get(ch, ch):
                out.append(c)
                orig.append(i)
                orig_end.append(i + 1)
            i += 1
        self.text = "".join(out)
        self.orig = orig
        self.orig_end = orig_end

    def span(self, start, end):
        """Original span of normalized [start, end), trimmed of collapsed spaces."""
        while start < end and self.text[start] == " ":
            start += 1
        while end > start and self.text[end - 1] == " ":
            end -= 1
        if start == end:
            return None
        return self.orig[start], self.orig_end[end - 1]


def normalize_key(s):
    return Normalized(s).text.strip()


class Document:
    def __init__(self, text):
        self.text = text
        self.line_starts = [0] + [m.end() for m in re.finditer("\n", text)]
        self.norm = Normalized(text)

    @property
    def line_count(self):
        return len(self.line_starts)

    def line_span(self, n):
        """[start, end) of 1-based line n, without its line ending."""
        start = self.line_starts[n - 1]
        end = self.line_starts[n] - 1 if n < self.line_count else len(self.text)
        if end > start and self.text[end - 1] == "\r":
            end -= 1
        return start, end

    def line_of(self, offset):
        lo, hi = 0, len(self.line_starts) - 1
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if self.line_starts[mid] <= offset:
                lo = mid
            else:
                hi = mid - 1
        return lo + 1

    def numbered(self):
        """The document with right-aligned line numbers, for prompts."""
        width = len(str(self.line_count))
        return "\n".join(f"{i + 1:>{width}}| {self.text[a:b]}"
                         for i, (a, b) in enumerate(self.line_span(k + 1) for k in range(self.line_count)))


def clean_quote(s):
    """Strips what LLMs add around quotes: line-number prefixes, wrapping quote
    marks or backticks, surrounding whitespace."""
    s = _LINE_PREFIX.sub("", s).strip()
    for a, b in (('"', '"'), ("'", "'"), ("`", "`"), ("“", "”")):
        if len(s) > 2 and s.startswith(a) and s.endswith(b) and s.count(a) == 1 + (a == b):
            s = s[1:-1].strip()
    return s


def _pick(doc, starts, hint_line):
    """Chooses among occurrences: nearest to the hint, else the first (ambiguous)."""
    if len(starts) == 1:
        return starts[0], False
    if hint_line:
        return min(starts, key=lambda s: abs(doc.line_of(s) - hint_line)), False
    return starts[0], True


def _find_all(text, needle, start=0):
    out, i = [], text.find(needle, start)
    while i != -1 and needle:
        out.append(i)
        i = text.find(needle, i + 1)
    return out


def _snap_words(doc, start, end):
    """Extends a span that cuts a word in half to the whole word."""
    t = doc.text
    while start > 0 and t[start - 1].isalnum() and t[start].isalnum():
        start -= 1
    while end < len(t) and t[end - 1].isalnum() and t[end].isalnum():
        end += 1
    return start, end


def _fuzzy(doc, key, hint_line):
    """Best approximate match of normalized key. Candidate windows start at each
    line and each sentence; the match is trimmed to where the key aligns."""
    if len(key) < FUZZY_MIN_LENGTH:
        return None
    norm = doc.norm
    starts = sorted({0} | {_norm_pos(norm, s) for s in doc.line_starts}
                    | {m.end() for m in re.finditer(r"[.!?:;] ", norm.text)})
    best = None  # (score, nstart, nend)
    for nstart in starts:
        window = norm.text[nstart:nstart + int(len(key) * 1.25) + 8]
        sm = difflib.SequenceMatcher(None, key, window, autojunk=False)
        # Cheap upper bound on the share of the key that can match; the window is
        # padded, so the plain ratio against it would understate a good match.
        if sm.quick_ratio() * (len(key) + len(window)) / (2 * len(key)) < FUZZY_MIN_RATIO:
            continue
        blocks = [b for b in sm.get_matching_blocks() if b.size]
        if not blocks:
            continue
        # Where the key's first and last characters would fall in the window.
        a0 = max(0, blocks[0].b - blocks[0].a)
        a1 = min(len(window), blocks[-1].b + len(key) - blocks[-1].a)
        if a1 <= a0:
            continue
        score = difflib.SequenceMatcher(None, key, window[a0:a1], autojunk=False).ratio()
        if hint_line:
            line = doc.line_of(norm.orig[min(nstart, len(norm.orig) - 1)])
            score -= min(abs(line - hint_line), 50) * 0.0005  # a nudge, never decisive
        if score >= FUZZY_MIN_RATIO and (best is None or score > best[0]):
            best = (score, nstart + a0, nstart + a1)
    if best is None:
        return None
    return norm.span(best[1], best[2])


def _norm_pos(norm, offset):
    lo, hi = 0, len(norm.orig)
    while lo < hi:
        mid = (lo + hi) // 2
        if norm.orig[mid] < offset:
            lo = mid + 1
        else:
            hi = mid
    return lo


def find_text(doc, quote, hint_line=None, after=0):
    """Locates one quote. Returns (start, end, status)."""
    q = clean_quote(quote)
    if not q:
        raise ResolveError("empty quote")
    starts = [s for s in _find_all(doc.text, q) if s >= after]
    if starts:
        s, ambiguous = _pick(doc, starts, hint_line)
        return s, s + len(q), "ambiguous" if ambiguous else "exact"
    key = normalize_key(q)
    nafter = _norm_pos(doc.norm, after)
    nstarts = [s for s in _find_all(doc.norm.text, key) if s >= nafter]
    if nstarts:
        orig = [doc.norm.orig[s] for s in nstarts]
        s, ambiguous = _pick(doc, orig, hint_line)
        ns = nstarts[orig.index(s)]
        span = doc.norm.span(ns, ns + len(key))
        if span:
            return span[0], span[1], "ambiguous" if ambiguous else "normalized"
    span = _fuzzy(doc, key, hint_line)
    if span and span[0] >= after:
        return span[0], span[1], "fuzzy"
    raise ResolveError(f"not found in the document: {q[:80]!r}")


def _parse_lines(value, doc):
    m = re.fullmatch(r"\s*L?(\d+)\s*(?:(?:-|–|to|\.\.)\s*L?(\d+))?\s*", str(value))
    if isinstance(value, (list, tuple)) and value:
        a, b = int(value[0]), int(value[-1])
    elif m:
        a, b = int(m.group(1)), int(m.group(2) or m.group(1))
    else:
        raise ResolveError(f"bad line range: {value!r}")
    if a > b:
        a, b = b, a
    if not 1 <= a <= b <= doc.line_count:
        raise ResolveError(f"lines {a}-{b} outside the document (1-{doc.line_count})")
    return a, b


def tier_of(item):
    """The range's "tier", 1 (core) to 5 (incidental); 0 when missing or invalid."""
    t = item.get("tier") if isinstance(item, dict) else None
    if isinstance(t, str) and t.isdigit():
        t = int(t)
    return t if isinstance(t, int) and 1 <= t <= 5 else 0


def note_of(item):
    """The range's "note": what the model's findings said about the passage."""
    n = item.get("note") if isinstance(item, dict) else None
    return n.strip() if isinstance(n, str) else ""


def resolve_range(doc, item):
    """Resolves one range item. Returns (start, end, status, tier, note); offsets are characters."""
    start, end, status = _resolve_untiered(doc, item)
    return start, end, status, tier_of(item), note_of(item)


def _resolve_untiered(doc, item):
    if not isinstance(item, dict):
        raise ResolveError(f"not a range object: {item!r}")
    hint = item.get("line")
    hint = int(hint) if isinstance(hint, (int, float)) or (isinstance(hint, str) and hint.isdigit()) else None

    if "lines" in item:
        a, b = _parse_lines(item["lines"], doc)
        start, _ = doc.line_span(a)
        _, end = doc.line_span(b)
        m = _LIST_MARKER.match(doc.text, start, end)
        if m:
            start = m.end()
        while start < end and doc.text[start].isspace():
            start += 1
        if start >= end:
            raise ResolveError(f"lines {a}-{b} are blank")
        return start, end, "exact"

    if "quote" in item:
        q = clean_quote(str(item["quote"]))
        parts = [p for p in _ELLIPSIS.split(q) if p.strip()]
        literal = q in doc.text or normalize_key(q) in doc.norm.text
        if len(parts) >= 2 and not literal:
            # An abbreviated quote: treat its first and last parts as from/to.
            return _resolve_untiered(doc, {"from": parts[0], "to": parts[-1], "line": hint})
        start, end, status = find_text(doc, q, hint)
        start, end = _snap_words(doc, start, end)
        return start, end, status

    if "from" in item and "to" in item:
        s, e1, st1 = find_text(doc, str(item["from"]), hint)
        to = clean_quote(str(item["to"]))
        # The end must follow the start; search from there, nearest first.
        s2, e2, st2 = find_text(doc, to, doc.line_of(s), after=s)
        if e2 < e1:
            raise ResolveError(f"'to' ends before 'from': {to[:60]!r}")
        start, end = _snap_words(doc, s, max(e1, e2))
        return start, end, worse(st1, st2)

    raise ResolveError(f"range needs \"lines\", \"quote\", or \"from\" and \"to\": {item!r}")


def merge_spans(spans, text):
    """Sorts spans and merges overlapping ones, or ones separated only by whitespace;
    a merged span keeps the most load-bearing (lowest) tier and the notes of all its
    parts. spans: (start, end, status, tier, note)."""
    out = []
    for s, e, st, tier, note in sorted(spans):
        if out and (s <= out[-1][1] or not text[out[-1][1]:s].strip()):
            ps, pe, pst, ptier, pnote = out[-1]
            best = min(t for t in (ptier, tier) if t) if (ptier or tier) else 0
            if note and note not in pnote:
                pnote = f"{pnote}\n{note}" if pnote else note
            out[-1] = (ps, max(pe, e), worse(pst, st), best, pnote)
        else:
            out.append((s, e, st, tier, note))
    return out
