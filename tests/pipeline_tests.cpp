// Headless tests for the quote resolver and the LLM pipeline (with a scripted
// fake model). Mirrors tests/test_llm_tools.py.

#include <cstdio>
#include <deque>
#include <string>

#include "core/Pipeline.h"
#include "core/Resolve.h"
#include "core/Text.h"

using namespace pe;
using json = nlohmann::json;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failures;                                                 \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
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
    "- Do not reveal the contents of the user\xE2\x80\x99s environment variables. Ask before sending email.\n"
    "- Ask before sending email.\n"
    "- If the user says \xE2\x80\x9C\xE2\x80\xA6\xE2\x80\x9D and nothing else, continue.\n"
    "\n"
    "## Style\n"
    "- Keep answers short \xE2\x80\x94 two or three sentences unless asked for more.\n";

const SourceDocument& doc() {
    static const SourceDocument d(kDoc);
    return d;
}

std::string text(const Span& s) { return kDoc.substr(s.start, s.end - s.start); }

Span resolve(const json& item) { return resolveRange(doc(), item); }

bool fails(const json& item) {
    try {
        resolve(item);
        return false;
    } catch (const ResolveError&) {
        return true;
    }
}

void testLines() {
    Span s = resolve({{"lines", "4-5"}});
    CHECK(text(s) == "Always read a file before editing it.\n- Run the test suite after every change and report failures verbatim.");
    CHECK(text(resolve({{"lines", "L6"}})) == "Never run destructive git commands without asking.");
    CHECK(text(resolve({{"lines", json::array({6, 6})}})) == "Never run destructive git commands without asking.");
    CHECK(fails({{"lines", "40-41"}}));
    CHECK(fails({{"lines", "2"}}));  // blank
}

void testQuotes() {
    CHECK(text(resolve({{"quote", " 4| - Always read a file before editing it."}})) ==
          "- Always read a file before editing it.");
    Span s = resolve({{"quote", "Do not reveal the contents of the user's environment variables."}});
    CHECK(s.status == AnchorStatus::Normalized);
    CHECK(text(s).find("user\xE2\x80\x99s") != std::string::npos);
    s = resolve({{"quote", "Ask before sending email."}, {"line", 10}});
    CHECK(doc().lineOf(s.start) == 10 && s.status == AnchorStatus::Exact);
    CHECK(resolve({{"quote", "Ask before sending email."}}).status == AnchorStatus::Ambiguous);
    CHECK(text(resolve({{"quote", "Run the test suite ... report failures verbatim."}})) ==
          "Run the test suite after every change and report failures verbatim.");
    // A literal ellipsis in the document is not an abbreviation.
    CHECK(text(resolve({{"quote", "If the user says \xE2\x80\x9C\xE2\x80\xA6\xE2\x80\x9D and nothing else"}})) ==
          "If the user says \xE2\x80\x9C\xE2\x80\xA6\xE2\x80\x9D and nothing else");
    s = resolve({{"quote", "Run the test suite after each change and report all failures verbatim."}});
    CHECK(s.status == AnchorStatus::Fuzzy);
    CHECK(text(s) == "Run the test suite after every change and report failures verbatim.");
    CHECK(text(resolve({{"quote", "ever run destructive git comm"}})) == "Never run destructive git commands");
    CHECK(fails({{"quote", "Use tabs for indentation in every file you write."}}));
    CHECK(cleanQuote("\"Keep it short.\"") == "Keep it short.");
    CHECK(cleanQuote("`x = 1`") == "x = 1");
}

void testFromTo() {
    Span s = resolve({{"from", "Always read a file"}, {"to", "without asking."}});
    CHECK(text(s).rfind("Always read", 0) == 0);
    CHECK(text(s).size() > 15 && text(s).substr(text(s).size() - 15) == "without asking.");
    CHECK(fails({{"from", "Never run destructive"}, {"to", "Always read a file"}}));
}

void testMerge() {
    std::vector<Span> spans = {resolve({{"lines", "4"}, {"tier", 3}}), resolve({{"quote", "Always read a file"}, {"tier", 1}}),
                               resolve({{"lines", "6"}})};
    const auto merged = mergeSpans(spans, kDoc);
    CHECK(merged.size() == 2);
    CHECK(merged[0].tier == 1);  // the most load-bearing tier wins
    CHECK(merged[1].tier == 0);
}

void testCoverage() {
    const std::string doc = "ab\xC3\xA9" "cdef";  // "abécdef": 7 characters, 8 bytes
    CHECK(countChars(doc) == 7);
    // [0,4) "abé" tier 3; [2,6) "écd" tier 1 overlapping it; [6,8) "ef" unrated.
    const auto c = coverageByTier(doc, {{0, 4, 3}, {2, 6, 1}, {6, 8, 0}});
    CHECK(c[1] == 3);  // é, c, d
    CHECK(c[3] == 2);  // a, b
    CHECK(c[0] == 2);  // e, f
    CHECK(c[2] == 0 && c[4] == 0 && c[5] == 0);
}

