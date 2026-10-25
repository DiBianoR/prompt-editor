#pragma once

#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "Index.h"
#include "Resolve.h"

namespace pe {

// The LLM indexing pipeline, independent of how the model is reached. Each
// topic is one conversation: the model first reports every passage on the
// topic in its own words, then converts its findings into ranges. Only the
// ranges that do not match the document go back to it, one by one, to fix;
// matched ranges are never resent. Whatever still fails is kept as lost.

struct ChatMessage {
    std::string role;
    std::string content;
};

class ChatClient {
public:
    virtual ~ChatClient() = default;
    // `schema`, when given, asks the server to hold the reply to that JSON schema
    // if it can. Throws on transport errors and on cancellation.
    virtual std::string chat(const std::vector<ChatMessage>& messages, const nlohmann::json* schema) = 0;
};

class Cancelled : public std::runtime_error {
public:
    Cancelled() : std::runtime_error("cancelled") {}
};

// ---- Prompts ----

struct PromptInfo {
    std::string id;
    std::string title;
    std::string placeholders;  // what {{...}} values the prompt receives
    std::string text;          // the built-in default
};
const std::vector<PromptInfo>& builtInPrompts();
using PromptSet = std::map<std::string, std::string>;  // id -> text
PromptSet defaultPromptSet();
std::string render(const std::string& tmpl, const std::map<std::string, std::string>& values);

// ---- Pipeline ----

struct TopicRequest {
    std::string name;
    std::string hint;  // what belongs under the topic, optional
};

// A range the model named that could not be matched to the document, even after
// repair. It is kept, as text, for the user to place or discard.
struct LostRange {
    std::string text;    // the model's quote, or "from ... to" ends
    std::string reason;  // why it did not match
};

struct TopicSpans {
    std::string name;
    std::string note;
    std::vector<Span> spans;
    std::vector<LostRange> lost;
    std::vector<TopicSpans> subtopics;
};

struct TopicOutcome {
    TopicSpans topic;
    std::string analysisFile;  // name passed to hooks.save for the step 1 findings
};

struct PipelineHooks {
    std::function<void(const std::string&)> log;
    std::function<bool()> cancelled;
    std::function<void(const std::string& name, const std::string& content)> save;  // transcripts
};

std::string stripThinking(const std::string& text);
// The first balanced JSON object in a reply; throws std::runtime_error.
nlohmann::json extractJsonObject(const std::string& text);
std::vector<TopicRequest> parseTopicLines(const std::string& text);

std::vector<TopicRequest> discoverTopics(ChatClient& llm, const SourceDocument& doc, const PromptSet& prompts,
                                         const Index* existing, const PipelineHooks& hooks);
TopicOutcome indexTopic(ChatClient& llm, const SourceDocument& doc, const TopicRequest& request,
                        const PromptSet& prompts, int repairs, const PipelineHooks& hooks);

// An index topic from pipeline output. Ids and colors of same-named topics in
// `previous` are kept; new topics take the next palette color.
Topic toIndexTopic(const TopicSpans& spans, const Index* previous, std::set<std::string>& usedIds,
                   size_t paletteIndex);
// Fills exact text, context, unindexed text and the hash for doc.
Index assembleIndex(const std::string& documentName, const std::string& doc, std::vector<Topic> topics);

const nlohmann::json& topicSchema();
const nlohmann::json& repairSchema();

}  // namespace pe
