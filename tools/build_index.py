"""Build a topic index from a quote-based spec.

The spec is what a human or an LLM writes: topics and subtopics whose ranges
are given as quotes from the document. LLMs are good at quoting text and bad at
counting characters, so the spec never contains offsets. This tool resolves
each quote against the document and emits the full index the editor reads:
byte offsets, exact text, and prefix/suffix context for re-anchoring.

Spec range forms:
    {"quote": "exact text"}                    the range is the quote itself
    {"from": "start text", "to": "end text"}   from the start of `from` through
                                               the end of the first `to` after it
    either form may add "occurrence": n        (1-based) when the text repeats
    {"span": [start, end]}                     character offsets, for tools that
                                               already resolved the text
    any form may add "status"                  e.g. "fuzzy", carried into the index
    any form may add "tier"                    1 (core) to 5 (incidental), carried into the index
    {"lost": "text"}                           a range not found in the document,
                                               kept for the user to place

Usage:
    py tools/build_index.py <document> <spec.json> <out.index.json>
"""

import hashlib
import json
import re
import sys

CONTEXT_CHARS = 32
PALETTE = ["#4FC3F7", "#FFB74D", "#81C784", "#E57373", "#BA68C8",
           "#FFF176", "#4DB6AC", "#F06292", "#A1887F", "#90A4AE"]


class SpecError(Exception):
    pass


def find_all(text, needle):
    hits, i = [], text.find(needle)
    while i != -1:
        hits.append(i)
        i = text.find(needle, i + 1)
    return hits


def pick(hits, needle, occurrence, where):
    if not hits:
        raise SpecError(f"{where}: not found: {needle!r}")
    if occurrence is None:
        if len(hits) > 1:
            raise SpecError(f"{where}: {len(hits)} matches, add \"occurrence\": {needle!r}")
        return hits[0]
    if not 1 <= occurrence <= len(hits):
        raise SpecError(f"{where}: occurrence {occurrence} of {len(hits)}: {needle!r}")
    return hits[occurrence - 1]


def resolve(text, spec, where):
    """Returns (start, end) as character offsets into text."""
    if "span" in spec:
        start, end = spec["span"]
        if not 0 <= start < end <= len(text):
            raise SpecError(f"{where}: span out of range: {spec['span']}")
        return start, end
    occurrence = spec.get("occurrence")
    if "quote" in spec:
        start = pick(find_all(text, spec["quote"]), spec["quote"], occurrence, where)
        return start, start + len(spec["quote"])
    start = pick(find_all(text, spec["from"]), spec["from"], occurrence, where)
    end = text.find(spec["to"], start)
    if end == -1:
        raise SpecError(f"{where}: 'to' not found after 'from': {spec['to']!r}")
    return start, end + len(spec["to"])


class ByteOffsets:
    """Maps character offsets to UTF-8 byte offsets, which is what Scintilla uses."""

    def __init__(self, text):
        self.prefix_bytes = [0]
        for ch in text:
            self.prefix_bytes.append(self.prefix_bytes[-1] + len(ch.encode("utf-8")))

    def __getitem__(self, char_offset):
        return self.prefix_bytes[char_offset]


def build_range(text, offsets, spec, where, covered):
    if "lost" in spec:
        out = {"start": 0, "end": 0, "exact": spec["lost"], "prefix": "", "suffix": "",
               "touched": False, "status": "orphaned"}
        for key in ("tier", "note"):
            if spec.get(key):
                out[key] = spec[key]
        return out
    start, end = resolve(text, spec, where)
    covered.append((start, end))
    out = {
        "start": offsets[start],
        "end": offsets[end],
        "exact": text[start:end],
        "prefix": text[max(0, start - CONTEXT_CHARS):start],
        "suffix": text[end:end + CONTEXT_CHARS],
        "touched": False,
    }
    if spec.get("status"):
        out["status"] = spec["status"]
    for key in ("tier", "note"):
        if spec.get(key):
            out[key] = spec[key]
    return out


def build_node(text, offsets, spec, where, covered, color):
    node = {"id": spec["id"], "name": spec["name"], "color": spec.get("color", color)}
    for key in ("note", "analysis"):
        if key in spec:
            node[key] = spec[key]
    node["ranges"] = [build_range(text, offsets, r, f"{where} range {i + 1}", covered)
                      for i, r in enumerate(spec.get("ranges", []))]
    return node


STRUCTURE_LINE = re.compile(r"^(#+ .*|[-*+]|\d+[.)])$")


def unindexed_gaps(text, offsets, covered):
    """Text outside every range that is more than whitespace and markdown scaffolding
    (headings, bare bullets, list numbers). Matches findUnindexed in src/core/Text.cpp."""
    gaps, pos = [], 0
    for start, end in sorted(covered) + [(len(text), len(text))]:
        if start > pos:
            lines = []  # (start, end) of each non-blank line, trimmed
            for m in re.finditer(r"[^\n]+", text[pos:start]):
                a, b = pos + m.start(), pos + m.end()
                while a < b and text[a].isspace():
                    a += 1
                while b > a and text[b - 1].isspace():
                    b -= 1
                if b > a:
                    lines.append((a, b))
            while lines and STRUCTURE_LINE.match(text[lines[0][0]:lines[0][1]]):
                lines.pop(0)
            while lines and STRUCTURE_LINE.match(text[lines[-1][0]:lines[-1][1]]):
                lines.pop()
            if lines:
                a, b = lines[0][0], lines[-1][1]
                gaps.append({"start": offsets[a], "end": offsets[b], "exact": text[a:b]})
        pos = max(pos, end)
    return gaps


def build_index(doc_path, spec):
    with open(doc_path, "rb") as f:
        raw = f.read()
    text = raw.decode("utf-8")
    offsets = ByteOffsets(text)
    covered = []
    topics = []
    for i, tspec in enumerate(spec["topics"]):
        where = f"topic {tspec['id']}"
        topic = build_node(text, offsets, tspec, where, covered, PALETTE[i % len(PALETTE)])
        topic["subtopics"] = [
            build_node(text, offsets, sspec, f"{where}/{sspec['id']}", covered, topic["color"])
            for sspec in tspec.get("subtopics", [])
        ]
        topics.append(topic)
    return {
        "version": 1,
        "document": spec.get("document", doc_path),
        "documentHash": "sha256:" + hashlib.sha256(raw).hexdigest(),
        "offsetUnit": "utf8-byte",
        "topics": topics,
        "unindexed": unindexed_gaps(text, offsets, covered),
    }


def main(argv):
    if len(argv) != 4:
        print(__doc__)
        return 2
    _, doc_path, spec_path, out_path = argv
    with open(spec_path, encoding="utf-8") as f:
        spec = json.load(f)
    try:
        index = build_index(doc_path, spec)
    except SpecError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(index, f, ensure_ascii=False, indent=2)
        f.write("\n")
    n_ranges = sum(len(t["ranges"]) + sum(len(s["ranges"]) for s in t["subtopics"])
                   for t in index["topics"])
    print(f"{len(index['topics'])} topics, {n_ranges} ranges, "
          f"{len(index['unindexed'])} unindexed gaps -> {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
