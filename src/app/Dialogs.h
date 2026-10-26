#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <map>
#include <string>
#include <vector>

#include "Settings.h"

// A resizable modal window built from code (no resource templates): disables
// the owner, runs its own message loop with dialog keyboard handling, and
// returns the code passed to end().
class ModalWindow {
public:
    virtual ~ModalWindow() = default;
    int run(HWND owner, const wchar_t* title, int width, int height);

protected:
    virtual void onCreate() = 0;
    virtual void onLayout(int width, int height) = 0;
    virtual void onCommand(int id, int code) = 0;
    // Windows that draw their own content set customPaint_ and override onPaint
    // (called double-buffered, with the background already filled).
    virtual void onPaint(HDC, const RECT&) {}
    bool customPaint_ = false;

    void end(int result);
    HWND add(const wchar_t* cls, const wchar_t* text, DWORD style, int id = 0, DWORD exStyle = 0);
    int scale(int px) const { return MulDiv(px, dpi_, 96); }
    void place(HWND w, int x, int y, int cx, int cy) const;

    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;
    HFONT mono_ = nullptr;
    int dpi_ = 96;

private:
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    bool done_ = false;
    int result_ = IDCANCEL;
};

// Server settings and the editable prompts.
class SettingsDialog : public ModalWindow {
public:
    explicit SettingsDialog(AppSettings settings) : settings_(std::move(settings)) {}
    bool show(HWND owner);
    const AppSettings& settings() const { return settings_; }

private:
    void onCreate() override;
    void onLayout(int width, int height) override;
    void onCommand(int id, int code) override;
    void loadModels();
    void storeCurrentPrompt();
    void showPrompt(int index);
    void updatePromptState();
    void collect();

    AppSettings settings_;
    std::map<std::string, std::string> edited_;  // prompt id -> text being edited
    int shownPrompt_ = -1;
    HWND urlLabel_{}, url_{}, keyLabel_{}, key_{}, modelLabel_{}, model_{}, loadModels_{};
    HWND promptLabel_{}, promptPick_{}, reset_{}, placeholders_{}, promptEdit_{}, ok_{}, cancel_{};
};

// The topic list to index, one per line ("Name | what belongs under it").
class TopicsDialog : public ModalWindow {
public:
    enum Result { kCancel = IDCANCEL, kBuild = IDOK, kPropose = 300 };
    explicit TopicsDialog(std::wstring text) : text_(std::move(text)) {}
    Result show(HWND owner);
    const std::wstring& text() const { return text_; }

private:
    void onCreate() override;
    void onLayout(int width, int height) override;
    void onCommand(int id, int code) override;

    std::wstring text_;
    HWND info_{}, edit_{}, propose_{}, build_{}, cancel_{};
};

// Read-only text, such as the model's analysis of a topic.
class TextViewer : public ModalWindow {
public:
    TextViewer(std::wstring title, std::wstring text, std::wstring source)
        : title_(std::move(title)), text_(std::move(text)), source_(std::move(source)) {}
    void show(HWND owner) { run(owner, title_.c_str(), 820, 680); }

private:
    void onCreate() override;
    void onLayout(int width, int height) override;
    void onCommand(int id, int code) override;

    std::wstring title_, text_, source_;
    HWND edit_{}, source_label_{}, close_{};
};

// One bar of the coverage chart: characters covered, split by tier ([0] = no tier).
struct CoverageRow {
    std::wstring name;
    COLORREF color = 0;
    std::array<size_t, 6> chars{};
    size_t total() const {
        size_t n = 0;
        for (size_t c : chars) n += c;
        return n;
    }
};

// Horizontal bars, 0-100% of the document, biggest topic first; each bar is
// shaded by how much of it is each tier.
class CoverageChart : public ModalWindow {
public:
    CoverageChart(std::vector<CoverageRow> rows, size_t docChars) : rows_(std::move(rows)), docChars_(docChars) {}
    void show(HWND owner);

private:
    void onCreate() override;
    void onLayout(int width, int height) override;
    void onCommand(int id, int code) override;
    void onPaint(HDC dc, const RECT& rc) override;

    std::vector<CoverageRow> rows_;
    size_t docChars_;
    HWND close_{};
    int buttonsTop_ = 0;
};

std::wstring widen(const std::string& s);
std::string narrow(const std::wstring& s);
