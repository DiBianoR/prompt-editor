#pragma once

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
    pe::PromptSet promptOverrides;

    pe::PromptSet prompts() const;  // defaults with overrides applied
    bool serverConfigured() const { return !baseUrl.empty() && !model.empty(); }

    static std::filesystem::path folder();  // %APPDATA%\PromptEditor
    static AppSettings load();
    bool save() const;
};

// %LOCALAPPDATA%\PromptEditor\runs: transcripts of indexing runs.
std::filesystem::path runsFolder();
