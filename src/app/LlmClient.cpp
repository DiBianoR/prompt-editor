#include "LlmClient.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string lastError(const char* what) {
    const DWORD e = GetLastError();
    std::string msg = std::string(what) + " failed (" + std::to_string(e) + ")";
    if (e == ERROR_WINHTTP_TIMEOUT) msg = std::string(what) + ": the server did not answer in time";
    else if (e == ERROR_WINHTTP_CANNOT_CONNECT) msg = "cannot connect to the server";
    else if (e == ERROR_WINHTTP_NAME_NOT_RESOLVED) msg = "the server name could not be resolved";
    else if (e == ERROR_WINHTTP_INVALID_URL || e == ERROR_WINHTTP_UNRECOGNIZED_SCHEME) msg = "the endpoint is not a valid URL";
    return msg;
}

struct Handle {
    HINTERNET h = nullptr;
    explicit Handle(HINTERNET handle) : h(handle) {}
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

}  // namespace

LlmClient::LlmClient(std::string baseUrl, std::string model, std::string apiKey)
    : baseUrl_(std::move(baseUrl)), model_(std::move(model)), apiKey_(std::move(apiKey)) {
    while (!baseUrl_.empty() && baseUrl_.back() == '/') baseUrl_.pop_back();
}

LlmClient::~LlmClient() = default;

void LlmClient::abort() {
    aborted_ = true;
    std::lock_guard<std::mutex> lock(mutex_);
    if (current_) {
        // Closing the handle makes the blocked WinHTTP call on the worker return.
        WinHttpCloseHandle(static_cast<HINTERNET>(current_));
        current_ = nullptr;
    }
}

std::string LlmClient::request(const wchar_t* method, const std::string& path, const std::string& body, int& status) {
    if (aborted_) throw pe::Cancelled();
    const std::wstring url = widen(baseUrl_ + path);
    URL_COMPONENTS parts{sizeof parts};
    wchar_t host[256], urlPath[2048];
    parts.lpszHostName = host;
    parts.dwHostNameLength = ARRAYSIZE(host);
    parts.lpszUrlPath = urlPath;
    parts.dwUrlPathLength = ARRAYSIZE(urlPath);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) throw std::runtime_error("the endpoint is not a valid URL");

    Handle session(WinHttpOpen(L"PromptEditor/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) throw std::runtime_error(lastError("WinHttpOpen"));
    const int receiveMs = timeoutSeconds_ * 1000;
    WinHttpSetTimeouts(session.h, 10000, 15000, 60000, receiveMs);
    Handle connect(WinHttpConnect(session.h, host, parts.nPort, 0));
    if (!connect.h) throw std::runtime_error(lastError("connect"));
    HINTERNET req = WinHttpOpenRequest(connect.h, method, urlPath, nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!req) throw std::runtime_error(lastError("open request"));
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current_ = req;
    }
    // Whoever gets here first closes the request: this thread, or abort().
    struct Release {
        LlmClient* self;
        HINTERNET req;
        ~Release() {
            std::lock_guard<std::mutex> lock(self->mutex_);
            if (self->current_ == req) {
                WinHttpCloseHandle(req);
                self->current_ = nullptr;
            }
        }
    } release{this, req};

    std::wstring headers = L"Content-Type: application/json\r\n";
    if (!apiKey_.empty()) headers += L"Authorization: Bearer " + widen(apiKey_) + L"\r\n";
    const BOOL sent = WinHttpSendRequest(req, headers.c_str(), static_cast<DWORD>(-1L),
                                         const_cast<char*>(body.data()), static_cast<DWORD>(body.size()),
                                         static_cast<DWORD>(body.size()), 0);
    if (!sent || !WinHttpReceiveResponse(req, nullptr)) {
        if (aborted_) throw pe::Cancelled();
        throw std::runtime_error(lastError("request"));
    }
    DWORD code = 0, size = sizeof code;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &code, &size, WINHTTP_NO_HEADER_INDEX);
    status = static_cast<int>(code);
    std::string response;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(req, &available)) {
            if (aborted_) throw pe::Cancelled();
            throw std::runtime_error(lastError("reading the reply"));
        }
        if (available == 0) break;
        const size_t at = response.size();
        response.resize(at + available);
        DWORD read = 0;
        if (!WinHttpReadData(req, response.data() + at, available, &read)) {
            if (aborted_) throw pe::Cancelled();
            throw std::runtime_error(lastError("reading the reply"));
        }
        response.resize(at + read);
    }
    return response;
}

std::vector<std::string> LlmClient::listModels() {
    int status = 0;
    const std::string body = request(L"GET", "/models", "", status);
    if (status != 200) throw std::runtime_error("HTTP " + std::to_string(status) + ": " + body.substr(0, 300));
    const nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
    std::vector<std::string> models;
    if (j.is_object() && j.contains("data") && j["data"].is_array())
        for (const auto& m : j["data"])
            if (m.contains("id") && m["id"].is_string()) models.push_back(m["id"].get<std::string>());
    return models;
}

std::string LlmClient::chat(const std::vector<pe::ChatMessage>& messages, const nlohmann::json* schema) {
    nlohmann::json msgs = nlohmann::json::array();
    for (const pe::ChatMessage& m : messages) msgs.push_back({{"role", m.role}, {"content", m.content}});
    for (;;) {
        const int mode = schema ? formatMode_ : 2;
        nlohmann::json body = {{"model", model_}, {"messages", msgs}, {"temperature", 0.2}, {"max_tokens", 16384}};
        if (mode == 0)
            body["response_format"] = {{"type", "json_schema"}, {"json_schema", {{"name", "topic"}, {"schema", *schema}}}};
        else if (mode == 1)
            body["response_format"] = {{"type", "json_object"}};

        const auto started = std::chrono::steady_clock::now();
        int status = 0;
        const std::string response = request(L"POST", "/chat/completions", body.dump(), status);
        if (status != 200) {
            // A server that rejects the response format gets the next simpler one.
            const bool formatProblem = status == 400 || status == 422 || response.find("format") != std::string::npos;
            if (mode < 2 && formatProblem) {
                ++formatMode_;
                continue;
            }
            throw std::runtime_error("HTTP " + std::to_string(status) + " from the server: " + response.substr(0, 300));
        }
        const nlohmann::json j = nlohmann::json::parse(response, nullptr, false);
        if (!j.is_object() || !j.contains("choices") || j["choices"].empty())
            throw std::runtime_error("unexpected reply from the server: " + response.substr(0, 300));
        const nlohmann::json& choice = j["choices"][0];
        std::string content;
        if (choice.contains("message") && choice["message"].contains("content") && choice["message"]["content"].is_string())
            content = choice["message"]["content"].get<std::string>();
        ++calls_;
        if (!transcripts_.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(transcripts_, ec);
            char name[16];
            std::snprintf(name, sizeof name, "%03d.json", calls_);
            nlohmann::json log = {
                {"seconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()},
                {"response_format", mode == 0 ? "json_schema" : mode == 1 ? "json_object" : "none"},
                {"messages", msgs},
                {"reply", content},
                {"finish_reason", choice.value("finish_reason", "")},
                {"usage", j.value("usage", nlohmann::json())}};
            std::ofstream(transcripts_ / name, std::ios::binary) << log.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
        }
        return content;
    }
}
