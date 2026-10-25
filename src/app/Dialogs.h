#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <map>
#include <string>

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

std::wstring widen(const std::string& s);
std::string narrow(const std::wstring& s);
