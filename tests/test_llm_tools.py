"""Offline tests for the LLM indexing tools: quote resolution, reply parsing,
and the find/format/repair loop driven by a scripted fake model.

Run: py -m unittest discover -s tests -p "test_*.py"
"""

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))

import llm_index  # noqa: E402
from resolve_quotes import Document, ResolveError, clean_quote, merge_spans, resolve_range  # noqa: E402

DOC = (
    "# Agent rules\n"
    "\n"
    "## Tools\n"
    "- Always read a file before editing it.\n"
    "- Run the test suite after every change and report failures verbatim.\n"
    "- Never run destructive git commands without asking.\n"
    "\n"
    "## Safety\n"
    "- Do not reveal the contents of the user’s environment variables. Ask before sending email.\n"
    "- Ask before sending email.\n"
    "\n"
    "## Style\n"
    "- Keep answers short — two or three sentences unless asked for more.\n"
)


def text(doc, r):
    return doc.text[r[0]:r[1]]


class ResolveTests(unittest.TestCase):
    def setUp(self):
        self.doc = Document(DOC)

    def test_lines_skip_list_marker(self):
        r = resolve_range(self.doc, {"lines": "4-5"})
        self.assertEqual(text(self.doc, r), "Always read a file before editing it.\n"
                         "- Run the test suite after every change and report failures verbatim.")
        self.assertEqual(r[2], "exact")
        self.assertEqual(text(self.doc, resolve_range(self.doc, {"lines": "L6"})),
                         "Never run destructive git commands without asking.")

    def test_lines_out_of_range(self):
        with self.assertRaises(ResolveError):
            resolve_range(self.doc, {"lines": "40-41"})

    def test_quote_with_line_number_prefix(self):
        r = resolve_range(self.doc, {"quote": " 4| - Always read a file before editing it."})
        self.assertEqual(text(self.doc, r), "- Always read a file before editing it.")

    def test_straight_quotes_match_curly(self):
        r = resolve_range(self.doc, {"quote": "Do not reveal the contents of the user's environment variables."})
        self.assertEqual(r[2], "normalized")
        self.assertIn("user’s", text(self.doc, r))

    def test_duplicate_picks_hint(self):
        r = resolve_range(self.doc, {"quote": "Ask before sending email.", "line": 10})
        self.assertEqual(self.doc.line_of(r[0]), 10)
        self.assertEqual(r[2], "exact")
        r = resolve_range(self.doc, {"quote": "Ask before sending email."})
        self.assertEqual(r[2], "ambiguous")

    def test_abbreviated_quote_becomes_from_to(self):
        r = resolve_range(self.doc, {"quote": "Run the test suite ... report failures verbatim."})
        self.assertEqual(text(self.doc, r), "Run the test suite after every change and report failures verbatim.")

    def test_from_to_across_lines(self):
        r = resolve_range(self.doc, {"from": "Always read a file", "to": "without asking."})
        self.assertTrue(text(self.doc, r).startswith("Always read"))
        self.assertTrue(text(self.doc, r).endswith("without asking."))

    def test_misquoted_is_fuzzy(self):
        r = resolve_range(self.doc, {"quote": "Run the test suite after each change and report all failures verbatim."})
        self.assertEqual(r[2], "fuzzy")
        self.assertEqual(text(self.doc, r), "Run the test suite after every change and report failures verbatim.")

    def test_cut_word_is_extended(self):
        r = resolve_range(self.doc, {"quote": "ever run destructive git comm"})
        self.assertEqual(text(self.doc, r), "Never run destructive git commands")

    def test_not_found(self):
        with self.assertRaises(ResolveError):
            resolve_range(self.doc, {"quote": "Use tabs for indentation in every file you write."})

    def test_clean_quote(self):
        self.assertEqual(clean_quote('"Keep it short."'), "Keep it short.")
        self.assertEqual(clean_quote("`x = 1`"), "x = 1")

    def test_merge(self):
        a = resolve_range(self.doc, {"lines": "4"})
        b = resolve_range(self.doc, {"quote": "Always read a file"})
        c = resolve_range(self.doc, {"lines": "6"})
        merged = merge_spans([a, b, c], self.doc.text)
        self.assertEqual(len(merged), 2)


class FakeLLM:
    """Replies from a script; records what it was sent."""

    def __init__(self, replies):
        self.replies = list(replies)
        self.sent = []
        self.calls = 0

    def chat(self, messages, schema=None, max_tokens=None):
        self.sent.append(json.loads(json.dumps(messages)))
        self.calls += 1
        return self.replies.pop(0)