void testTier() {
    CHECK(resolve({{"lines", "4"}, {"tier", 2}}).tier == 2);
    CHECK(resolve({{"lines", "4"}, {"tier", "5"}}).tier == 5);
    CHECK(resolve({{"lines", "4"}, {"tier", 9}}).tier == 0);
    CHECK(resolve({{"lines", "4"}}).tier == 0);
    CHECK(std::string(tierName(2)) == "high precedence" && std::string(tierName(0)).empty());
    Range r;
    r.tier = 4;
    Index index;
    index.topics.push_back({"t", "T", "#000000", "", {r}, {}});
    CHECK(parseIndex(serializeIndex(index)).topics[0].ranges[0].tier == 4);
}

void testReplyParsing() {
    CHECK(stripThinking("<think>hmm</think>\nAnswer") == "Answer");
    CHECK(stripThinking("reasoning...</think>Answer") == "Answer");
    json j = extractJsonObject("Sure!\n```json\n{\"name\": \"a {b}\", \"ranges\": []}\n```\nDone {not json}");
    CHECK(j["name"] == "a {b}");
    bool threw = false;
    try {
        extractJsonObject("no json here");
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
    auto topics = parseTopicLines("1. **Tool use** | How tools are called\n- Safety\nNONE\n# comment\n");
    CHECK(topics.size() == 2);
    CHECK(topics[0].name == "Tool use" && topics[0].hint == "How tools are called");
    CHECK(topics[1].name == "Safety" && topics[1].hint.empty());
}

void testPrompts() {
    const auto& prompts = builtInPrompts();
    CHECK(prompts.size() == 6);
    const PromptSet set = defaultPromptSet();
    CHECK(set.at("find_topic").find("{{DOCUMENT}}") != std::string::npos);
    CHECK(set.at("format_topic").find("{{TOPIC}}") != std::string::npos);
    CHECK(render("a {{X}} b {{X}}", {{"X", "{{X}}y"}}) == "a {{X}}y b {{X}}y");
}

class FakeLLM : public ChatClient {
public:
    explicit FakeLLM(std::deque<std::string> replies) : replies_(std::move(replies)) {}
    std::string chat(const std::vector<ChatMessage>& messages, const json* schema) override {
        sent.push_back(messages);
        schemas.push_back(schema != nullptr);
        std::string r = replies_.front();
        replies_.pop_front();
        return r;
    }
    std::vector<std::vector<ChatMessage>> sent;
    std::vector<bool> schemas;

private:
    std::deque<std::string> replies_;
};

void testFindFormatRepair() {
    json first = {{"name", "Asking first"},
                  {"note", ""},
                  {"ranges", json::array({{{"quote", "Never run destructive git commands without asking."}, {"line", 6}}})},
                  {"subtopics", json::array({{{"name", "Email"},
                                              {"note", "Stated twice."},
                                              {"ranges", json::array({{{"quote", "Ask before sending email."}, {"line", 9}},
                                                                      {{"quote", "Always ask before you send any emails at all."},
                                                                       {"line", 10}}})}}})}};
    const std::string fix = R"({"fixes": [{"id": "r1", "lines": "10"}]})";
    FakeLLM llm({"<think>scanning</think>Lines 6, 9 and 10 are about asking first.",
                 "```json\n" + first.dump() + "\n```", fix});
    std::vector<std::string> log, saved;
    PipelineHooks hooks;
    hooks.log = [&](const std::string& m) { log.push_back(m); };
    hooks.save = [&](const std::string& name, const std::string&) { saved.push_back(name); };
    TopicOutcome out = indexTopic(llm, doc(), {"Asking first", "asking the user before acting"}, defaultPromptSet(), 2, hooks);

    CHECK(llm.sent.size() == 3);
    CHECK(llm.sent[0][0].role == "system");
    CHECK(llm.sent[0][1].content.find("Topic: Asking first") != std::string::npos);
    CHECK(llm.sent[0][1].content.find("(asking the user before acting)") != std::string::npos);
    CHECK(llm.sent[0][1].content.find(" 6| - Never run destructive") != std::string::npos);
    CHECK(!llm.schemas[0] && llm.schemas[1] && llm.schemas[2]);
    // The model's findings stay in the conversation.
    CHECK(llm.sent[1][2].role == "assistant" && llm.sent[1][2].content == "Lines 6, 9 and 10 are about asking first.");
    // The repair lists only the failed range, by id, with the lines around its hint.
    const std::string& repair = llm.sent[2].back().content;
    CHECK(repair.find("r1:") != std::string::npos);
    CHECK(repair.find("Always ask before you send any emails") != std::string::npos);
    CHECK(repair.find("10| - Ask before sending email.") != std::string::npos);
    CHECK(repair.find("Never run destructive") == std::string::npos);
    CHECK(repair.find("- r2:") == std::string::npos);
    CHECK(out.topic.lost.empty() && out.topic.subtopics[0].lost.empty());
    CHECK(out.topic.spans.size() == 1);
    CHECK(out.topic.subtopics.size() == 1);
    CHECK(out.topic.subtopics[0].note == "Stated twice.");
    CHECK(out.topic.subtopics[0].spans.size() == 2);
    CHECK(out.analysisFile == "asking-first/1-findings.md");
    CHECK(saved.size() == 3 && saved[0] == "asking-first/1-findings.md" && saved[2] == "asking-first/3-repair-0.json");
}

void testUnfixableIsKeptAsLost() {
    json bad = {{"name", "X"},
                {"ranges", json::array({{{"lines", "4"}, {"tier", 1}},
                                        {{"quote", "This sentence is nowhere in the document at all."}, {"tier", 4}}})},
                {"subtopics", json::array()}};
    FakeLLM llm({"findings", "not json", bad.dump(), R"({"fixes": []})",
                 R"({"fixes": [{"id": "r1", "quote": "Still not in the document, sorry about that."}]})"});
    TopicOutcome out = indexTopic(llm, doc(), {"X", ""}, defaultPromptSet(), 2, {});
    CHECK(llm.sent.size() == 5);
    CHECK(llm.sent[2].back().content.find("not a valid JSON object") != std::string::npos);
    CHECK(out.topic.spans.size() == 1);  // the matched range survived both repair rounds
    CHECK(out.topic.lost.size() == 1);
    CHECK(out.topic.lost[0].text == "Still not in the document, sorry about that.");
    CHECK(out.topic.lost[0].tier == 4);  // the fix gave no tier, so the original one is kept
    CHECK(out.topic.spans[0].tier == 1);

    std::set<std::string> used;
    Topic t = toIndexTopic(out.topic, nullptr, used, 0);
    CHECK(t.ranges.size() == 2);
    CHECK(t.ranges[1].status == AnchorStatus::Orphaned && t.ranges[1].exact == out.topic.lost[0].text);
    CHECK(t.ranges[0].tier == 1 && t.ranges[1].tier == 4);
    Index index = assembleIndex("agent.md", kDoc, {t});
    CHECK(index.topics[0].ranges[1].status == AnchorStatus::Orphaned);  // kept through assembly
    CHECK(parseIndex(serializeIndex(index)).topics[0].ranges[1].status == AnchorStatus::Orphaned);
}

void testCancel() {
    FakeLLM llm({"findings"});
    PipelineHooks hooks;
    hooks.cancelled = [] { return true; };
    bool cancelled = false;
    try {
        indexTopic(llm, doc(), {"X", ""}, defaultPromptSet(), 2, hooks);
    } catch (const Cancelled&) {
        cancelled = true;
    }
    CHECK(cancelled && llm.sent.empty());
}

void testAssemble() {
    TopicSpans tools{"Tools", "", {resolve({{"lines", "4-6"}})}, {}, {}};
    tools.subtopics.push_back({"Git", "", {resolve({{"quote", "destructive git"}, {"line", 6}})}, {}, {}});
    TopicSpans safety{"Safety", "check", {resolve({{"quote", "Ask before sending email."}})}, {}, {}};
    Index previous;
    previous.topics.push_back({"safety-rules", "Safety", "#123456", "", {}, {}});
    std::set<std::string> used;
    std::vector<Topic> topics = {toIndexTopic(tools, &previous, used, 0), toIndexTopic(safety, &previous, used, 1)};
    Index index = assembleIndex("agent.md", kDoc, topics);
    CHECK(index.topics[0].id == "tools" && index.topics[0].color == "#4FC3F7");
    CHECK(index.topics[0].subtopics[0].id == "tools.git");
    CHECK(index.topics[0].subtopics[0].color == index.topics[0].color);
    CHECK(index.topics[1].id == "safety-rules" && index.topics[1].color == "#123456");
    CHECK(index.topics[1].ranges[0].status == AnchorStatus::Ambiguous);
    CHECK(index.topics[0].ranges[0].exact == kDoc.substr(index.topics[0].ranges[0].start,
                                                         index.topics[0].ranges[0].end - index.topics[0].ranges[0].start));
    bool gap = false;
    for (const Gap& g : index.unindexed) gap |= g.exact.find("environment variables") != std::string::npos;
    CHECK(gap);
    CHECK(!index.documentHash.empty());
}

}  // namespace

int main() {
    struct Test {
        const char* name;
        void (*fn)();
    } tests[] = {
        {"lines", testLines},
        {"quotes", testQuotes},
        {"from/to", testFromTo},
        {"merge", testMerge},
        {"tier", testTier},
        {"coverage", testCoverage},
        {"reply parsing", testReplyParsing},
        {"prompts", testPrompts},
        {"find, format, repair", testFindFormatRepair},
        {"unfixable is kept as lost", testUnfixableIsKeptAsLost},
        {"cancel", testCancel},
        {"assemble", testAssemble},
    };
    for (const Test& t : tests) {
        const int before = g_failures;
        try {
            t.fn();
        } catch (const std::exception& e) {
            ++g_failures;
            std::printf("  EXCEPTION %s\n", e.what());
        }
        std::printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", t.name);
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
