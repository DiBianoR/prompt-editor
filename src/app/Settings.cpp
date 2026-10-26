#include "Settings.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dpapi.h>
#include <shlobj.h>
#include <wincrypt.h>

#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace {

std::filesystem::path knownFolder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    std::filesystem::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &path))) out = path;
    CoTaskMemFree(path);
    return out;
}

std::string protect(const std::string& secret) {
    if (secret.empty()) return {};
    DATA_BLOB in{static_cast<DWORD>(secret.size()), reinterpret_cast<BYTE*>(const_cast<char*>(secret.data()))};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"Prompt Editor API key", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return {};
    DWORD len = 0;
    CryptBinaryToStringA(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &len);
    std::string b64(len, '\0');
    CryptBinaryToStringA(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64.data(), &len);
    b64.resize(len);
    LocalFree(out.pbData);
    return b64;
}

std::string unprotect(const std::string& b64) {
    if (b64.empty()) return {};
    DWORD len = 0;
    if (!CryptStringToBinaryA(b64.c_str(), 0, CRYPT_STRING_BASE64, nullptr, &len, nullptr, nullptr)) return {};
    std::string bytes(len, '\0');
    CryptStringToBinaryA(b64.c_str(), 0, CRYPT_STRING_BASE64, reinterpret_cast<BYTE*>(bytes.data()), &len, nullptr,
                         nullptr);
    DATA_BLOB in{len, reinterpret_cast<BYTE*>(bytes.data())};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    std::string secret(reinterpret_cast<char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return secret;
}

}  // namespace

pe::PromptSet AppSettings::prompts() const {
    pe::PromptSet set = pe::defaultPromptSet();
    for (const auto& [id, text] : promptOverrides) set[id] = text;
    return set;
}

std::filesystem::path AppSettings::folder() {
    // Tests point this elsewhere so they never touch the real settings.
    wchar_t custom[MAX_PATH];
    if (GetEnvironmentVariableW(L"PROMPT_EDITOR_SETTINGS_DIR", custom, MAX_PATH)) return custom;
    return knownFolder(FOLDERID_RoamingAppData) / L"PromptEditor";
}

std::filesystem::path runsFolder() { return knownFolder(FOLDERID_LocalAppData) / L"PromptEditor" / L"runs"; }
std::filesystem::path documentsFolder() {
    return knownFolder(FOLDERID_LocalAppData) / L"PromptEditor" / L"documents";
}

AppSettings AppSettings::load() {
    AppSettings s;
    std::ifstream f(folder() / L"settings.json", std::ios::binary);
    if (!f) return s;
    std::ostringstream ss;
    ss << f.rdbuf();
    const nlohmann::json j = nlohmann::json::parse(ss.str(), nullptr, false);
    if (!j.is_object()) return s;
    s.baseUrl = j.value("baseUrl", "");
    s.model = j.value("model", "");
    s.apiKey = unprotect(j.value("apiKeyProtected", ""));
    s.repairs = j.value("repairs", 2);
    s.contextTokens = std::max(8192, j.value("contextTokens", 262144));
    if (auto it = j.find("prompts"); it != j.end() && it->is_object())
        for (const auto& [id, text] : it->items())
            if (text.is_string()) s.promptOverrides[id] = text.get<std::string>();
    return s;
}

bool AppSettings::save() const {
    nlohmann::json j;
    j["baseUrl"] = baseUrl;
    j["model"] = model;
    j["apiKeyProtected"] = protect(apiKey);
    j["repairs"] = repairs;
    j["contextTokens"] = contextTokens;
    j["prompts"] = nlohmann::json::object();
    const pe::PromptSet defaults = pe::defaultPromptSet();
    for (const auto& [id, text] : promptOverrides) {
        auto d = defaults.find(id);
        if (d == defaults.end() || d->second != text) j["prompts"][id] = text;
    }
    std::error_code ec;
    std::filesystem::create_directories(folder(), ec);
    std::ofstream f(folder() / L"settings.json", std::ios::binary | std::ios::trunc);
    f << j.dump(2) << "\n";
    return static_cast<bool>(f);
}
