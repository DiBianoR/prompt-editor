#pragma once

#include <algorithm>
#include <filesystem>
#include <string>

#include "core/Pipeline.h"

// Per-user settings in %APPDATA%\PromptEditor\settings.json. The API key is
// encrypted with Windows DPAPI, so the file is useless on another account or
// machine. Only prompts that differ from the built-in defaults are stored.
struct AppSettings {
    std::string baseUrl;
    std::string model;
    std::string apiKey;
    int repairs = 2;
    int contextTokens = 262144;  // the model's context window
    pe::PromptSet promptOverrides;

    // Each reply, thinking included, may use up to 40% of the context window.
    int replyTokens() const { return std::max(4096, contextTokens / 5 * 2); }

    pe::PromptSet prompts() const;  // defaults with overrides applied
    bool serverConfigured() const { return !baseUrl.empty() && !model.empty(); }

    static std::filesystem::path folder();  // %APPDATA%\PromptEditor
    static AppSettings load();
    bool save() const;
};

// %LOCALAPPDATA%\PromptEditor\runs: one folder per indexing run, with the
// transcript of every model call and the run's progress. Kept as an archive.
std::filesystem::path runsFolder();
// %LOCALAPPDATA%\PromptEditor\documents: one folder per indexed document,
// "<name>-<index id>", holding the latest files of each topic under topics\.
std::filesystem::path documentsFolder();
