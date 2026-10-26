#include "Dialogs.h"

#include <commctrl.h>

#include <algorithm>

#include "LlmClient.h"

namespace {

const wchar_t kModalClass[] = L"PromptEditorModal";

enum : int {
    IDC_URL = 400,
    IDC_KEY,
    IDC_MODEL,
    IDC_LOAD_MODELS,
    IDC_PROMPT_PICK,
    IDC_RESET,
    IDC_PROMPT_EDIT,
    IDC_PROPOSE,
};

// Edit controls want CRLF; everything else here uses LF.
std::wstring toEdit(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        if (c == L'\n') out += L'\r';
        if (c != L'\r') out += c;
    }
    return out;
}

std::wstring fromEdit(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s)
        if (c != L'\r') out += c;
    return out;
}

std::wstring textOf(HWND w) {
    std::wstring s(GetWindowTextLengthW(w), L'\0');
    GetWindowTextW(w, s.data(), static_cast<int>(s.size()) + 1);
    return s;
}

std::string trimmed(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

}  // namespace

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

// ---- ModalWindow ----

int ModalWindow::run(HWND owner, const wchar_t* title, int width, int height) {
    static bool registered = false;
    HINSTANCE instance = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = proc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = kModalClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    dpi_ = GetDpiForWindow(owner);
    RECT orc;
    GetWindowRect(owner, &orc);
    const int w = scale(width), h = scale(height);
    hwnd_ = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kModalClass, title,
                            WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN,
                            orc.left + (orc.right - orc.left - w) / 2, orc.top + (orc.bottom - orc.top - h) / 2, w, h,
                            owner, nullptr, instance, this);
    NONCLIENTMETRICSW ncm{sizeof ncm};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi_);
    font_ = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW lf = ncm.lfMessageFont;
    wcscpy_s(lf.lfFaceName, L"Consolas");
    mono_ = CreateFontIndirectW(&lf);
    onCreate();
    RECT rc;
    GetClientRect(hwnd_, &rc);
    onLayout(rc.right, rc.bottom);

    EnableWindow(owner, FALSE);
    ShowWindow(hwnd_, SW_SHOW);
    MSG msg{};
    while (!done_ && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd_, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(owner, TRUE);
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    DeleteObject(font_);
    DeleteObject(mono_);
    SetActiveWindow(owner);
    if (msg.message == WM_QUIT) PostQuitMessage(static_cast<int>(msg.wParam));
    return result_;
}

void ModalWindow::end(int result) {
    result_ = result;
    done_ = true;
}

