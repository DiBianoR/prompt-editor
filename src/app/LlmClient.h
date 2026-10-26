#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "core/Pipeline.h"

// OpenAI-compatible chat completions over WinHTTP. Asks for a JSON schema
// response format when the pipeline wants JSON, and falls back to plain JSON
// mode, then to none, if the server refuses it. abort() may be called from
// another thread to cut off a request in flight.
class LlmClient : public pe::ChatClient {
public:
    LlmClient(std::string baseUrl, std::string model, std::string apiKey);
    ~LlmClient() override;

    std::vector<std::string> listModels();
    pe::ChatReply chat(const std::vector<pe::ChatMessage>& messages, const pe::ChatOptions& options) override;
    void abort();

    void setTimeoutSeconds(int seconds) { timeoutSeconds_ = seconds; }
    // Upper limit on each reply, thinking included.
    void setMaxTokens(int tokens) { maxTokens_ = tokens; }
    // Each call is written there as NNN.json (request, reply, timing).
    void setTranscriptFolder(std::filesystem::path folder) { transcripts_ = std::move(folder); }
    int calls() const { return calls_; }

private:
    std::string request(const wchar_t* method, const std::string& path, const std::string& body, int& status);

    std::string baseUrl_;
    std::string model_;
    std::string apiKey_;
    int timeoutSeconds_ = 1800;
    int maxTokens_ = 16384;
    std::filesystem::path transcripts_;
    int calls_ = 0;
    int formatMode_ = 0;  // 0 json_schema, 1 json_object, 2 none

    std::mutex mutex_;
    void* current_ = nullptr;  // HINTERNET of the request in flight
    std::atomic<bool> aborted_{false};
};