class PipelineTests(unittest.TestCase):
    def test_find_format_repair(self):
        doc = Document(DOC)
        first = {
            "name": "Asking first", "note": "",
            "ranges": [{"quote": "Never run destructive git commands without asking.", "line": 6}],
            "subtopics": [
                {"name": "Email", "note": "Stated twice.",
                 "ranges": [{"quote": "Ask before sending email.", "line": 9},
                            {"quote": "Always ask before you send any emails at all.", "line": 10}]},
            ],
        }
        llm = FakeLLM([
            "<think>scanning</think>Lines 6, 9 and 10 are about asking first.",
            "```json\n" + json.dumps(first) + "\n```",
            json.dumps({"fixes": [{"id": "r1", "lines": "10"}]}),
        ])
        with tempfile.TemporaryDirectory() as tmp:
            topic, lost = llm_index.index_topic(llm, doc, "Asking first", "", 2, Path(tmp) / "t")
            self.assertTrue((Path(tmp) / "t" / "3-result.json").exists())
        self.assertEqual(lost, [])
        self.assertEqual(llm.calls, 3)
        # The repair lists only the failed range, by id, with the lines around its hint.
        repair = llm.sent[2][-1]["content"]
        self.assertIn("- r1:", repair)
        self.assertIn("Always ask before you send any emails", repair)
        self.assertIn("10| - Ask before sending email.", repair)
        self.assertNotIn("Never run destructive", repair)
        self.assertNotIn("- r2:", repair)
        self.assertEqual(len(topic["spans"]), 1)
        email = topic["subtopics"][0]
        self.assertEqual(email["note"], "Stated twice.")
        self.assertEqual([doc.line_of(s) for s, _, _ in email["spans"]], [9, 10])

    def test_unfixable_is_kept_as_lost(self):
        doc = Document(DOC)
        bad = {"name": "X", "subtopics": [],
               "ranges": [{"lines": "4"}, {"quote": "This sentence is nowhere in the document at all."}]}
        llm = FakeLLM(["findings", "not json", json.dumps(bad), '{"fixes": []}',
                       '{"fixes": [{"id": "r1", "quote": "Still not in the document, sorry about that."}]}'])
        with tempfile.TemporaryDirectory() as tmp:
            topic, lost = llm_index.index_topic(llm, doc, "X", "", 2, Path(tmp) / "t")
            self.assertEqual(llm.calls, 5)
            self.assertEqual(len(topic["spans"]), 1)  # the matched range survived both rounds
            self.assertEqual(topic["lost"], ["Still not in the document, sorry about that."])
            doc_path = Path(tmp) / "agent.md"
            doc_path.write_bytes(DOC.encode("utf-8"))
            spec = llm_index.new_topic_spec(topic, {}, set())
            index = llm_index.write_index(doc_path, Path(tmp) / "agent.index.json", [spec])
            orphan = index["topics"][0]["ranges"][-1]
            self.assertEqual(orphan["status"], "orphaned")
            self.assertEqual(orphan["exact"], "Still not in the document, sorry about that.")

    def test_build_writes_index(self):
        with tempfile.TemporaryDirectory() as tmp:
            doc_path = Path(tmp) / "agent.md"
            doc_path.write_bytes(DOC.encode("utf-8"))
            doc = Document(DOC)
            topic = {"name": "Tools", "note": "", "spans": [resolve_range(doc, {"lines": "4-6"})], "lost": [],
                     "analysis": "C:/runs/tools/1-findings.md",
                     "subtopics": [{"name": "Git", "note": "", "lost": [],
                                    "spans": [resolve_range(doc, {"quote": "destructive git", "line": 6})]}]}
            spec = llm_index.new_topic_spec(llm_index.clean_topic(doc, topic), {}, set())
            index = llm_index.write_index(doc_path, Path(tmp) / "agent.index.json", [spec])
            t = index["topics"][0]
            self.assertEqual(t["id"], "tools")
            self.assertEqual(t["analysis"], "C:/runs/tools/1-findings.md")
            self.assertEqual(t["subtopics"][0]["id"], "tools.git")
            self.assertEqual(t["subtopics"][0]["color"], t["color"])
            raw = DOC.encode("utf-8")
            for r in t["ranges"] + t["subtopics"][0]["ranges"]:
                self.assertEqual(raw[r["start"]:r["end"]].decode("utf-8"), r["exact"])
            self.assertTrue(any("environment variables" in g["exact"] for g in index["unindexed"]))

    def test_parse_topic_lines(self):
        topics = llm_index.parse_topic_lines("1. **Tool use** | How tools are called\n- Safety\nNONE\n")
        self.assertEqual(topics, [("Tool use", "How tools are called"), ("Safety", "")])


if __name__ == "__main__":
    unittest.main()