HWND ModalWindow::add(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD exStyle) {
    HWND w = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, hwnd_,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(w, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
    return w;
}

void ModalWindow::place(HWND w, int x, int y, int cx, int cy) const {
    SetWindowPos(w, nullptr, x, y, std::max(0, cx), std::max(0, cy), SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT CALLBACK ModalWindow::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* self = static_cast<ModalWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    auto* self = reinterpret_cast<ModalWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && self->font_) {
        switch (msg) {
            case WM_SIZE:
                self->onLayout(LOWORD(lp), HIWORD(lp));
                if (self->customPaint_) InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            case WM_ERASEBKGND:
                if (self->customPaint_) return 1;
                break;
            case WM_PAINT:
                if (self->customPaint_) {
                    PAINTSTRUCT ps;
                    HDC dc = BeginPaint(hwnd, &ps);
                    RECT rc;
                    GetClientRect(hwnd, &rc);
                    HDC mem = CreateCompatibleDC(dc);
                    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
                    HGDIOBJ old = SelectObject(mem, bmp);
                    FillRect(mem, &rc, GetSysColorBrush(COLOR_WINDOW));
                    self->onPaint(mem, rc);
                    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
                    SelectObject(mem, old);
                    DeleteObject(bmp);
                    DeleteDC(mem);
                    EndPaint(hwnd, &ps);
                    return 0;
                }
                break;
            case WM_GETMINMAXINFO: {
                auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
                mm->ptMinTrackSize = {self->scale(560), self->scale(420)};
                return 0;
            }
            case WM_COMMAND:
                self->onCommand(LOWORD(wp), HIWORD(wp));
                return 0;
            case DM_GETDEFID:
                return MAKELRESULT(IDOK, DC_HASDEFID);
            case WM_CLOSE:
                self->end(IDCANCEL);
                return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- CoverageChart ----

void CoverageChart::show(HWND owner) {
    // Room for the title block, axis, one 30px row per bar, legend, note and Close button.
    const int height = 300 + static_cast<int>(rows_.size()) * 30;
    run(owner, L"Topic coverage", 820, std::clamp(height, 420, 900));
}

void CoverageChart::onCreate() {
    customPaint_ = true;
    close_ = add(WC_BUTTONW, L"Close", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK);
}

void CoverageChart::onLayout(int width, int height) {
    const int m = scale(12), rowH = scale(26);
    buttonsTop_ = height - m - rowH;
    place(close_, width - m - scale(90), buttonsTop_, scale(90), rowH);
}

void CoverageChart::onCommand(int id, int) {
    if (id == IDOK || id == IDCANCEL) end(id);
}

void CoverageChart::onPaint(HDC dc, const RECT& rc) {
    // Tier shading matches the editor: tier 1 full colour, tier 5 faint.
    static const double strength[6] = {1.0, 1.0, 0.8, 0.6, 0.4, 0.25};
    static const wchar_t* const tierNames[6] = {L"no tier", L"core", L"high precedence", L"direct", L"slight",
                                                L"adjacent"};
    const COLORREF bg = GetSysColor(COLOR_WINDOW), ink = GetSysColor(COLOR_WINDOWTEXT);
    const COLORREF grid = RGB(0xE2, 0xE2, 0xE2), muted = RGB(0x70, 0x70, 0x70);
    auto fade = [&](COLORREF c, double s) {
        auto mix = [&](int a, int b) { return static_cast<int>(a * s + b * (1 - s)); };
        return RGB(mix(GetRValue(c), GetRValue(bg)), mix(GetGValue(c), GetGValue(bg)), mix(GetBValue(c), GetBValue(bg)));
    };
    auto fill = [&](RECT r, COLORREF c) {
        HBRUSH b = CreateSolidBrush(c);
        FillRect(dc, &r, b);
        DeleteObject(b);
    };
    auto text = [&](const std::wstring& s, RECT r, UINT format, COLORREF color) {
        SetTextColor(dc, color);
        DrawTextW(dc, s.c_str(), static_cast<int>(s.size()), &r, format | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    };
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(dc, font_);

    const int m = scale(16);
    int y = m;
    text(L"Share of the document each topic covers", {m, y, rc.right - m, y + scale(22)}, DT_LEFT, ink);
    y += scale(22);
    text(L"Characters covered, overlaps within a topic counted once. Darker = more load-bearing passages.",
         {m, y, rc.right - m, y + scale(20)}, DT_LEFT | DT_END_ELLIPSIS, muted);
    y += scale(30);

    // Label column sized to the longest name, within limits.
    int labelW = scale(120);
    for (const CoverageRow& row : rows_) {
        SIZE sz;
        GetTextExtentPoint32W(dc, row.name.c_str(), static_cast<int>(row.name.size()), &sz);
        labelW = std::max(labelW, static_cast<int>(sz.cx) + scale(12));
    }
    labelW = std::min<int>(labelW, (rc.right - 2 * m) * 2 / 5);
    const int x0 = m + labelW, x1 = static_cast<int>(rc.right) - m - scale(48);
    const int legendH = scale(28), noteH = scale(20);
    const int rowsBottom = buttonsTop_ - legendH - noteH - scale(12);
    const int axisH = scale(18);
    const int rowH = rows_.empty() ? 0 : std::clamp((rowsBottom - y - axisH) / static_cast<int>(rows_.size()), scale(14), scale(30));
    auto xOf = [&](double fraction) { return x0 + static_cast<int>((x1 - x0) * fraction); };

    // Axis: gridlines and labels at 0, 25, 50, 75, 100%.
    const int gridTop = y + axisH, gridBottom = gridTop + rowH * static_cast<int>(rows_.size());
    for (int p = 0; p <= 100; p += 25) {
        const int x = xOf(p / 100.0);
        fill({x, gridTop, x + 1, gridBottom}, grid);
        text(std::to_wstring(p) + L"%", {x - scale(30), y, x + scale(30), y + axisH}, DT_CENTER, muted);
    }

    int ry = gridTop;
    for (const CoverageRow& row : rows_) {
        text(row.name, {m, ry, x0 - scale(8), ry + rowH}, DT_LEFT | DT_END_ELLIPSIS, ink);
        const int barTop = ry + rowH / 5, barBottom = ry + rowH - rowH / 5;
        int x = x0;
        double cumulative = 0;
        for (int t : {1, 2, 3, 4, 5, 0}) {
            if (!row.chars[t] || !docChars_) continue;
            cumulative += static_cast<double>(row.chars[t]) / docChars_;
            const int nx = xOf(std::min(cumulative, 1.0));
            if (nx > x) fill({x, barTop, nx, barBottom}, fade(row.color, strength[t]));
            x = nx;
        }
        const double share = docChars_ ? 100.0 * row.total() / docChars_ : 0;
        wchar_t label[16];
        swprintf_s(label, share > 0 && share < 1 ? L"<1%%" : L"%.0f%%", share);
        text(label, {x + scale(6), ry, rc.right - m, ry + rowH}, DT_LEFT, ink);
        ry += rowH;
    }

    // Legend: the five tier shades on a neutral colour.
    int lx = m;
    const int ly = std::min(rowsBottom, gridBottom) + scale(12);
    const COLORREF sample = RGB(0x3A, 0x7B, 0xD5);
    for (int t = 1; t <= 5; ++t) {
        fill({lx, ly + scale(5), lx + scale(14), ly + scale(19)}, fade(sample, strength[t]));
        const std::wstring name = std::to_wstring(t) + L" " + tierNames[t];
        SIZE sz;
        GetTextExtentPoint32W(dc, name.c_str(), static_cast<int>(name.size()), &sz);
        text(name, {lx + scale(18), ly, lx + scale(18) + sz.cx + 1, ly + legendH}, DT_LEFT, muted);
        lx += scale(18) + sz.cx + scale(16);
    }
    text(L"Topics overlap one another, so the bars can add up to more than 100%.",
         {m, ly + legendH, rc.right - m, ly + legendH + noteH}, DT_LEFT | DT_END_ELLIPSIS, muted);
    SelectObject(dc, oldFont);
}

// ---- SettingsDialog ----

bool SettingsDialog::show(HWND owner) {
    for (const pe::PromptInfo& p : pe::builtInPrompts()) edited_[p.id] = settings_.prompts().at(p.id);
    return run(owner, L"LLM settings", 860, 720) == IDOK;
}

void SettingsDialog::onCreate() {
    urlLabel_ = add(WC_STATICW, L"Endpoint (base URL)", SS_LEFT);
    url_ = add(WC_EDITW, widen(settings_.baseUrl).c_str(), WS_TABSTOP | ES_AUTOHSCROLL, IDC_URL, WS_EX_CLIENTEDGE);
    SendMessageW(url_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"http://host:port/v1"));
    keyLabel_ = add(WC_STATICW, L"API key", SS_LEFT);
    key_ = add(WC_EDITW, widen(settings_.apiKey).c_str(), WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD, IDC_KEY,
               WS_EX_CLIENTEDGE);
    SendMessageW(key_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"leave empty if the server needs none"));
    modelLabel_ = add(WC_STATICW, L"Model", SS_LEFT);
    model_ = add(WC_COMBOBOXW, L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN | CBS_AUTOHSCROLL, IDC_MODEL);
    SetWindowTextW(model_, widen(settings_.model).c_str());
    loadModels_ = add(WC_BUTTONW, L"Load models", WS_TABSTOP | BS_PUSHBUTTON, IDC_LOAD_MODELS);

    promptLabel_ = add(WC_STATICW, L"Prompt", SS_LEFT);
    promptPick_ = add(WC_COMBOBOXW, L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, IDC_PROMPT_PICK);
    for (const pe::PromptInfo& p : pe::builtInPrompts())
        SendMessageW(promptPick_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(widen(p.title).c_str()));
    reset_ = add(WC_BUTTONW, L"Reset to default", WS_TABSTOP | BS_PUSHBUTTON, IDC_RESET);
    placeholders_ = add(WC_STATICW, L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS);
    promptEdit_ = add(WC_EDITW, L"",
                      WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | ES_NOHIDESEL,
                      IDC_PROMPT_EDIT, WS_EX_CLIENTEDGE);
    SendMessageW(promptEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(mono_), FALSE);
    SendMessageW(promptEdit_, EM_SETLIMITTEXT, 1 << 20, 0);
    ok_ = add(WC_BUTTONW, L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK);
    cancel_ = add(WC_BUTTONW, L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, IDCANCEL);

    SendMessageW(promptPick_, CB_SETCURSEL, 0, 0);
    showPrompt(0);
    SetFocus(url_);
    SendMessageW(url_, EM_SETSEL, 0, -1);
}

void SettingsDialog::onLayout(int width, int height) {
    const int m = scale(12), gap = scale(8), rowH = scale(24), labelW = scale(130), btnW = scale(120);
    const int fieldW = width - 2 * m - labelW;
    int y = m;
    place(urlLabel_, m, y + scale(4), labelW, rowH);
    place(url_, m + labelW, y, fieldW, rowH);
    y += rowH + gap;
    place(keyLabel_, m, y + scale(4), labelW, rowH);
    place(key_, m + labelW, y, fieldW, rowH);
    y += rowH + gap;
    place(modelLabel_, m, y + scale(4), labelW, rowH);
    place(model_, m + labelW, y, fieldW - btnW - gap, scale(300));
    place(loadModels_, width - m - btnW, y, btnW, rowH);
    y += rowH + gap * 3;
    place(promptLabel_, m, y + scale(4), labelW, rowH);
    place(promptPick_, m + labelW, y, fieldW - btnW - gap, scale(300));
    place(reset_, width - m - btnW, y, btnW, rowH);
    y += rowH + gap;
    place(placeholders_, m, y, width - 2 * m, rowH);
    y += rowH;
    const int buttonsY = height - m - rowH;
    place(promptEdit_, m, y, width - 2 * m, buttonsY - gap - y);
    place(cancel_, width - m - scale(90), buttonsY, scale(90), rowH);
    place(ok_, width - m - scale(90) * 2 - gap, buttonsY, scale(90), rowH);
}

void SettingsDialog::onCommand(int id, int code) {
    switch (id) {
        case IDOK:
            collect();
            end(IDOK);
            break;
        case IDCANCEL:
            end(IDCANCEL);
            break;
        case IDC_LOAD_MODELS:
            loadModels();
            break;
        case IDC_PROMPT_PICK:
            if (code == CBN_SELCHANGE) {
                storeCurrentPrompt();
                showPrompt(static_cast<int>(SendMessageW(promptPick_, CB_GETCURSEL, 0, 0)));
            }
            break;
        case IDC_RESET:
            if (shownPrompt_ >= 0) {
                SetWindowTextW(promptEdit_, toEdit(widen(pe::builtInPrompts()[shownPrompt_].text)).c_str());
                updatePromptState();
            }
            break;
        case IDC_PROMPT_EDIT:
            if (code == EN_CHANGE) updatePromptState();
            break;
    }
}

void SettingsDialog::loadModels() {
    const std::string url = trimmed(narrow(textOf(url_)));
    if (url.empty()) {
        MessageBoxW(hwnd_, L"Enter the endpoint first, for example http://localhost:8080/v1", L"LLM settings",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    try {
        LlmClient client(url, "", narrow(textOf(key_)));
        client.setTimeoutSeconds(20);
        const std::vector<std::string> models = client.listModels();
        const std::wstring current = textOf(model_);
        SendMessageW(model_, CB_RESETCONTENT, 0, 0);
        for (const std::string& m : models)
            SendMessageW(model_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(widen(m).c_str()));
        SetWindowTextW(model_, current.c_str());
        SetCursor(old);
        if (models.empty()) {
            MessageBoxW(hwnd_, L"The server answered but listed no models. Type the model name.", L"LLM settings",
                        MB_OK | MB_ICONINFORMATION);
        } else {
            SendMessageW(model_, CB_SHOWDROPDOWN, TRUE, 0);
        }
    } catch (const std::exception& e) {
        SetCursor(old);
        MessageBoxW(hwnd_, (L"Could not load the model list:\n" + widen(e.what())).c_str(), L"LLM settings",
                    MB_OK | MB_ICONWARNING);
    }
}

void SettingsDialog::storeCurrentPrompt() {
    if (shownPrompt_ >= 0) edited_[pe::builtInPrompts()[shownPrompt_].id] = narrow(fromEdit(textOf(promptEdit_)));
}

void SettingsDialog::showPrompt(int index) {
    if (index < 0) return;
    shownPrompt_ = index;
    const pe::PromptInfo& p = pe::builtInPrompts()[index];
    SetWindowTextW(promptEdit_, toEdit(widen(edited_[p.id])).c_str());
    updatePromptState();
}

void SettingsDialog::updatePromptState() {
    if (shownPrompt_ < 0) return;
    const pe::PromptInfo& p = pe::builtInPrompts()[shownPrompt_];
    const bool changed = narrow(fromEdit(textOf(promptEdit_))) != p.text;
    const std::wstring info = L"Filled in when sent: " + widen(p.placeholders) +
                              (changed ? L"      (changed from the default)" : L"      (default)");
    SetWindowTextW(placeholders_, info.c_str());
    EnableWindow(reset_, changed);
}

void SettingsDialog::collect() {
    storeCurrentPrompt();
    settings_.baseUrl = trimmed(narrow(textOf(url_)));
    settings_.apiKey = trimmed(narrow(textOf(key_)));
    settings_.model = trimmed(narrow(textOf(model_)));
    settings_.promptOverrides.clear();
    for (const pe::PromptInfo& p : pe::builtInPrompts())
        if (edited_[p.id] != p.text) settings_.promptOverrides[p.id] = edited_[p.id];
}

// ---- TextViewer ----

void TextViewer::onCreate() {
    edit_ = add(WC_EDITW, toEdit(text_).c_str(),
                WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | ES_NOHIDESEL, 0,
                WS_EX_CLIENTEDGE);
    SendMessageW(edit_, EM_SETLIMITTEXT, 1 << 22, 0);
    SetWindowTextW(edit_, toEdit(text_).c_str());
    source_label_ = add(WC_STATICW, source_.c_str(), SS_LEFT | SS_NOPREFIX | SS_PATHELLIPSIS);
    close_ = add(WC_BUTTONW, L"Close", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK);
    SetFocus(edit_);
}

void TextViewer::onLayout(int width, int height) {
    const int m = scale(12), gap = scale(8), rowH = scale(26);
    const int buttonsY = height - m - rowH;
    place(edit_, m, m, width - 2 * m, buttonsY - gap - m);
    place(source_label_, m, buttonsY + scale(4), width - 3 * m - scale(90), rowH);
    place(close_, width - m - scale(90), buttonsY, scale(90), rowH);
}

void TextViewer::onCommand(int id, int) {
    if (id == IDOK || id == IDCANCEL) end(id);
}

// ---- TopicsDialog ----

TopicsDialog::Result TopicsDialog::show(HWND owner) {
    return static_cast<Result>(run(owner, L"Rebuild index", 620, 560));
}

void TopicsDialog::onCreate() {
    info_ = add(WC_STATICW,
                L"Topics to index, one per line. Optionally add \" | \" and what belongs under the topic. "
                L"Each topic is sent to the model separately, so this takes a while.",
                SS_LEFT);
    edit_ = add(WC_EDITW, toEdit(text_).c_str(),
                WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN, 0, WS_EX_CLIENTEDGE);
    SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(mono_), FALSE);
    propose_ = add(WC_BUTTONW, L"Propose topics with the model", WS_TABSTOP | BS_PUSHBUTTON, IDC_PROPOSE);
    build_ = add(WC_BUTTONW, L"Build index", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK);
    cancel_ = add(WC_BUTTONW, L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, IDCANCEL);
    SetFocus(edit_);
}

void TopicsDialog::onLayout(int width, int height) {
    const int m = scale(12), gap = scale(8), rowH = scale(26);
    place(info_, m, m, width - 2 * m, scale(40));
    const int buttonsY = height - m - rowH;
    place(edit_, m, m + scale(44), width - 2 * m, buttonsY - gap - m - scale(44));
    place(propose_, m, buttonsY, scale(220), rowH);
    place(cancel_, width - m - scale(90), buttonsY, scale(90), rowH);
    place(build_, width - m - scale(90) - gap - scale(110), buttonsY, scale(110), rowH);
}

void TopicsDialog::onCommand(int id, int) {
    if (id == IDOK || id == IDC_PROPOSE) {
        text_ = fromEdit(textOf(edit_));
        if (id == IDOK && trimmed(narrow(text_)).empty()) {
            MessageBoxW(hwnd_, L"Add at least one topic, or let the model propose some.", L"Rebuild index",
                        MB_OK | MB_ICONINFORMATION);
            return;
        }
        end(id == IDOK ? kBuild : kPropose);
    } else if (id == IDCANCEL) {
        end(kCancel);
    }
}
