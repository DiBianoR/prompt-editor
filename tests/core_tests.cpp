// Headless tests for the index, matcher and edit tracker.
// Usage: core_tests [fixture_dir]   (fixture_dir adds the local fixture checks)

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "core/Anchor.h"
#include "core/Index.h"
#include "core/Text.h"
#include "core/Tracker.h"

using namespace pe;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                    \
    } while (0)

const std::string kDoc =
    "# Agent rules\n"
    "\n"
    "## Tools\n"
    "- Always read a file before editing it.\n"
    "- Run the test suite after every change and report failures verbatim.\n"
    "- Never run destructive git commands without asking.\n"
    "\n"
    "## Safety\n"
    "- Do not reveal the contents of the user\xE2\x80\x99s environment variables.\n"
    "- Ask before sending email.\n"
    "- Ask before sending email.\n"
    "\n"
    "## Style\n"
    "- Keep answers short \xE2\x80\x94 two or three sentences unless asked for more.\n";

Range rangeFor(const std::string& doc, const std::string& quote, size_t from = 0) {
    Range r;
    r.start = doc.find(quote, from);
    r.end = r.start + quote.size();
    r.exact = quote;
    r.prefix = contextBefore(doc, r.start);
    r.suffix = contextAfter(doc, r.end);
    return r;
}

std::string replaced(std::string s, const std::string& from, const std::string& to) {
    const size_t i = s.find(from);
    if (i != std::string::npos) s.replace(i, from.size(), to);
    return s;
}

std::string text(const std::string& doc, const Range& r) { return doc.substr(r.start, r.end - r.start); }

std::string readFile(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void testUnchanged() {
    Range r = rangeFor(kDoc, "Always read a file before editing it.");
    anchorRange(r, kDoc);
    CHECK(r.status == AnchorStatus::Exact);
}

void testInsertedBefore() {
    Range r = rangeFor(kDoc, "Never run destructive git commands without asking.");
    const std::string doc = replaced(kDoc, "# Agent rules\n", "# Agent rules\nYou are a coding agent.\n");
    anchorRange(r, doc);
    CHECK(r.status == AnchorStatus::Moved);
    CHECK(text(doc, r) == r.exact);
}

void testParagraphMoved() {
    const std::string style = "## Style\n- Keep answers short \xE2\x80\x94 two or three sentences unless asked for more.\n";
    Range r = rangeFor(kDoc, "Keep answers short");
    r.end = kDoc.find(" unless", r.start);
    r.exact = text(kDoc, r);
    std::string doc = replaced(kDoc, style, "");
    doc = replaced(doc, "## Tools\n", style + "\n## Tools\n");
    anchorRange(r, doc);
    CHECK(r.status == AnchorStatus::Moved);
    CHECK(text(doc, r) == r.exact);
}

void testSmartQuotesFlattened() {
    Range r = rangeFor(kDoc, "Do not reveal the contents of the user\xE2\x80\x99s environment variables.");
    std::string doc = replaced(kDoc, "user\xE2\x80\x99s", "user's");
    doc = replaced(doc, "\xE2\x80\x94", "--");  // unrelated edit elsewhere
    anchorRange(r, doc);
    CHECK(r.status == AnchorStatus::Normalized);
    CHECK(text(doc, r) == "Do not reveal the contents of the user's environment variables.");
}

void testWhitespaceReflowed() {
    Range r = rangeFor(kDoc, "Run the test suite after every change and report failures verbatim.");
    const std::string doc = replaced(kDoc, "after every change and", "after every change\n  and");
    anchorRange(r, doc);
    CHECK(r.status == AnchorStatus::Normalized);
    CHECK(text(doc, r) == "Run the test suite after every change\n  and report failures verbatim.");
}

void testReworded() {
    Range r = rangeFor(kDoc, "Run the test suite after every change and report failures verbatim.");
    const std::string doc = replaced(kDoc, "after every change and report", "after each change, and report");
    anchorRange(r, doc);
    CHECK(r.status == AnchorStatus::Fuzzy);
    CHECK(text(doc, r) == "Run the test suite after each change, and report failures verbatim.");
}

void testLongRangeEditedInside() {
    std::string body;
    for (int i = 0; i < 12; ++i)
        body += "Rule " + std::to_string(i) + ": prefer small, reviewable commits with clear messages. ";
    const std::string doc = "# Long\n" + body + "\n# End\n";
    Range r = rangeFor(doc, body.substr(0, body.size() - 1));
    const std::string edited = replaced(doc, "Rule 5: prefer small", "Rule 5: always prefer very small");
    anchorRange(r, edited);
    CHECK(r.status == AnchorStatus::Fuzzy);
    CHECK(edited.compare(r.start, 7, "Rule 0:") == 0);
    CHECK(edited.compare(r.end - 9, 9, "messages.") == 0);
    CHECK(edited.find("Rule 11", r.start) < r.end);
}

void testDeleted() {
    Range r = rangeFor(kDoc, "Never run destructive git commands without asking.");
    const std::string doc = replaced(kDoc, "- Never run destructive git commands without asking.\n", "");
    anchorRange(r, doc);
    CHECK(r.status == AnchorStatus::Orphaned);
}

void testShortQuoteNotFuzzed() {
    Range r = rangeFor(kDoc, "Ask before");
    const std::string doc = replaced(replaced(kDoc, "Ask before", "Ask after"), "Ask before", "Ask after");
    anchorRange(r, doc);
    CHECK(r.status == AnchorStatus::Orphaned);
}

void testDuplicateResolvedByContext() {
    // The second "Ask before sending email." is followed by a blank line; the first is not.
    const size_t first = kDoc.find("Ask before sending email.");
    Range r = rangeFor(kDoc, "Ask before sending email.", first + 1);
    const std::string doc = replaced(kDoc, "## Safety\n", "## Safety\n- Be careful.\n");
    anchorRange(r, doc);
    CHECK(r.status == AnchorStatus::Moved);
    CHECK(r.start == doc.find("Ask before sending email.", doc.find("Ask before sending email.") + 1));
}

void testDuplicateWithoutContextIsAmbiguous() {
    Range r = rangeFor(kDoc, "Ask before sending email.");
    r.prefix = r.suffix = "";
    r.start += 3;  // stale offsets
    r.end += 3;
    anchorRange(r, kDoc);
    CHECK(r.status == AnchorStatus::Ambiguous);
    CHECK(r.start == kDoc.find("Ask before sending email."));
}

Index sampleIndex(const std::string& doc) {
    Index index;
    index.document = "agent.md";
    Topic tools{"tools", "Tools", "#4FC3F7", "", {rangeFor(doc, "Always read a file before editing it.")}, {}};
    tools.subtopics.push_back(
        {"tools.git", "Git", "#4FC3F7", "", {rangeFor(doc, "Never run destructive git commands without asking.")}, {}});
    Topic safety{"safety", "Safety", "#E57373", "check", {rangeFor(doc, "Ask before sending email.")}, {}};
    index.topics = {tools, safety};
    return index;
}

void testHashTrusted() {
    Index index = sampleIndex(kDoc);
    index.documentHash = "sha256:" + sha256Hex(kDoc);
    index.topics[0].ranges[0].exact = "this text is not in the document";
    const AnchorSummary s = anchorIndex(index, kDoc);
    CHECK(s.hashMatched);
    CHECK(s.count(AnchorStatus::Exact) == 3);
}

void testTracker() {
    Index index = sampleIndex(kDoc);
    Range& readRule = index.topics[0].ranges[0];
    const Range before = readRule;

    trackInsert(index, before.start, 4);  // at the start: shifts, untouched
    CHECK(readRule.start == before.start + 4 && readRule.end == before.end + 4 && !readRule.touched);
    trackDelete(index, before.start, 4);
    CHECK(readRule.start == before.start && readRule.end == before.end);

    trackInsert(index, before.end, 3);  // at the end: stays outside
    CHECK(readRule.end == before.end && !readRule.touched);
    trackDelete(index, before.end, 3);

    trackInsert(index, before.start + 7, 5);  // inside: grows, touched
    CHECK(readRule.end == before.end + 5 && readRule.touched);

    trackDelete(index, before.start - 2, 10);  // straddles the start
    CHECK(readRule.start == before.start - 2 && readRule.end == before.end - 5);

    Range& gitRule = index.topics[0].subtopics[0].ranges[0];
    trackDelete(index, gitRule.start - 1, gitRule.end - gitRule.start + 2);  // swallows it whole
    CHECK(gitRule.status == AnchorStatus::Orphaned && gitRule.start == gitRule.end);
}

void testRefreshAndUnindexed() {
    std::string doc = kDoc;
    Index index = sampleIndex(doc);
    index.forEachRange([](Range& r) { r.status = AnchorStatus::Moved; });
    refreshFromDocument(index, doc);
    CHECK(index.documentHash == "sha256:" + sha256Hex(doc));
    CHECK(index.topics[0].ranges[0].status == AnchorStatus::Exact);
    bool foundSafetyRule = false, foundHeading = false;
    for (const Gap& g : index.unindexed) {
        foundSafetyRule |= g.exact.find("environment variables") != std::string::npos;
        foundHeading |= g.exact == "## Tools";
    }
    CHECK(foundSafetyRule);
    CHECK(!foundHeading);  // markdown scaffolding alone is not flagged
}

void testRoundTrip() {
    Index index = sampleIndex(kDoc);
    index.topics[1].ranges[0].status = AnchorStatus::Fuzzy;
    index.topics[1].ranges[0].touched = true;
    refreshFromDocument(index, kDoc);
    const Index back = parseIndex(serializeIndex(index));
    CHECK(back.topics.size() == 2);
    CHECK(back.topics[0].subtopics.size() == 1);
    CHECK(back.topics[0].subtopics[0].color == "#4FC3F7");
    CHECK(back.topics[1].note == "check");
    CHECK(back.topics[1].ranges[0].status == AnchorStatus::Fuzzy);
    CHECK(back.topics[1].ranges[0].touched);
    CHECK(back.topics[0].ranges[0].exact == index.topics[0].ranges[0].exact);
    CHECK(back.documentHash == index.documentHash);
    CHECK(back.unindexed.size() == index.unindexed.size());
}

// Checks against the hand-made index for a real prompt, when present locally.
void testLocalFixture(const std::filesystem::path& dir) {
    const auto docPath = dir / "ani.md", indexPath = dir / "ani.index.json";
    if (!std::filesystem::exists(docPath) || !std::filesystem::exists(indexPath)) {
        std::printf("  (local fixture not found in %s, skipped)\n", dir.string().c_str());
        return;
    }
    const std::string doc = readFile(docPath);
    Index index = parseIndex(readFile(indexPath));
    AnchorSummary s = anchorIndex(index, doc);
    CHECK(s.hashMatched);
    CHECK(s.count(AnchorStatus::Exact) == s.total());

    // Same index against an edited copy: smart quotes flattened, a line added,
    // one rule reworded and one deleted.
    std::string edited = doc;
    for (const char* q : {"\xE2\x80\x99", "\xE2\x80\x9C", "\xE2\x80\x9D"})
        for (size_t i; (i = edited.find(q)) != std::string::npos;)
            edited.replace(i, 3, std::string(q)[2] == '\x99' ? "'" : "\"");
    edited = replaced(edited, "# Current state", "- Never mention competitors by name.\n\n# Current state");
    edited = replaced(edited, "You are very expressive and you let your emotions out.",
                      "You are very expressive and you always let your feelings out.");
    edited = replaced(edited, "- You can hide background.\n", "");
    Index again = parseIndex(readFile(indexPath));
    s = anchorIndex(again, edited);
    std::printf("  local fixture after edits: %d ranges, %d exact, %d moved, %d normalized, %d fuzzy, "
                "%d ambiguous, %d orphaned\n",
                s.total(), s.count(AnchorStatus::Exact), s.count(AnchorStatus::Moved),
                s.count(AnchorStatus::Normalized), s.count(AnchorStatus::Fuzzy),
                s.count(AnchorStatus::Ambiguous), s.count(AnchorStatus::Orphaned));
    again.forEachRange([&](Range& r) {
        if (r.status == AnchorStatus::Fuzzy)
            std::printf("    fuzzy: %.60s...\n", text(edited, r).c_str());
    });
    CHECK(!s.hashMatched);
    CHECK(s.count(AnchorStatus::Orphaned) == 0);
    CHECK(s.count(AnchorStatus::Ambiguous) == 0);
    CHECK(s.count(AnchorStatus::Fuzzy) >= 1);
    refreshFromDocument(again, edited);
    bool flaggedNewLine = false;
    for (const Gap& g : again.unindexed) flaggedNewLine |= g.exact == "- Never mention competitors by name.";
    CHECK(flaggedNewLine);
}

}  // namespace

int main(int argc, char** argv) {
    struct Test {
        const char* name;
        void (*fn)();
    } tests[] = {
        {"unchanged", testUnchanged},
        {"inserted before", testInsertedBefore},
        {"paragraph moved", testParagraphMoved},
        {"smart quotes flattened", testSmartQuotesFlattened},
        {"whitespace reflowed", testWhitespaceReflowed},
        {"reworded", testReworded},
        {"long range edited inside", testLongRangeEditedInside},
        {"deleted", testDeleted},
        {"short quote not fuzzed", testShortQuoteNotFuzzed},
        {"duplicate resolved by context", testDuplicateResolvedByContext},
        {"duplicate without context", testDuplicateWithoutContextIsAmbiguous},
        {"hash trusted", testHashTrusted},
        {"tracker", testTracker},
        {"refresh and unindexed", testRefreshAndUnindexed},
        {"round trip", testRoundTrip},
    };
    for (const Test& t : tests) {
        const int before = g_failures;
        t.fn();
        std::printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", t.name);
    }
    if (argc > 1) {
        const int before = g_failures;
        testLocalFixture(argv[1]);
        std::printf("%s local fixture\n", g_failures == before ? "ok  " : "FAIL");
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
