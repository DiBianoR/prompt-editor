#include "MainWindow.h"

#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

#include "Dialogs.h"
#include "LlmClient.h"
#include "core/Text.h"
#include "core/Tracker.h"

namespace {

enum : int {
    IDM_OPEN = 100,
    IDM_SAVE,
    IDM_RELOAD_INDEX,
    IDM_EXIT,
    IDM_NEXT,
    IDM_PREV,
    IDM_FOCUS_TOPICS,
    IDM_FOCUS_EDITOR,
    IDM_REBUILD,
    IDM_REBUILD_TOPIC,
    IDM_CANCEL_INDEXING,
    IDM_SETTINGS,
    IDM_SHOW_ANALYSIS,
    IDM_COVERAGE_CHART,
    IDM_ADD_SELECTION,
    IDM_REMOVE_RANGE,
    IDM_FIT_RANGE,
    IDM_TIER_1,  // ..IDM_TIER_1 + 4 for tiers 1-5
    IDM_TIER_NONE = IDM_TIER_1 + 5,
    IDC_TREE = 200,
    IDC_EDITOR,
    IDC_PREV_BUTTON,
    IDC_NEXT_BUTTON,
    IDC_NAV_LABEL,
    IDC_STATUS,
    IDC_OVERVIEW,
    IDC_LOG_EDIT,
    IDC_LOG_CANCEL,
    IDC_LOG_CLOSE,
    IDC_REVIEW_LIST,
    IDC_REVIEW_PLACE,
    IDC_REVIEW_DISCARD,
};

constexpr UINT WM_APP_REFRESH = WM_APP + 1;
constexpr UINT WM_APP_JOB_LOG = WM_APP + 2;   // lParam: std::wstring*, owned by the receiver
constexpr UINT WM_APP_JOB_DONE = WM_APP + 3;  // lParam: JobResult*, owned by the receiver
constexpr int kIndicTopic = 8;    // ranges of the selection without a tier, in the topic color
constexpr int kIndicCurrent = 9;  // the hit Next/Previous is on
constexpr int kIndicReview = 10;  // fuzzy or ambiguous anchors: verify these
constexpr int kIndicTier1 = 11;   // 11..15: ranges of tiers 1-5, fading with the tier
constexpr int kTierAlpha[pe::kTierCount + 1] = {90, 125, 95, 70, 45, 25};  // [0] = no tier
constexpr double kTierStrength[pe::kTierCount + 1] = {1.0, 1.0, 0.8, 0.6, 0.4, 0.25};
constexpr COLORREF kGapColor = RGB(0xB0, 0xB0, 0xB0);

int indicForTier(int tier) { return tier >= 1 && tier <= pe::kTierCount ? kIndicTier1 + tier - 1 : kIndicTopic; }

const wchar_t kMainClass[] = L"PromptEditorMain";
const wchar_t kOverviewClass[] = L"PromptEditorOverview";
const wchar_t kAppName[] = L"Prompt Editor";

COLORREF parseColor(const std::string& hex, COLORREF fallback) {
    if (hex.size() != 7 || hex[0] != '#') return fallback;
    try {
        const unsigned long v = std::stoul(hex.substr(1), nullptr, 16);
        return RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
    } catch (...) {
        return fallback;
    }
}

COLORREF darken(COLORREF c, double f) {
    return RGB(static_cast<int>(GetRValue(c) * f), static_cast<int>(GetGValue(c) * f),
               static_cast<int>(GetBValue(c) * f));
}

// Mixes c toward the background: strength 1 = c, 0 = background.
COLORREF fade(COLORREF c, double strength, COLORREF background) {
    auto mix = [&](int a, int b) { return static_cast<int>(a * strength + b * (1 - strength)); };
    return RGB(mix(GetRValue(c), GetRValue(background)), mix(GetGValue(c), GetGValue(background)),
               mix(GetBValue(c), GetBValue(background)));
}

std::wstring percent(size_t part, size_t whole) {
    if (whole == 0 || part == 0) return L"0%";
    const double p = 100.0 * part / whole;
    if (p < 1) return L"<1%";
    return std::to_wstring(static_cast<int>(p + 0.5)) + L"%";
}

bool readFile(const std::filesystem::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// Writes next to the target, then swaps it in, so a failed write never truncates.
bool writeFileAtomic(const std::filesystem::path& p, const std::string& data) {
    std::filesystem::path tmp = p;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!f) return false;
    }
    return MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

std::filesystem::path defaultIndexPath(const std::filesystem::path& doc) {
    std::filesystem::path p = doc;
    p.replace_extension(L".index.json");
    return p;
}

void showError(HWND owner, const std::wstring& text) {
    MessageBoxW(owner, text.c_str(), kAppName, MB_OK | MB_ICONWARNING);
}

}  // namespace

bool MainWindow::create(HINSTANCE instance, int show) {
    instance_ = instance;
    Scintilla_RegisterClasses(instance);

    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.lpszClassName = kMainClass;
    RegisterClassExW(&wc);

    WNDCLASSEXW oc{sizeof oc};
    oc.lpfnWndProc = overviewProc;
    oc.hInstance = instance;
    oc.hCursor = LoadCursor(nullptr, IDC_HAND);
    oc.lpszClassName = kOverviewClass;
    RegisterClassExW(&oc);

    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IDM_OPEN, L"&Open...\tCtrl+O");
    AppendMenuW(file, MF_STRING, IDM_SAVE, L"&Save\tCtrl+S");
    AppendMenuW(file, MF_STRING, IDM_RELOAD_INDEX, L"&Reload index\tF5");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_EXIT, L"E&xit");
    HMENU nav = CreatePopupMenu();
    AppendMenuW(nav, MF_STRING, IDM_NEXT, L"&Next match\tF3 / Alt+Down / Right in topics");
    AppendMenuW(nav, MF_STRING, IDM_PREV, L"&Previous match\tShift+F3 / Alt+Up / Left in topics");
    AppendMenuW(nav, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(nav, MF_STRING, IDM_FOCUS_TOPICS, L"Go to &topics\tCtrl+1");
    AppendMenuW(nav, MF_STRING, IDM_FOCUS_EDITOR, L"Go to &document\tCtrl+2");
    HMENU indexMenu = CreatePopupMenu();
    AppendMenuW(indexMenu, MF_STRING, IDM_REBUILD, L"&Build index with the model...\tCtrl+R");
    AppendMenuW(indexMenu, MF_STRING, IDM_REBUILD_TOPIC, L"Rebuild selected &topic");
    AppendMenuW(indexMenu, MF_STRING, IDM_CANCEL_INDEXING, L"&Cancel indexing");
    AppendMenuW(indexMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(indexMenu, MF_STRING, IDM_SHOW_ANALYSIS, L"Show the model's &analysis\tCtrl+I");
    AppendMenuW(indexMenu, MF_STRING, IDM_COVERAGE_CHART, L"Topic co&verage chart\tCtrl+G");
    AppendMenuW(indexMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(indexMenu, MF_STRING, IDM_SETTINGS, L"LLM &settings...");
    HMENU tierMenu = CreatePopupMenu();
    for (int t = 1; t <= pe::kTierCount; ++t) {
        const std::wstring label = L"&" + std::to_wstring(t) + L"  " + widen(pe::tierName(t)) + L"\tAlt+" +
                                   std::to_wstring(t);
        AppendMenuW(tierMenu, MF_STRING, IDM_TIER_1 + t - 1, label.c_str());
    }
    AppendMenuW(tierMenu, MF_STRING, IDM_TIER_NONE, L"&No tier\tAlt+0");
    HMENU passage = CreatePopupMenu();
    AppendMenuW(passage, MF_STRING, IDM_ADD_SELECTION, L"&Add selection to topic\tCtrl+Shift+A");
    AppendMenuW(passage, MF_STRING, IDM_REMOVE_RANGE, L"&Remove passage from topic\tCtrl+Shift+Del");
    AppendMenuW(passage, MF_STRING, IDM_FIT_RANGE, L"&Fit passage to selection\tCtrl+Shift+F");
    AppendMenuW(passage, MF_POPUP, reinterpret_cast<UINT_PTR>(tierMenu), L"Set &tier");
    HMENU bar = CreateMenu();
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(nav), L"&Navigate");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(passage), L"&Passage");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(indexMenu), L"&Index");

    ACCEL keys[] = {
        {FCONTROL | FVIRTKEY, 'O', IDM_OPEN},
        {FCONTROL | FVIRTKEY, 'S', IDM_SAVE},
        {FVIRTKEY, VK_F5, IDM_RELOAD_INDEX},
        {FVIRTKEY, VK_F3, IDM_NEXT},
        {FSHIFT | FVIRTKEY, VK_F3, IDM_PREV},
        {FALT | FVIRTKEY, VK_DOWN, IDM_NEXT},
        {FALT | FVIRTKEY, VK_UP, IDM_PREV},
        {FCONTROL | FVIRTKEY, '1', IDM_FOCUS_TOPICS},
        {FCONTROL | FVIRTKEY, '2', IDM_FOCUS_EDITOR},
        {FCONTROL | FVIRTKEY, 'R', IDM_REBUILD},
        {FCONTROL | FVIRTKEY, 'I', IDM_SHOW_ANALYSIS},
        {FCONTROL | FVIRTKEY, 'G', IDM_COVERAGE_CHART},
        {FCONTROL | FSHIFT | FVIRTKEY, 'A', IDM_ADD_SELECTION},
        {FCONTROL | FSHIFT | FVIRTKEY, VK_DELETE, IDM_REMOVE_RANGE},
        {FCONTROL | FSHIFT | FVIRTKEY, 'F', IDM_FIT_RANGE},
        {FALT | FVIRTKEY, '1', IDM_TIER_1},
        {FALT | FVIRTKEY, '2', IDM_TIER_1 + 1},
        {FALT | FVIRTKEY, '3', IDM_TIER_1 + 2},
        {FALT | FVIRTKEY, '4', IDM_TIER_1 + 3},
        {FALT | FVIRTKEY, '5', IDM_TIER_1 + 4},
        {FALT | FVIRTKEY, '0', IDM_TIER_NONE},
    };
    accel_ = CreateAcceleratorTableW(keys, ARRAYSIZE(keys));

    hwnd_ = CreateWindowExW(WS_EX_ACCEPTFILES, kMainClass, kAppName, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, nullptr, bar, instance,
                            this);
    if (!hwnd_) return false;
    dpi_ = GetDpiForWindow(hwnd_);
    // A comfortable default size, kept inside the monitor's work area.
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& work = mi.rcWork;
    const int w = std::min<int>(scale(1300), (work.right - work.left) * 9 / 10);
    const int h = std::min<int>(scale(860), (work.bottom - work.top) * 9 / 10);
    SetWindowPos(hwnd_, nullptr, work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 2,
                 w, h, SWP_NOZORDER);
    createChildren();
    updateTitle();
    updateNavLabel();
    updateStatus();
    ShowWindow(hwnd_, show);
    return true;
}

void MainWindow::createChildren() {
    tree_ = CreateWindowExW(0, WC_TREEVIEWW, nullptr,
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_LINESATROOT |
                                TVS_SHOWSELALWAYS | TVS_INFOTIP,
                            0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IDC_TREE), instance_, nullptr);
    TreeView_SetExtendedStyle(tree_, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
    SetWindowSubclass(tree_, treeKeyProc, 1, reinterpret_cast<DWORD_PTR>(this));
    prevButton_ = CreateWindowExW(0, WC_BUTTONW, L"▲  Previous", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0,
                                  hwnd_, reinterpret_cast<HMENU>(IDC_PREV_BUTTON), instance_, nullptr);
    nextButton_ = CreateWindowExW(0, WC_BUTTONW, L"▼  Next", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0,
                                  hwnd_, reinterpret_cast<HMENU>(IDC_NEXT_BUTTON), instance_, nullptr);
    navLabel_ = CreateWindowExW(0, WC_STATICW, nullptr, WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_CENTERIMAGE |
                                    SS_ENDELLIPSIS | SS_NOPREFIX,
                                0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IDC_NAV_LABEL), instance_, nullptr);
    editor_ = CreateWindowExW(0, L"Scintilla", nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPCHILDREN, 0, 0,
                              0, 0, hwnd_, reinterpret_cast<HMENU>(IDC_EDITOR), instance_, nullptr);
    overview_ = CreateWindowExW(0, kOverviewClass, nullptr, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd_,
                                reinterpret_cast<HMENU>(IDC_OVERVIEW), instance_, this);
    status_ = CreateWindowExW(0, STATUSCLASSNAMEW, nullptr, WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0,
                              hwnd_, reinterpret_cast<HMENU>(IDC_STATUS), instance_, nullptr);
    // Indexing log, shown at the bottom while the model works.
    logTitle_ = CreateWindowExW(0, WC_STATICW, L"Indexing", WS_CHILD | SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS |
                                    SS_NOPREFIX,
                                0, 0, 0, 0, hwnd_, nullptr, instance_, nullptr);
    logEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, nullptr,
                               WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 0, 0, 0, 0, hwnd_,
                               reinterpret_cast<HMENU>(IDC_LOG_EDIT), instance_, nullptr);
    SendMessageW(logEdit_, EM_SETLIMITTEXT, 1 << 22, 0);
    logCancel_ = CreateWindowExW(0, WC_BUTTONW, L"Cancel", WS_CHILD | WS_TABSTOP, 0, 0, 0, 0, hwnd_,
                                 reinterpret_cast<HMENU>(IDC_LOG_CANCEL), instance_, nullptr);
    logClose_ = CreateWindowExW(0, WC_BUTTONW, L"Hide", WS_CHILD | WS_TABSTOP, 0, 0, 0, 0, hwnd_,
                                reinterpret_cast<HMENU>(IDC_LOG_CLOSE), instance_, nullptr);
    // Lost ranges, under the tree; shown only when there are some.
    reviewLabel_ = CreateWindowExW(0, WC_STATICW, nullptr, WS_CHILD | SS_LEFT | SS_NOPREFIX, 0, 0, 0, 0, hwnd_,
                                   nullptr, instance_, nullptr);
    reviewList_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
                                  WS_CHILD | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS |
                                      LVS_NOSORTHEADER,
                                  0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IDC_REVIEW_LIST), instance_, nullptr);
    ListView_SetExtendedListViewStyle(reviewList_, LVS_EX_FULLROWSELECT | LVS_EX_INFOTIP | LVS_EX_DOUBLEBUFFER);
    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = 110;
    col.pszText = const_cast<wchar_t*>(L"Topic");
    ListView_InsertColumn(reviewList_, 0, &col);
    col.cx = 400;
    col.pszText = const_cast<wchar_t*>(L"Text the model named");
    ListView_InsertColumn(reviewList_, 1, &col);
    reviewPlace_ = CreateWindowExW(0, WC_BUTTONW, L"Place at selection", WS_CHILD | WS_TABSTOP, 0, 0, 0, 0, hwnd_,
                                   reinterpret_cast<HMENU>(IDC_REVIEW_PLACE), instance_, nullptr);
    reviewDiscard_ = CreateWindowExW(0, WC_BUTTONW, L"Discard", WS_CHILD | WS_TABSTOP, 0, 0, 0, 0, hwnd_,
                                     reinterpret_cast<HMENU>(IDC_REVIEW_DISCARD), instance_, nullptr);

    sciFn_ = reinterpret_cast<SciFnDirect>(SendMessageW(editor_, SCI_GETDIRECTFUNCTION, 0, 0));
    sciPtr_ = static_cast<sptr_t>(SendMessageW(editor_, SCI_GETDIRECTPOINTER, 0, 0));
    setupEditor();
    updateFonts();
    layout();
}

void MainWindow::setupEditor() {
    sci(SCI_SETCODEPAGE, SC_CP_UTF8);
    sci(SCI_SETTECHNOLOGY, SC_TECHNOLOGY_DIRECTWRITE);
    sci(SCI_STYLESETFONT, STYLE_DEFAULT, reinterpret_cast<sptr_t>("Consolas"));
    sci(SCI_STYLESETSIZE, STYLE_DEFAULT, 11);
    sci(SCI_STYLECLEARALL);
    sci(SCI_SETWRAPMODE, SC_WRAP_WORD);
    sci(SCI_SETWRAPVISUALFLAGS, SC_WRAPVISUALFLAG_START);
    sci(SCI_SETWRAPINDENTMODE, SC_WRAPINDENT_INDENT);
    sci(SCI_SETMARGINTYPEN, 0, SC_MARGIN_NUMBER);
    sci(SCI_SETMARGINWIDTHN, 1, 0);
    sci(SCI_SETCARETLINEVISIBLEALWAYS, 1);
    sci(SCI_SETCARETLINELAYER, SC_LAYER_UNDER_TEXT);  // translucent, so highlights show through
    sci(SCI_SETELEMENTCOLOUR, SC_ELEMENT_CARET_LINE_BACK, 0x10000000 | RGB(0, 0, 0));
    sci(SCI_SETMODEVENTMASK, SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT);
    sci(SCI_SETYCARETPOLICY, CARET_SLOP | CARET_STRICT | CARET_EVEN, 4);

    // One highlight indicator per tier, so less load-bearing passages show lighter.
    for (int tier = 0; tier <= pe::kTierCount; ++tier) {
        const int indic = indicForTier(tier);
        sci(SCI_INDICSETSTYLE, indic, INDIC_STRAIGHTBOX);
        sci(SCI_INDICSETFLAGS, indic, SC_INDICFLAG_VALUEFORE);
        sci(SCI_INDICSETALPHA, indic, kTierAlpha[tier]);
        sci(SCI_INDICSETOUTLINEALPHA, indic, 0);
        sci(SCI_INDICSETUNDER, indic, 1);
    }

    sci(SCI_INDICSETSTYLE, kIndicCurrent, INDIC_ROUNDBOX);
    sci(SCI_INDICSETFLAGS, kIndicCurrent, SC_INDICFLAG_VALUEFORE);
    sci(SCI_INDICSETALPHA, kIndicCurrent, 70);
    sci(SCI_INDICSETOUTLINEALPHA, kIndicCurrent, 255);
    sci(SCI_INDICSETUNDER, kIndicCurrent, 1);

    sci(SCI_INDICSETSTYLE, kIndicReview, INDIC_SQUIGGLEPIXMAP);
    sci(SCI_INDICSETFORE, kIndicReview, RGB(0xD0, 0x30, 0x30));
}

void MainWindow::updateFonts() {
    NONCLIENTMETRICSW ncm{sizeof ncm};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi_);
    HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);
    for (HWND w : {tree_, prevButton_, nextButton_, navLabel_, status_, logTitle_, logEdit_, logCancel_, logClose_,
                   reviewLabel_, reviewList_, reviewPlace_, reviewDiscard_})
        SendMessageW(w, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    if (uiFont_) DeleteObject(uiFont_);
    uiFont_ = font;
    // Line numbers wide enough for five digits.
    const int digits = static_cast<int>(sci(SCI_TEXTWIDTH, STYLE_LINENUMBER, reinterpret_cast<sptr_t>("_99999")));
    sci(SCI_SETMARGINWIDTHN, 0, digits);
}

void MainWindow::layout() {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    SendMessageW(status_, WM_SIZE, 0, 0);
    RECT sr;
    GetWindowRect(status_, &sr);
    const int statusH = sr.bottom - sr.top;
    const int logH = logVisible_ ? scale(190) : 0;
    const int w = rc.right, h = rc.bottom - statusH - logH;
    if (logVisible_) {
        const int pad = scale(4), rowH = scale(28), btnW = scale(90);
        SetWindowPos(logTitle_, nullptr, pad * 2, h + pad, w - 2 * btnW - pad * 6, rowH, SWP_NOZORDER);
        SetWindowPos(logCancel_, nullptr, w - 2 * btnW - pad * 2, h + pad, btnW, rowH, SWP_NOZORDER);
        SetWindowPos(logClose_, nullptr, w - btnW - pad, h + pad, btnW, rowH, SWP_NOZORDER);
        SetWindowPos(logEdit_, nullptr, 0, h + rowH + pad * 2, w, logH - rowH - pad * 2, SWP_NOZORDER);
    }
    const int tw = std::clamp(scale(treeWidth_), scale(120), std::max(scale(120), w - scale(200)));
    const int split = scale(5), navH = scale(36), ovW = scale(14), pad = scale(4);
    const int x = tw + split;

    int parts[] = {w / 2, w * 3 / 4 + scale(60), -1};
    SendMessageW(status_, SB_SETPARTS, 3, reinterpret_cast<LPARAM>(parts));

    const int reviewH = lost_.empty() ? 0 : std::min(scale(240), h / 2);
    if (reviewH) {
        const int rowH = scale(26), labelH = scale(40), top = h - reviewH;
        SetWindowPos(reviewLabel_, nullptr, pad, top + pad, tw - 2 * pad, labelH, SWP_NOZORDER);
        SetWindowPos(reviewList_, nullptr, 0, top + labelH + pad, tw, reviewH - labelH - rowH - pad * 3, SWP_NOZORDER);
        const int bw = (tw - pad * 3) / 2;
        SetWindowPos(reviewPlace_, nullptr, pad, h - rowH - pad, bw, rowH, SWP_NOZORDER);
        SetWindowPos(reviewDiscard_, nullptr, pad * 2 + bw, h - rowH - pad, bw, rowH, SWP_NOZORDER);
        ListView_SetColumnWidth(reviewList_, 1, std::max(scale(100), tw - scale(110) - GetSystemMetrics(SM_CXVSCROLL) - 4));
        ListView_SetColumnWidth(reviewList_, 0, scale(110));
    }

    HDWP dwp = BeginDeferWindowPos(6);
    dwp = DeferWindowPos(dwp, tree_, nullptr, 0, 0, tw, h - reviewH, SWP_NOZORDER);
    dwp = DeferWindowPos(dwp, prevButton_, nullptr, x + pad, pad, scale(96), navH - 2 * pad, SWP_NOZORDER);
    dwp = DeferWindowPos(dwp, nextButton_, nullptr, x + pad * 2 + scale(96), pad, scale(96), navH - 2 * pad,
                         SWP_NOZORDER);
    const int lx = x + pad * 4 + scale(192);
    dwp = DeferWindowPos(dwp, navLabel_, nullptr, lx, pad, std::max(0, w - lx - pad), navH - 2 * pad, SWP_NOZORDER);
    dwp = DeferWindowPos(dwp, editor_, nullptr, x, navH, std::max(0, w - x - ovW), std::max(0, h - navH),
                         SWP_NOZORDER);
    dwp = DeferWindowPos(dwp, overview_, nullptr, w - ovW, navH, ovW, std::max(0, h - navH), SWP_NOZORDER);
    EndDeferWindowPos(dwp);
}

// ---- Files ---------------------------------------------------------------

bool MainWindow::open(const std::filesystem::path& docPath, std::filesystem::path indexPath) {
    std::string bytes;
    if (!readFile(docPath, bytes)) {
        showError(hwnd_, L"Could not read " + docPath.wstring());
        return false;
    }
    hadBom_ = bytes.compare(0, 3, "\xEF\xBB\xBF") == 0;
    if (hadBom_) bytes.erase(0, 3);

    docPath_ = docPath;
    indexPath_ = indexPath.empty() ? defaultIndexPath(docPath) : indexPath;
    hasIndex_ = false;
    index_ = {};
    summary_ = {};
    std::string json;
    if (readFile(indexPath_, json)) {
        try {
            index_ = pe::parseIndex(json);
            hasIndex_ = true;
        } catch (const std::exception& e) {
            showError(hwnd_, L"The index could not be read and will be ignored:\n" + widen(e.what()));
        }
    }
    if (hasIndex_) summary_ = pe::anchorIndex(index_, bytes);
    indexSource_ = !hasIndex_ ? L"" : summary_.hashMatched ? L"Index matched the file" : L"Index re-anchored";

    loading_ = true;
    sci(SCI_SETEOLMODE, bytes.find("\r\n") != std::string::npos ? SC_EOL_CRLF : SC_EOL_LF);
    sci(SCI_CLEARALL);
    sci(SCI_APPENDTEXT, bytes.size(), reinterpret_cast<sptr_t>(bytes.data()));
    sci(SCI_EMPTYUNDOBUFFER);
    sci(SCI_SETSAVEPOINT);
    sci(SCI_GOTOPOS, 0);
    loading_ = false;

    selected_ = {-1, -1};
    hits_.clear();
    current_ = -1;
    buildTree();
    refresh();
    updateTitle();
    return true;
}

std::string MainWindow::documentText() const {
    const auto* p = reinterpret_cast<const char*>(sci(SCI_GETCHARACTERPOINTER));
    return std::string(p, static_cast<size_t>(sci(SCI_GETLENGTH)));
}

bool MainWindow::save() {
    if (docPath_.empty()) return false;
    const std::string doc = documentText();
    if (!writeFileAtomic(docPath_, hadBom_ ? "\xEF\xBB\xBF" + doc : doc)) {
        showError(hwnd_, L"Could not save " + docPath_.wstring());
        return false;
    }
    if (hasIndex_) {
        pe::refreshFromDocument(index_, doc);
        if (!writeFileAtomic(indexPath_, pe::serializeIndex(index_))) {
            showError(hwnd_, L"The document was saved, but the index could not be written to " +
                                 indexPath_.wstring());
            return false;
        }
    }
    sci(SCI_SETSAVEPOINT);
    refresh();
    return true;
}

bool MainWindow::confirmDiscard() {
    if (!sci(SCI_GETMODIFY)) return true;
    const int answer = MessageBoxW(hwnd_, L"Save changes to the document and its index?", kAppName,
                                   MB_YESNOCANCEL | MB_ICONQUESTION);
    if (answer == IDCANCEL) return false;
    return answer == IDNO || save();
}

void MainWindow::openDialog() {
    if (!confirmDiscard()) return;
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{sizeof ofn};
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"Prompts (*.md;*.txt;*.prompt)\0*.md;*.txt;*.prompt\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) open(file);
}

// Re-reads the index (for example after the LLM refreshed it) and anchors it
// against the text as it is in the editor now, saved or not.
void MainWindow::reloadIndex() {
    if (docPath_.empty()) return;
    std::string json;
    if (!readFile(indexPath_, json)) {
        showError(hwnd_, L"No index found at " + indexPath_.wstring());
        return;
    }
    try {
        index_ = pe::parseIndex(json);
    } catch (const std::exception& e) {
        showError(hwnd_, L"The index could not be read:\n" + widen(e.what()));
        return;
    }
    hasIndex_ = true;
    summary_ = pe::anchorIndex(index_, documentText());
    indexSource_ = L"Index reloaded";
    selected_ = {-1, -1};
    hits_.clear();
    current_ = -1;
    buildTree();
    refresh();
}

// ---- Topic tree ----------------------------------------------------------

const pe::Topic* MainWindow::topicOf(const NodeRef& node) const {
    if (node.topic < 0 || node.topic >= static_cast<int>(index_.topics.size())) return nullptr;
    const pe::Topic& t = index_.topics[node.topic];
    return node.sub < 0 ? &t : &t.subtopics[node.sub];
}

std::vector<pe::TierSpan> MainWindow::nodeSpans(const NodeRef& node) const {
    std::vector<pe::TierSpan> spans;
    if (node.topic == kGapsNode) {
        for (const pe::Gap& g : gaps_) spans.push_back({g.start, g.end, 0});
        return spans;
    }
    const pe::Topic* t = topicOf(node);
    if (!t) return spans;
    auto add = [&](const pe::Topic& topic) {
        for (const pe::Range& r : topic.ranges)
            if (r.status != pe::AnchorStatus::Orphaned) spans.push_back({r.start, r.end, r.tier});
    };
    add(*t);
    if (node.sub < 0)
        for (const pe::Topic& s : t->subtopics) add(s);
    return spans;
}

std::wstring MainWindow::nodeLabel(const NodeRef& node, const std::string& doc) const {
    // Share of the document's characters this node covers, overlaps counted once.
    const auto cov = pe::coverageByTier(doc, nodeSpans(node));
    size_t covered = 0;
    for (size_t c : cov) covered += c;
    const std::wstring share = L"  · " + percent(covered, pe::countChars(doc));
    if (node.topic == kGapsNode) return L"Unindexed text (" + std::to_wstring(gaps_.size()) + L")" + share;
    const pe::Topic* t = topicOf(node);
    int live = 0, lost = 0, review = 0;
    auto count = [&](const pe::Topic& topic) {
        for (const pe::Range& r : topic.ranges) {
            if (r.status == pe::AnchorStatus::Orphaned) {
                ++lost;
            } else {
                ++live;
                review += pe::needsReview(r.status);
            }
        }
    };
    count(*t);
    if (node.sub < 0)
        for (const pe::Topic& s : t->subtopics) count(s);
    std::wstring label = widen(t->name) + L"  (" + std::to_wstring(live) + L")" + share;
    if (review) label += L"  • " + std::to_wstring(review) + L" to verify";
    if (lost) label += L"  • " + std::to_wstring(lost) + L" lost";
    return label;
}

void MainWindow::buildTree() {
    // Deleting items moves the tree's selection item by item; ignore those events.
    rebuildingTree_ = true;
    SendMessageW(tree_, WM_SETREDRAW, FALSE, 0);
    TreeView_DeleteAllItems(tree_);
    nodes_.clear();
    items_.clear();
    if (images_) ImageList_Destroy(images_);
    const int sz = scale(12);
    images_ = ImageList_Create(sz, sz, ILC_COLOR32, 16, 16);

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    auto addSwatch = [&](COLORREF color) {
        HBITMAP bmp = CreateCompatibleBitmap(screen, sz, sz);
        HGDIOBJ old = SelectObject(mem, bmp);
        RECT r{0, 0, sz, sz};
        HBRUSH border = CreateSolidBrush(darken(color, 0.6));
        FillRect(mem, &r, border);
        InflateRect(&r, -1, -1);
        HBRUSH fill = CreateSolidBrush(color);
        FillRect(mem, &r, fill);
        DeleteObject(border);
        DeleteObject(fill);
        SelectObject(mem, old);
        const int i = ImageList_Add(images_, bmp, nullptr);
        DeleteObject(bmp);
        return i;
    };
    const std::string doc = documentText();
    auto insert = [&](HTREEITEM parent, const NodeRef& node, COLORREF color) {
        nodes_.push_back(node);
        const std::wstring label = nodeLabel(node, doc);
        TVINSERTSTRUCTW is{};
        is.hParent = parent;
        is.hInsertAfter = TVI_LAST;
        is.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
        is.item.pszText = const_cast<wchar_t*>(label.c_str());
        is.item.lParam = static_cast<LPARAM>(nodes_.size() - 1);
        is.item.iImage = is.item.iSelectedImage = addSwatch(color);
        HTREEITEM item = TreeView_InsertItem(tree_, &is);
        items_.push_back(item);
        return item;
    };

    for (int i = 0; i < static_cast<int>(index_.topics.size()); ++i) {
        const pe::Topic& t = index_.topics[i];
        HTREEITEM item = insert(TVI_ROOT, {i, -1}, parseColor(t.color, kGapColor));
        for (int j = 0; j < static_cast<int>(t.subtopics.size()); ++j)
            insert(item, {i, j}, parseColor(t.subtopics[j].color, kGapColor));
        TreeView_Expand(tree_, item, TVE_EXPAND);
    }
    if (hasIndex_) insert(TVI_ROOT, {kGapsNode, -1}, kGapColor);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    TreeView_SetImageList(tree_, images_, TVSIL_NORMAL);
    rebuildingTree_ = false;
    SendMessageW(tree_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(tree_, nullptr, TRUE);
    updateReview(true);
}

void MainWindow::relabelTree() {
    const std::string doc = documentText();
    for (size_t i = 0; i < items_.size(); ++i) {
        const std::wstring label = nodeLabel(nodes_[i], doc);
        TVITEMW item{};
        item.mask = TVIF_TEXT;
        item.hItem = items_[i];
        item.pszText = const_cast<wchar_t*>(label.c_str());
        TreeView_SetItem(tree_, &item);
    }
}

// ---- Selection and navigation --------------------------------------------

void MainWindow::selectNode(const NodeRef& node) {
    selected_ = node;
    current_ = -1;
    collectHits();
    applyIndicators();
    if (!hits_.empty()) gotoHit(0);
    updateNavLabel();
    InvalidateRect(overview_, nullptr, FALSE);
}

void MainWindow::collectHits() {
    hits_.clear();
    if (selected_.topic == kGapsNode) {
        for (const pe::Gap& g : gaps_) hits_.push_back({g.start, g.end, kGapColor, nullptr, 0});
        return;
    }
    const pe::Topic* t = topicOf(selected_);
    if (!t) return;
    auto add = [&](const pe::Topic& topic) {
        const COLORREF color = parseColor(topic.color, kGapColor);
        for (const pe::Range& r : topic.ranges)
            if (r.status != pe::AnchorStatus::Orphaned) hits_.push_back({r.start, r.end, color, &r, r.tier});
    };
    add(*t);
    if (selected_.sub < 0)
        for (const pe::Topic& s : t->subtopics) add(s);
    std::stable_sort(hits_.begin(), hits_.end(), [](const Hit& a, const Hit& b) {
        return a.start != b.start ? a.start < b.start : a.end < b.end;
    });
    hits_.erase(std::unique(hits_.begin(), hits_.end(),
                            [](const Hit& a, const Hit& b) { return a.start == b.start && a.end == b.end; }),
                hits_.end());
}

void MainWindow::applyIndicators() {
    const sptr_t length = sci(SCI_GETLENGTH);
    for (int indic : {kIndicCurrent, kIndicReview}) {
        sci(SCI_SETINDICATORCURRENT, indic);
        sci(SCI_INDICATORCLEARRANGE, 0, length);
    }
    for (int tier = 0; tier <= pe::kTierCount; ++tier) {
        sci(SCI_SETINDICATORCURRENT, indicForTier(tier));
        sci(SCI_INDICATORCLEARRANGE, 0, length);
    }
    for (int i = 0; i < static_cast<int>(hits_.size()); ++i) {
        const Hit& h = hits_[i];
        const sptr_t end = std::min<sptr_t>(static_cast<sptr_t>(h.end), length);
        const sptr_t start = std::min<sptr_t>(static_cast<sptr_t>(h.start), end);
        sci(SCI_SETINDICATORCURRENT, indicForTier(h.tier));
        sci(SCI_SETINDICATORVALUE, SC_INDICVALUEBIT | h.color);
        sci(SCI_INDICATORFILLRANGE, start, end - start);
        if (i == current_) {
            sci(SCI_SETINDICATORCURRENT, kIndicCurrent);
            sci(SCI_SETINDICATORVALUE, SC_INDICVALUEBIT | darken(h.color, 0.55));
            sci(SCI_INDICATORFILLRANGE, start, end - start);
        }
        if (h.range && pe::needsReview(h.range->status)) {
            sci(SCI_SETINDICATORCURRENT, kIndicReview);
            sci(SCI_INDICATORFILLRANGE, start, end - start);
        }
    }
}

void MainWindow::gotoHit(int i) {
    if (i < 0 || i >= static_cast<int>(hits_.size())) return;
    current_ = i;
    const Hit& h = hits_[i];
    // Caret at the start of the hit, with the hit placed a third of the way down.
    sci(SCI_GOTOPOS, h.start);
    const sptr_t line = sci(SCI_LINEFROMPOSITION, h.start);
    const sptr_t visible = sci(SCI_VISIBLEFROMDOCLINE, line);
    sci(SCI_SETFIRSTVISIBLELINE, std::max<sptr_t>(0, visible - sci(SCI_LINESONSCREEN) / 3));
    applyIndicators();
    updateNavLabel();
    InvalidateRect(overview_, nullptr, FALSE);
}

// Steps from the caret, like Find Next: the first hit after it, wrapping around.
void MainWindow::next(int direction) {
    if (hits_.empty()) {
        MessageBeep(MB_OK);
        return;
    }
    const size_t caret = static_cast<size_t>(sci(SCI_GETCURRENTPOS));
    const int n = static_cast<int>(hits_.size());
    int target;
    if (direction > 0) {
        target = 0;
        while (target < n && hits_[target].start <= caret) ++target;
        if (target == n) target = 0;
    } else {
        target = n - 1;
        while (target >= 0 && hits_[target].start >= caret) --target;
        if (target < 0) target = n - 1;
    }
    gotoHit(target);
}

void MainWindow::scheduleRefresh() {
    if (refreshPending_) return;
    refreshPending_ = true;
    PostMessageW(hwnd_, WM_APP_REFRESH, 0, 0);
}

// After undo or redo, ranges near the change may have been shifted instead of
// grown back. Any touched or lost range whose stored text is again present
// verbatim snaps back to it.
void MainWindow::reanchorAfterUndo(const std::string& doc) {
    index_.forEachRange([&](pe::Range& r) {
        if (!r.touched && r.status != pe::AnchorStatus::Orphaned) return;
        if (r.exact.empty()) return;
        size_t best = std::string::npos;
        for (size_t i = doc.find(r.exact); i != std::string::npos; i = doc.find(r.exact, i + 1)) {
            const auto dist = [&](size_t x) { return x > r.start ? x - r.start : r.start - x; };
            if (best == std::string::npos || dist(i) < dist(best)) best = i;
        }
        if (best == std::string::npos) return;
        r.start = best;
        r.end = best + r.exact.size();
        if (r.status == pe::AnchorStatus::Orphaned) r.status = pe::AnchorStatus::Moved;
    });
}

void MainWindow::refresh() {
    refreshPending_ = false;
    if (hasIndex_) {
        const std::string doc = documentText();
        if (undoSinceRefresh_) reanchorAfterUndo(doc);
        std::vector<std::pair<size_t, size_t>> covered;
        index_.forEachRange([&](pe::Range& r) {
            if (r.status != pe::AnchorStatus::Orphaned) covered.emplace_back(r.start, r.end);
        });
        gaps_ = pe::findUnindexed(doc, std::move(covered));
    }
    undoSinceRefresh_ = false;

    // Keep Next/Previous on the same hit if it still exists.
    const size_t oldStart = current_ >= 0 && current_ < static_cast<int>(hits_.size())
                                ? (hits_[current_].range ? hits_[current_].range->start : hits_[current_].start)
                                : std::string::npos;
    collectHits();
    current_ = -1;
    for (int i = 0; i < static_cast<int>(hits_.size()); ++i)
        if (hits_[i].start == oldStart) current_ = i;
    applyIndicators();
    relabelTree();
    updateNavLabel();
    updateStatus();
    updateReview();
    InvalidateRect(overview_, nullptr, FALSE);
}

// ---- Lost ranges -------------------------------------------------------------

pe::Topic& MainWindow::nodeTopic(int topic, int sub) {
    pe::Topic& t = index_.topics[topic];
    return sub < 0 ? t : t.subtopics[sub];
}

void MainWindow::updateReview(bool force) {
    std::vector<LostRef> lost;
    if (hasIndex_) {
        for (int i = 0; i < static_cast<int>(index_.topics.size()); ++i)
            for (int j = -1; j < static_cast<int>(index_.topics[i].subtopics.size()); ++j) {
                const pe::Topic& t = nodeTopic(i, j);
                for (size_t k = 0; k < t.ranges.size(); ++k)
                    if (t.ranges[k].status == pe::AnchorStatus::Orphaned) lost.push_back({i, j, k});
            }
    }
    if (!force && lost == lost_) return;
    const bool wasShown = !lost_.empty();
    const int selected = ListView_GetNextItem(reviewList_, -1, LVNI_SELECTED);
    lost_ = std::move(lost);

    SendMessageW(reviewList_, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(reviewList_);
    for (size_t i = 0; i < lost_.size(); ++i) {
        const LostRef& l = lost_[i];
        std::wstring topic = widen(index_.topics[l.topic].name);
        if (l.sub >= 0) topic += L" › " + widen(index_.topics[l.topic].subtopics[l.sub].name);
        std::wstring text = widen(nodeTopic(l.topic, l.sub).ranges[l.range].exact.substr(0, 400));
        std::replace(text.begin(), text.end(), L'\n', L' ');
        std::replace(text.begin(), text.end(), L'\r', L' ');
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = topic.data();
        ListView_InsertItem(reviewList_, &item);
        ListView_SetItemText(reviewList_, static_cast<int>(i), 1, text.data());
    }
    if (!lost_.empty()) {
        const int sel = std::clamp(selected, 0, static_cast<int>(lost_.size()) - 1);
        ListView_SetItemState(reviewList_, sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    SendMessageW(reviewList_, WM_SETREDRAW, TRUE, 0);
    const std::wstring label = L"Lost ranges (" + std::to_wstring(lost_.size()) +
                               L"): not found in the document. Select the passage in the document, then Place.";
    SetWindowTextW(reviewLabel_, label.c_str());
    if (wasShown != !lost_.empty()) {
        for (HWND w : {reviewLabel_, reviewList_, reviewPlace_, reviewDiscard_})
            ShowWindow(w, lost_.empty() ? SW_HIDE : SW_SHOW);
        layout();
    }
}

void MainWindow::placeLost() {
    const int i = ListView_GetNextItem(reviewList_, -1, LVNI_SELECTED);
    if (i < 0 || i >= static_cast<int>(lost_.size())) return;
    const sptr_t a = sci(SCI_GETSELECTIONSTART), b = sci(SCI_GETSELECTIONEND);
    if (a == b) {
        MessageBoxW(hwnd_, L"Select the passage in the document first, then click Place at selection.", kAppName,
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    const LostRef l = lost_[i];
    const std::string doc = documentText();
    pe::Range& r = nodeTopic(l.topic, l.sub).ranges[l.range];
    r.start = static_cast<size_t>(a);
    r.end = static_cast<size_t>(b);
    r.exact = doc.substr(r.start, r.end - r.start);
    r.prefix = pe::contextBefore(doc, r.start);
    r.suffix = pe::contextAfter(doc, r.end);
    r.status = pe::AnchorStatus::Exact;
    r.touched = true;  // placed by hand since the last LLM refresh
    writeIndexFile();
    refresh();
    selectTreeNode(l.topic, l.sub);
}

void MainWindow::discardLost() {
    const int i = ListView_GetNextItem(reviewList_, -1, LVNI_SELECTED);
    if (i < 0 || i >= static_cast<int>(lost_.size())) return;
    if (MessageBoxW(hwnd_, L"Discard this lost range? It will be removed from the index.", kAppName,
                    MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return;
    const LostRef l = lost_[i];
    hits_.clear();  // they point into the ranges being changed
    current_ = -1;
    auto& ranges = nodeTopic(l.topic, l.sub).ranges;
    ranges.erase(ranges.begin() + static_cast<std::ptrdiff_t>(l.range));
    writeIndexFile();
    refresh();
}

void MainWindow::selectTreeNode(int topic, int sub) {
    for (size_t i = 0; i < nodes_.size(); ++i)
        if (nodes_[i].topic == topic && nodes_[i].sub == sub) {
            TreeView_SelectItem(tree_, items_[i]);
            if (selected_.topic == topic && selected_.sub == sub) selectNode(nodes_[i]);  // no change event
            return;
        }
}

// ---- Editing the index by hand ------------------------------------------------

pe::Range* MainWindow::currentRange() {
    if (current_ < 0 || current_ >= static_cast<int>(hits_.size()) || !hits_[current_].range) return nullptr;
    return const_cast<pe::Range*>(hits_[current_].range);  // hits point into index_, which this owns
}

void MainWindow::indexChangedByHand(const std::wstring& what) {
    writeIndexFile();
    refresh();
    SendMessageW(status_, SB_SETTEXTW, 2, reinterpret_cast<LPARAM>(what.c_str()));
}

void MainWindow::addSelectionToTopic() {
    if (!hasIndex_ || selected_.topic < 0) {
        MessageBoxW(hwnd_, L"Select a topic or subtopic in the list first, then the passage in the document.",
                    kAppName, MB_OK | MB_ICONINFORMATION);
        return;
    }
    const sptr_t a = sci(SCI_GETSELECTIONSTART), b = sci(SCI_GETSELECTIONEND);
    if (a == b) {
        MessageBoxW(hwnd_, L"Select the passage in the document first.", kAppName, MB_OK | MB_ICONINFORMATION);
        return;
    }
    const std::string doc = documentText();
    pe::Range r;
    r.start = static_cast<size_t>(a);
    r.end = static_cast<size_t>(b);
    r.exact = doc.substr(r.start, r.end - r.start);
    r.prefix = pe::contextBefore(doc, r.start);
    r.suffix = pe::contextAfter(doc, r.end);
    r.touched = true;
    r.tier = 3;  // "standard" until the user says otherwise (Alt+1..5)
    hits_.clear();  // adding to the vector may move the ranges they point to
    current_ = -1;
    pe::Topic& topic = nodeTopic(selected_.topic, selected_.sub);
    topic.ranges.push_back(r);
    indexChangedByHand(L"Added a passage to " + widen(topic.name));
    for (int i = 0; i < static_cast<int>(hits_.size()); ++i)
        if (hits_[i].range == &topic.ranges.back()) gotoHit(i);
}

void MainWindow::removeCurrentRange() {
    pe::Range* target = currentRange();
    if (!target) {
        MessageBoxW(hwnd_, L"Go to the passage with Previous/Next first; the one the counter is on is removed.",
                    kAppName, MB_OK | MB_ICONINFORMATION);
        return;
    }
    for (pe::Topic& t : index_.topics)
        for (int j = -1; j < static_cast<int>(t.subtopics.size()); ++j) {
            auto& ranges = j < 0 ? t.ranges : t.subtopics[j].ranges;
            for (size_t k = 0; k < ranges.size(); ++k)
                if (&ranges[k] == target) {
                    const std::wstring name = widen(j < 0 ? t.name : t.subtopics[j].name);
                    const int was = current_;
                    hits_.clear();
                    current_ = -1;
                    ranges.erase(ranges.begin() + static_cast<std::ptrdiff_t>(k));
                    indexChangedByHand(L"Removed a passage from " + name);
                    if (!hits_.empty()) gotoHit(std::min(was, static_cast<int>(hits_.size()) - 1));
                    return;
                }
        }
}

void MainWindow::fitCurrentRange() {
    pe::Range* r = currentRange();
    const sptr_t a = sci(SCI_GETSELECTIONSTART), b = sci(SCI_GETSELECTIONEND);
    if (!r || a == b) {
        MessageBoxW(hwnd_, L"Go to the passage with Previous/Next, then select the text it should cover.", kAppName,
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    const std::string doc = documentText();
    r->start = static_cast<size_t>(a);
    r->end = static_cast<size_t>(b);
    r->exact = doc.substr(r->start, r->end - r->start);
    r->prefix = pe::contextBefore(doc, r->start);
    r->suffix = pe::contextAfter(doc, r->end);
    r->status = pe::AnchorStatus::Exact;  // placed by hand, so nothing left to verify
    r->touched = true;
    indexChangedByHand(L"Fitted the passage to the selection");
}

void MainWindow::setCurrentTier(int tier) {
    pe::Range* r = currentRange();
    if (!r) {
        MessageBoxW(hwnd_, L"Go to the passage with Previous/Next first.", kAppName, MB_OK | MB_ICONINFORMATION);
        return;
    }
    r->tier = tier;
    for (Hit& h : hits_)
        if (h.range == r) h.tier = tier;
    indexChangedByHand(tier ? L"Tier set to " + std::to_wstring(tier) + L": " + widen(pe::tierName(tier))
                            : L"Tier cleared");
}

LRESULT CALLBACK MainWindow::treeKeyProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    // In the topic list, Left/Right step through the selected topic's passages
    // (Up/Down already move between topics and subtopics).
    if (msg == WM_KEYDOWN && (wp == VK_LEFT || wp == VK_RIGHT)) {
        reinterpret_cast<MainWindow*>(data)->next(wp == VK_RIGHT ? +1 : -1);
        return 0;
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, treeKeyProc, 1);
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void MainWindow::showCoverageChart() {
    if (!hasIndex_) return;
    const std::string doc = documentText();
    std::vector<CoverageRow> rows;
    for (int i = 0; i < static_cast<int>(index_.topics.size()); ++i) {
        CoverageRow row;
        row.name = widen(index_.topics[i].name);
        row.color = parseColor(index_.topics[i].color, kGapColor);
        row.chars = pe::coverageByTier(doc, nodeSpans({i, -1}));
        rows.push_back(row);
    }
    std::stable_sort(rows.begin(), rows.end(), [](const CoverageRow& a, const CoverageRow& b) {
        return a.total() > b.total();
    });
    CoverageRow gaps;
    gaps.name = L"Unindexed text";
    gaps.color = kGapColor;
    gaps.chars = pe::coverageByTier(doc, nodeSpans({kGapsNode, -1}));
    rows.push_back(gaps);
    CoverageChart chart(std::move(rows), pe::countChars(doc));
    chart.show(hwnd_);
}

void MainWindow::showAnalysis() {
    if (!hasIndex_ || selected_.topic < 0) return;
    const pe::Topic& t = index_.topics[selected_.topic];
    if (t.analysis.empty()) {
        MessageBoxW(hwnd_, L"This topic has no saved analysis. It is recorded when the model builds the topic.",
                    kAppName, MB_OK | MB_ICONINFORMATION);
        return;
    }
    const std::filesystem::path path = std::filesystem::u8path(t.analysis);
    std::string text;
    if (!readFile(path, text)) {
        showError(hwnd_, L"The analysis file is no longer there:\n" + path.wstring());
        return;
    }
    TextViewer viewer(L"The model's analysis: " + widen(t.name), widen(text), path.wstring());
    viewer.show(hwnd_);
}

// ---- Labels --------------------------------------------------------------

void MainWindow::updateTitle() {
    std::wstring title = docPath_.empty() ? std::wstring(kAppName) : docPath_.filename().wstring();
    if (!docPath_.empty() && sci(SCI_GETMODIFY)) title += L" *";
    if (!docPath_.empty()) title += std::wstring(L" - ") + kAppName;
    SetWindowTextW(hwnd_, title.c_str());
}

void MainWindow::updateNavLabel() {
    std::wstring text;
    if (docPath_.empty()) {
        text = L"Open a prompt with Ctrl+O, or drop a file here.";
    } else if (!hasIndex_) {
        text = L"No index: " + indexPath_.filename().wstring() + L" not found next to the document.";
    } else if (selected_.topic == -1) {
        text = L"Select a topic to highlight it.";
    } else {
        std::wstring name;
        std::string note;
        if (selected_.topic == kGapsNode) {
            name = L"Unindexed text";
        } else {
            const pe::Topic* t = topicOf(selected_);
            if (selected_.sub >= 0) name = widen(index_.topics[selected_.topic].name) + L" › ";
            name += widen(t->name);
            note = t->note;
        }
        text = name + L"     ";
        if (hits_.empty()) {
            text += L"no matches";
        } else {
            text += (current_ >= 0 ? std::to_wstring(current_ + 1) : L"–") + L" of " +
                    std::to_wstring(hits_.size());
            if (current_ >= 0 && hits_[current_].range && hits_[current_].range->tier)
                text += L"   tier " + std::to_wstring(hits_[current_].range->tier) + L": " +
                        widen(pe::tierName(hits_[current_].range->tier));
            if (current_ >= 0 && hits_[current_].range && hits_[current_].range->status != pe::AnchorStatus::Exact)
                text += L"   [" + widen(pe::toString(hits_[current_].range->status)) + L"]";
        }
        if (!note.empty()) text += L"     Note: " + widen(note);
    }
    SetWindowTextW(navLabel_, text.c_str());
}

void MainWindow::updateStatus() {
    std::wstring path;
    if (!docPath_.empty())
        path = docPath_.filename().wstring() + (hasIndex_ ? L"  +  " + indexPath_.filename().wstring() : L"");
    std::wstring anchors;
    if (hasIndex_) {
        int live = 0, review = 0, lost = 0, touched = 0;
        index_.forEachRange([&](pe::Range& r) {
            if (r.status == pe::AnchorStatus::Orphaned) {
                ++lost;
            } else {
                ++live;
                review += pe::needsReview(r.status);
            }
            touched += r.touched;
        });
        anchors = std::to_wstring(live) + L" ranges";
        if (review) anchors += L" • " + std::to_wstring(review) + L" to verify";
        if (lost) anchors += L" • " + std::to_wstring(lost) + L" lost";
        anchors += L" • " + std::to_wstring(touched) + L" edited • " + std::to_wstring(gaps_.size()) +
                   L" unindexed";
    }
    const std::wstring source = jobRunning_ ? L"Indexing..." : indexSource_;
    SendMessageW(status_, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(path.c_str()));
    SendMessageW(status_, SB_SETTEXTW, 1, reinterpret_cast<LPARAM>(anchors.c_str()));
    SendMessageW(status_, SB_SETTEXTW, 2, reinterpret_cast<LPARAM>(source.c_str()));
}

// ---- Overview strip ------------------------------------------------------

void MainWindow::paintOverview(HDC dc, const RECT& rc) {
    HBRUSH bg = CreateSolidBrush(RGB(0xF3, 0xF3, 0xF3));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    const int height = rc.bottom - rc.top;
    const sptr_t total = std::max<sptr_t>(1, sci(SCI_VISIBLEFROMDOCLINE, sci(SCI_GETLINECOUNT)));
    auto yOf = [&](sptr_t displayLine) { return static_cast<int>(displayLine * height / total); };

    // The visible part of the document.
    const sptr_t first = sci(SCI_GETFIRSTVISIBLELINE);
    RECT view{rc.left, yOf(first), rc.right, std::max(yOf(first + sci(SCI_LINESONSCREEN)), yOf(first) + 4)};
    HBRUSH viewBrush = CreateSolidBrush(RGB(0xDD, 0xDD, 0xDD));
    FillRect(dc, &view, viewBrush);
    DeleteObject(viewBrush);

    for (int i = 0; i < static_cast<int>(hits_.size()); ++i) {
        const Hit& h = hits_[i];
        const sptr_t a = sci(SCI_VISIBLEFROMDOCLINE, sci(SCI_LINEFROMPOSITION, h.start));
        const sptr_t b = sci(SCI_VISIBLEFROMDOCLINE, sci(SCI_LINEFROMPOSITION, h.end) + 1);
        RECT r{rc.left + 2, yOf(a), rc.right - 2, std::max(yOf(b), yOf(a) + scale(3))};
        HBRUSH fill = CreateSolidBrush(i == current_ ? darken(h.color, 0.55)
                                                     : fade(h.color, kTierStrength[h.tier], RGB(0xF3, 0xF3, 0xF3)));
        FillRect(dc, &r, fill);
        DeleteObject(fill);
    }
}

// Clicking a tick jumps to that hit; clicking elsewhere scrolls there.
void MainWindow::overviewClick(int y, int height) {
    if (height <= 0) return;
    const sptr_t total = std::max<sptr_t>(1, sci(SCI_VISIBLEFROMDOCLINE, sci(SCI_GETLINECOUNT)));
    const int slop = scale(4);
    for (int i = 0; i < static_cast<int>(hits_.size()); ++i) {
        const sptr_t a = sci(SCI_VISIBLEFROMDOCLINE, sci(SCI_LINEFROMPOSITION, hits_[i].start));
        const sptr_t b = sci(SCI_VISIBLEFROMDOCLINE, sci(SCI_LINEFROMPOSITION, hits_[i].end) + 1);
        const int ya = static_cast<int>(a * height / total);
        const int yb = std::max(static_cast<int>(b * height / total), ya + scale(3));
        if (y >= ya - slop && y <= yb + slop) {
            gotoHit(i);
            return;
        }
    }
    const sptr_t line = static_cast<sptr_t>(y) * total / height;
    sci(SCI_SETFIRSTVISIBLELINE, std::max<sptr_t>(0, line - sci(SCI_LINESONSCREEN) / 2));
}

LRESULT CALLBACK MainWindow::overviewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE)
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            // Double-buffered to avoid flicker while scrolling.
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
            HGDIOBJ old = SelectObject(mem, bmp);
            if (self) self->paintOverview(mem, rc);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_LBUTTONDOWN: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            if (self) self->overviewClick(GET_Y_LPARAM(lp), rc.bottom);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- LLM indexing --------------------------------------------------------

// progress.json in a run's folder: the topics asked for and every topic
// finished so far, saved as each one completes. A run that stops early (cancel,
// error, closing the app, a crash) can be resumed from it. Each finished topic
// is stored as a one-topic index with the hash of the text the model saw, so it
// can be anchored to whatever the document looks like when it is applied.
struct RunProgress {
    std::filesystem::path folder;
    std::string document;  // absolute path of the prompt, UTF-8
    std::string state;     // running, interrupted, done, abandoned
    bool replaceMode = false;  // rebuilding one topic, keeping the rest
    std::vector<pe::TopicRequest> topics;
    nlohmann::json completed = nlohmann::json::array();  // [{"name", "index"}]

    std::filesystem::path file() const { return folder / L"progress.json"; }

    std::set<std::string> doneNames() const {
        std::set<std::string> names;
        for (const auto& c : completed) names.insert(c.value("name", ""));
        return names;
    }

    std::vector<pe::TopicRequest> remaining() const {
        const std::set<std::string> done = doneNames();
        std::vector<pe::TopicRequest> out;
        for (const pe::TopicRequest& t : topics)
            if (!done.count(t.name)) out.push_back(t);
        return out;
    }

    bool save() const {
        nlohmann::json j = {{"version", 1}, {"document", document}, {"state", state},
                            {"replaceMode", replaceMode}, {"topics", nlohmann::json::array()},
                            {"completed", completed}};
        for (const pe::TopicRequest& t : topics) j["topics"].push_back({{"name", t.name}, {"hint", t.hint}});
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        return writeFileAtomic(file(), j.dump(1));
    }

    static bool load(const std::filesystem::path& folder, RunProgress& out) {
        std::string text;
        if (!readFile(folder / L"progress.json", text)) return false;
        const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        if (!j.is_object()) return false;
        out = {};
        out.folder = folder;
        out.document = j.value("document", "");
        out.state = j.value("state", "");
        out.replaceMode = j.value("replaceMode", false);
        if (auto t = j.find("topics"); t != j.end() && t->is_array())
            for (const auto& x : *t) out.topics.push_back({x.value("name", ""), x.value("hint", "")});
        if (auto c = j.find("completed"); c != j.end() && c->is_array()) out.completed = *c;
        return true;
    }
};

namespace {

bool samePath(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code ec;
    return std::filesystem::equivalent(a, b, ec) || _wcsicmp(a.c_str(), b.c_str()) == 0;
}

// The newest run for this document that stopped with topics left to do.
bool findInterruptedRun(const std::filesystem::path& doc, RunProgress& out) {
    std::error_code ec;
    std::vector<std::filesystem::path> folders;
    for (const auto& entry : std::filesystem::directory_iterator(runsFolder(), ec))
        if (entry.is_directory()) folders.push_back(entry.path());
    std::sort(folders.rbegin(), folders.rend());  // names start with a timestamp
    for (const auto& folder : folders) {
        RunProgress p;
        if (!RunProgress::load(folder, p)) continue;
        if (!samePath(std::filesystem::u8path(p.document), doc)) continue;
        if (p.state == "done" || p.state == "abandoned") return false;  // the latest run is settled
        if (p.remaining().empty()) continue;
        out = std::move(p);
        return true;
    }
    return false;
}

std::wstring runStartedAt(const std::filesystem::path& folder) {
    const std::wstring n = folder.filename().wstring();  // 20261025-164439-name
    if (n.size() < 15) return n;
    return n.substr(0, 4) + L"-" + n.substr(4, 2) + L"-" + n.substr(6, 2) + L" " + n.substr(9, 2) + L":" + n.substr(11, 2);
}

}  // namespace

struct MainWindow::JobResult {
    Job job;
    std::vector<pe::TopicRequest> proposals;
    std::filesystem::path folder;
    std::string model;
    std::string error;
    bool cancelled = false;
    int failedTopics = 0;
};

void MainWindow::openSettings() {
    SettingsDialog dlg(AppSettings::load());
    if (!dlg.show(hwnd_)) return;
    settings_ = dlg.settings();
    if (!settings_.save()) showError(hwnd_, L"The settings could not be saved to " + AppSettings::folder().wstring());
}

// If a run for this document stopped early, offers to finish it. Returns true
// when the user chose to resume (the job is started) or to cancel altogether.
// Choosing to start over fills `topicsText` with that run's topic list.
bool MainWindow::offerResume(std::wstring& topicsText) {
    RunProgress run;
    if (!findInterruptedRun(docPath_, run)) return false;
    const size_t done = run.completed.size(), total = run.topics.size(), left = run.remaining().size();
    const std::wstring text = L"An indexing run for this document stopped after " + std::to_wstring(done) + L" of " +
                              std::to_wstring(total) + L" topics (started " + runStartedAt(run.folder) + L").\n\n" +
                              L"Yes: resume, indexing the " + std::to_wstring(left) + L" remaining topics.\n" +
                              L"No: start a new run instead.";
    const int answer = MessageBoxW(hwnd_, text.c_str(), kAppName, MB_YESNOCANCEL | MB_ICONQUESTION);
    if (answer == IDCANCEL) return true;
    if (answer == IDNO) {
        run.state = "abandoned";
        run.save();
        for (const pe::TopicRequest& t : run.topics)
            topicsText += widen(t.name) + (t.hint.empty() ? L"" : L" | " + widen(t.hint)) + L"\n";
        return false;
    }
    Job job;
    job.topics = run.remaining();
    job.replaceMode = run.replaceMode;
    job.resumeFolder = run.folder;
    startJob(std::move(job));
    return true;
}

void MainWindow::rebuildIndexDialog(std::wstring topicsText) {
    if (docPath_.empty() || jobRunning_) return;
    if (topicsText.empty() && offerResume(topicsText)) return;
    if (topicsText.empty() && hasIndex_)
        for (const pe::Topic& t : index_.topics) topicsText += widen(t.name) + L"\n";
    TopicsDialog dlg(topicsText, hasIndex_ && !index_.topics.empty());
    const TopicsDialog::Result result = dlg.show(hwnd_);
    if (result == TopicsDialog::kPropose) {
        Job job;
        job.kind = Job::Discover;
        job.topicsText = dlg.text();
        startJob(std::move(job));
    } else if (result == TopicsDialog::kBuild) {
        Job job;
        job.topics = pe::parseTopicLines(narrow(dlg.text()));
        if (!job.topics.empty()) startJob(std::move(job));
    } else if (result == TopicsDialog::kBuildNew) {
        // Only topics the index does not have yet; existing ones are left exactly as they are.
        auto lower = [](std::string s) {
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        std::set<std::string> have;
        for (const pe::Topic& t : index_.topics) have.insert(lower(t.name));
        Job job;
        job.replaceMode = true;
        for (const pe::TopicRequest& t : pe::parseTopicLines(narrow(dlg.text())))
            if (!have.count(lower(t.name))) job.topics.push_back(t);
        if (job.topics.empty()) {
            MessageBoxW(hwnd_, L"Every topic in the list is already in the index. Add new topics to the list, or "
                               L"use Rebuild full index to redo them.",
                        kAppName, MB_OK | MB_ICONINFORMATION);
            rebuildIndexDialog(dlg.text());
            return;
        }
        startJob(std::move(job));
    }
}

void MainWindow::rebuildSelectedTopic() {
    if (jobRunning_ || !hasIndex_ || selected_.topic < 0) return;
    Job job;
    job.replaceMode = true;  // a subtopic rebuilds its whole topic
    job.topics.push_back({index_.topics[selected_.topic].name, ""});
    startJob(std::move(job));
}

void MainWindow::startJob(Job job) {
    if (jobRunning_ || docPath_.empty()) return;
    settings_ = AppSettings::load();
    if (!settings_.serverConfigured()) {
        MessageBoxW(hwnd_, L"Set the endpoint and model first.", kAppName, MB_OK | MB_ICONINFORMATION);
        openSettings();
        if (!settings_.serverConfigured()) return;
    }

    auto result = std::make_unique<JobResult>();
    RunProgress progress;
    if (!job.resumeFolder.empty() && RunProgress::load(job.resumeFolder, progress)) {
        result->folder = job.resumeFolder;  // keep adding to the interrupted run
    } else {
        char stamp[32];
        const std::time_t now = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &now);
        std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
        result->folder = runsFolder() / (widen(stamp) + L"-" + docPath_.stem().wstring());
        progress.folder = result->folder;
        progress.document = std::filesystem::absolute(docPath_).u8string();
        progress.replaceMode = job.replaceMode;
        progress.topics = job.topics;
    }
    progress.state = "running";
    if (job.kind == Job::Build && !progress.save()) {
        showError(hwnd_, L"Could not write the run's progress file in " + result->folder.wstring());
        return;
    }
    result->model = settings_.model;

    client_ = std::make_shared<LlmClient>(settings_.baseUrl, settings_.model, settings_.apiKey);
    client_->setTranscriptFolder(result->folder / L"calls");
    cancel_ = false;
    jobRunning_ = true;
    SetWindowTextW(logEdit_, L"");
    showLog(true);
    SetWindowTextW(logTitle_, (L"Indexing with " + widen(settings_.model)).c_str());
    appendLog(L"Model " + widen(settings_.model) + L" at " + widen(settings_.baseUrl));
    if (!job.resumeFolder.empty())
        appendLog(L"Resuming: " + std::to_wstring(progress.completed.size()) + L" topics already done, " +
                  std::to_wstring(job.topics.size()) + L" to go.");
    updateStatus();

    // The worker gets copies of everything it reads; only cancel_ is shared.
    const std::string snapshot = documentText();
    const std::string docName = narrow(docPath_.filename().wstring());
    const pe::Index previous = index_;
    const bool hasPrevious = hasIndex_;
    const pe::PromptSet prompts = settings_.prompts();
    const int repairs = settings_.repairs;
    result->job = std::move(job);
    std::shared_ptr<LlmClient> client = client_;
    HWND hwnd = hwnd_;
    std::atomic<bool>* cancel = &cancel_;
    worker_ = std::thread([result = result.release(), progress, snapshot, docName, previous, hasPrevious, prompts,
                           repairs, client, hwnd, cancel]() mutable {
        auto log = [hwnd](const std::string& line) {
            PostMessageW(hwnd, WM_APP_JOB_LOG, 0, reinterpret_cast<LPARAM>(new std::wstring(widen(line))));
        };
        pe::PipelineHooks hooks;
        hooks.log = log;
        hooks.cancelled = [cancel] { return cancel->load(); };
        hooks.save = [folder = result->folder](const std::string& name, const std::string& content) {
            std::error_code ec;
            const std::filesystem::path p = folder / std::filesystem::u8path(name);
            std::filesystem::create_directories(p.parent_path(), ec);
            std::ofstream(p, std::ios::binary) << content;
        };
        try {
            const pe::SourceDocument doc(snapshot);
            const pe::Index* prev = hasPrevious ? &previous : nullptr;
            if (result->job.kind == Job::Discover) {
                result->proposals = pe::discoverTopics(*client, doc, prompts, prev, hooks);
            } else {
                // Ids already taken: the topics kept as they are, and topics finished earlier in this run.
                std::set<std::string> used;
                for (const pe::Topic& t : previous.topics) {
                    const bool rebuilt = std::any_of(progress.topics.begin(), progress.topics.end(),
                                                     [&](const pe::TopicRequest& r) { return r.name == t.name; });
                    if (progress.replaceMode && !rebuilt) {
                        used.insert(t.id);
                        for (const pe::Topic& s : t.subtopics) used.insert(s.id);
                    }
                }
                const auto& topics = result->job.topics;
                const size_t total = progress.topics.size();
                for (size_t i = 0; i < topics.size(); ++i) {
                    const size_t number = progress.completed.size() + 1;
                    log("[" + std::to_string(number) + "/" + std::to_string(total) + "] " + topics[i].name);
                    // Colors follow the topic's place in the list. When topics are added to an
                    // index, a topic already there keeps its place, and new ones continue after it.
                    size_t palette = 0;
                    for (size_t k = 0; k < progress.topics.size(); ++k)
                        if (progress.topics[k].name == topics[i].name)
                            palette = progress.replaceMode ? previous.topics.size() + k : k;
                    if (progress.replaceMode)
                        for (size_t k = 0; k < previous.topics.size(); ++k)
                            if (previous.topics[k].name == topics[i].name) palette = k;
                    try {
                        pe::TopicOutcome out = pe::indexTopic(*client, doc, topics[i], prompts, repairs, hooks);
                        size_t ranges = out.topic.spans.size(), lost = out.topic.lost.size();
                        for (const auto& s : out.topic.subtopics) {
                            ranges += s.spans.size();
                            lost += s.lost.size();
                        }
                        log("  done: " + std::to_string(ranges) + " ranges, " +
                            std::to_string(out.topic.subtopics.size()) + " subtopics" +
                            (lost ? ", " + std::to_string(lost) + " lost (listed under the topics)" : ""));
                        pe::Topic topic = pe::toIndexTopic(out.topic, prev, used, palette);
                        topic.analysis =
                            (result->folder / std::filesystem::u8path(out.analysisFile)).make_preferred().u8string();
                        // Saved right away, with the text it refers to, so it survives anything that follows.
                        const pe::Index chunk = pe::assembleIndex(docName, snapshot, {topic});
                        progress.completed.push_back(
                            {{"name", topics[i].name}, {"index", nlohmann::json::parse(pe::serializeIndex(chunk))}});
                        progress.save();
                    } catch (const pe::Cancelled&) {
                        throw;
                    } catch (const std::exception& e) {
                        // The index keeps what it already had for this topic.
                        ++result->failedTopics;
                        log(std::string("  FAILED: ") + e.what());
                    }
                }
            }
        } catch (const pe::Cancelled&) {
            result->cancelled = true;
        } catch (const std::exception& e) {
            result->error = e.what();
        }
        PostMessageW(hwnd, WM_APP_JOB_DONE, 0, reinterpret_cast<LPARAM>(result));
    });
}

void MainWindow::cancelJob() {
    if (!jobRunning_) return;
    cancel_ = true;
    if (client_) client_->abort();
    appendLog(L"Cancelling...");
}

void MainWindow::onJobDone(JobResult* raw) {
    std::unique_ptr<JobResult> result(raw);
    if (worker_.joinable()) worker_.join();
    jobRunning_ = false;
    EnableWindow(logCancel_, FALSE);
    const int calls = client_ ? client_->calls() : 0;
    client_.reset();
    updateStatus();
    const std::wstring transcripts = L"Transcripts: " + result->folder.wstring();

    if (result->job.kind == Job::Discover) {
        if (result->cancelled || !result->error.empty()) {
            appendLog(result->cancelled ? L"Cancelled." : L"Stopped: " + widen(result->error));
            SetWindowTextW(logTitle_, result->cancelled ? L"Cancelled" : L"Proposing topics failed");
            return;
        }
        SetWindowTextW(logTitle_, L"Topics proposed");
        std::wstring text = result->job.topicsText;
        std::set<std::string> have;
        for (const pe::TopicRequest& t : pe::parseTopicLines(narrow(text))) have.insert(t.name);
        if (!text.empty() && text.back() != L'\n') text += L"\n";
        int added = 0;
        for (const pe::TopicRequest& t : result->proposals) {
            if (have.count(t.name)) continue;
            text += widen(t.name) + (t.hint.empty() ? L"" : L" | " + widen(t.hint)) + L"\n";
            ++added;
        }
        appendLog(std::to_wstring(added) + L" new topics proposed. " + transcripts);
        rebuildIndexDialog(text);
        return;
    }

    // Whatever finished is applied, however the run ended.
    RunProgress progress;
    if (!RunProgress::load(result->folder, progress)) {
        appendLog(L"The run's progress file could not be read; the index was not changed.");
        return;
    }
    const size_t done = progress.completed.size(), total = progress.topics.size();
    progress.state = progress.remaining().empty() ? "done" : "interrupted";
    progress.save();
    if (result->cancelled) appendLog(L"Cancelled.");
    if (!result->error.empty()) appendLog(L"Stopped: " + widen(result->error));
    if (done > 0) applyRun(progress);

    int ranges = 0, review = 0, lost = 0;
    index_.forEachRange([&](pe::Range& r) {
        if (r.status == pe::AnchorStatus::Orphaned) {
            ++lost;
        } else {
            ++ranges;
            review += pe::needsReview(r.status);
        }
    });
    if (progress.state == "done") {
        SetWindowTextW(logTitle_, L"Indexing finished");
        appendLog(L"Done: " + std::to_wstring(index_.topics.size()) + L" topics, " + std::to_wstring(ranges) +
                  L" ranges, " + std::to_wstring(review) + L" to verify, " + std::to_wstring(lost) + L" lost, " +
                  std::to_wstring(gaps_.size()) + L" unindexed passages, " + std::to_wstring(calls) +
                  L" model calls" +
                  (result->failedTopics ? L", " + std::to_wstring(result->failedTopics) + L" topics failed" : L"") +
                  L".");
    } else {
        SetWindowTextW(logTitle_, L"Indexing stopped early");
        appendLog(std::to_wstring(done) + L" of " + std::to_wstring(total) + L" topics are finished" +
                  (done ? L" and in the index" : L"") + L". Rebuild index (Ctrl+R) offers to resume the other " +
                  std::to_wstring(total - done) + L".");
    }
    appendLog(transcripts);
}

// Puts a run's finished topics into the index, each anchored to the current
// text. A full rebuild orders the index by the run's topic list and keeps the
// previous version of topics not finished yet; a one-topic rebuild replaces
// just that topic.
void MainWindow::applyRun(const RunProgress& progress) {
    const std::string current = documentText();
    std::map<std::string, pe::Topic> done;
    for (const auto& c : progress.completed) {
        try {
            pe::Index chunk = pe::parseIndex(c.at("index").dump());
            pe::anchorIndex(chunk, current);  // trusts the offsets if the text is unchanged
            if (!chunk.topics.empty()) done[c.value("name", "")] = std::move(chunk.topics.front());
        } catch (const std::exception& e) {
            appendLog(L"Skipped a saved topic that could not be read: " + widen(e.what()));
        }
    }
    auto oldTopic = [&](const std::string& name) -> const pe::Topic* {
        if (!hasIndex_) return nullptr;
        for (const pe::Topic& t : index_.topics)
            if (t.name == name) return &t;
        return nullptr;
    };
    std::vector<pe::Topic> topics;
    if (progress.replaceMode && hasIndex_) {
        topics = index_.topics;
        for (auto& [name, topic] : done) {
            auto it = std::find_if(topics.begin(), topics.end(), [&](const pe::Topic& t) { return t.name == name; });
            if (it != topics.end()) *it = topic;
            else topics.push_back(topic);
        }
    } else {
        for (const pe::TopicRequest& r : progress.topics) {
            if (auto it = done.find(r.name); it != done.end()) topics.push_back(it->second);
            else if (const pe::Topic* old = oldTopic(r.name)) topics.push_back(*old);
        }
    }
    hits_.clear();  // they point into the old topics
    current_ = -1;
    selected_ = {-1, -1};
    if (!hasIndex_) index_ = pe::Index{};
    index_.document = narrow(docPath_.filename().wstring());
    index_.topics = std::move(topics);
    hasIndex_ = true;
    indexSource_ = L"Index built by " + widen(settings_.model);
    writeIndexFile();
    buildTree();
    refresh();
}

// Writes the index now if the document is saved; otherwise it is written with
// the next save, so the index never describes text that is not on disk.
void MainWindow::writeIndexFile() {
    if (sci(SCI_GETMODIFY)) {
        appendLog(L"The index will be saved together with the document (Ctrl+S).");
        return;
    }
    std::error_code ec;
    if (std::filesystem::exists(indexPath_, ec)) {
        std::filesystem::path backup = indexPath_;
        backup += L".bak";
        std::filesystem::copy_file(indexPath_, backup, std::filesystem::copy_options::overwrite_existing, ec);
    }
    pe::refreshFromDocument(index_, documentText());
    if (writeFileAtomic(indexPath_, pe::serializeIndex(index_)))
        appendLog(L"Saved " + indexPath_.filename().wstring() + L" (previous version kept as .bak).");
    else
        appendLog(L"Could not write " + indexPath_.wstring());
}

void MainWindow::appendLog(const std::wstring& line) {
    std::wstring text = line;
    for (size_t i = 0; (i = text.find(L'\n', i)) != std::wstring::npos; i += 2) text.insert(i, 1, L'\r');
    text += L"\r\n";
    const int len = GetWindowTextLengthW(logEdit_);
    SendMessageW(logEdit_, EM_SETSEL, len, len);
    SendMessageW(logEdit_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
    if (jobRunning_ && !line.empty() && line[0] != L' ') SetWindowTextW(logTitle_, line.c_str());
}

void MainWindow::showLog(bool show) {
    logVisible_ = show;
    EnableWindow(logCancel_, jobRunning_);
    for (HWND w : {logTitle_, logEdit_, logCancel_, logClose_}) ShowWindow(w, show ? SW_SHOW : SW_HIDE);
    layout();
}

// ---- Window procedure ----------------------------------------------------

LRESULT CALLBACK MainWindow::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self && self->editor_ ? self->handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT MainWindow::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            layout();
            return 0;

        case WM_DPICHANGED: {
            dpi_ = HIWORD(wp);
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            updateFonts();
            buildTree();
            layout();
            return 0;
        }

        case WM_SETCURSOR: {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd_, &pt);
            const int tw = scale(treeWidth_);
            if (reinterpret_cast<HWND>(wp) == hwnd_ && pt.x >= tw && pt.x < tw + scale(5)) {
                SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
                return TRUE;
            }
            break;
        }

        case WM_LBUTTONDOWN: {
            const int x = GET_X_LPARAM(lp), tw = scale(treeWidth_);
            if (x >= tw && x < tw + scale(5)) {
                draggingSplitter_ = true;
                SetCapture(hwnd_);
            }
            return 0;
        }
        case WM_MOUSEMOVE:
            if (draggingSplitter_) {
                treeWidth_ = std::max(120, MulDiv(GET_X_LPARAM(lp), 96, dpi_));
                layout();
            }
            return 0;
        case WM_LBUTTONUP:
            if (draggingSplitter_) {
                draggingSplitter_ = false;
                ReleaseCapture();
            }
            return 0;

        case WM_DROPFILES: {
            wchar_t file[MAX_PATH];
            if (DragQueryFileW(reinterpret_cast<HDROP>(wp), 0, file, MAX_PATH) && confirmDiscard()) open(file);
            DragFinish(reinterpret_cast<HDROP>(wp));
            return 0;
        }

        case WM_COMMAND:
            if (LOWORD(wp) >= IDM_TIER_1 && LOWORD(wp) < IDM_TIER_1 + pe::kTierCount) {
                setCurrentTier(LOWORD(wp) - IDM_TIER_1 + 1);
                return 0;
            }
            switch (LOWORD(wp)) {
                case IDM_OPEN: openDialog(); return 0;
                case IDM_SAVE: save(); return 0;
                case IDM_RELOAD_INDEX: reloadIndex(); return 0;
                case IDM_EXIT: PostMessageW(hwnd_, WM_CLOSE, 0, 0); return 0;
                case IDM_NEXT:
                case IDC_NEXT_BUTTON: next(+1); return 0;
                case IDM_PREV:
                case IDC_PREV_BUTTON: next(-1); return 0;
                case IDM_FOCUS_TOPICS: SetFocus(tree_); return 0;
                case IDM_FOCUS_EDITOR: SetFocus(editor_); return 0;
                case IDM_REBUILD: rebuildIndexDialog(); return 0;
                case IDM_REBUILD_TOPIC: rebuildSelectedTopic(); return 0;
                case IDM_CANCEL_INDEXING:
                case IDC_LOG_CANCEL: cancelJob(); return 0;
                case IDM_SETTINGS: openSettings(); return 0;
                case IDC_LOG_CLOSE: showLog(false); return 0;
                case IDM_SHOW_ANALYSIS: showAnalysis(); return 0;
                case IDM_COVERAGE_CHART: showCoverageChart(); return 0;
                case IDM_ADD_SELECTION: addSelectionToTopic(); return 0;
                case IDM_REMOVE_RANGE: removeCurrentRange(); return 0;
                case IDM_FIT_RANGE: fitCurrentRange(); return 0;
                case IDM_TIER_NONE: setCurrentTier(0); return 0;
                case IDC_REVIEW_PLACE: placeLost(); return 0;
                case IDC_REVIEW_DISCARD: discardLost(); return 0;
            }
            break;

        case WM_INITMENUPOPUP: {
            HMENU menu = reinterpret_cast<HMENU>(wp);
            auto enable = [&](UINT id, bool on) { EnableMenuItem(menu, id, MF_BYCOMMAND | (on ? MF_ENABLED : MF_GRAYED)); };
            enable(IDM_REBUILD, !jobRunning_ && !docPath_.empty());
            enable(IDM_REBUILD_TOPIC, !jobRunning_ && hasIndex_ && selected_.topic >= 0);
            enable(IDM_CANCEL_INDEXING, jobRunning_);
            enable(IDM_SHOW_ANALYSIS, hasIndex_ && selected_.topic >= 0 &&
                                          !index_.topics[selected_.topic].analysis.empty());
            enable(IDM_COVERAGE_CHART, hasIndex_);
            enable(IDM_ADD_SELECTION, hasIndex_ && selected_.topic >= 0);
            const bool onPassage = current_ >= 0 && current_ < static_cast<int>(hits_.size()) && hits_[current_].range;
            enable(IDM_REMOVE_RANGE, onPassage);
            enable(IDM_FIT_RANGE, onPassage);
            for (int t = 0; t <= pe::kTierCount; ++t) {
                const UINT id = t ? IDM_TIER_1 + t - 1 : IDM_TIER_NONE;
                enable(id, onPassage);
                CheckMenuItem(menu, id,
                              MF_BYCOMMAND | (onPassage && hits_[current_].range->tier == t ? MF_CHECKED : MF_UNCHECKED));
            }
            return 0;
        }

        case WM_APP_JOB_LOG: {
            std::unique_ptr<std::wstring> line(reinterpret_cast<std::wstring*>(lp));
            appendLog(*line);
            return 0;
        }
        case WM_APP_JOB_DONE:
            onJobDone(reinterpret_cast<JobResult*>(lp));
            return 0;

        case WM_NOTIFY: {
            const auto* hdr = reinterpret_cast<const NMHDR*>(lp);
            if (hdr->hwndFrom == editor_) {
                const auto* n = reinterpret_cast<const SCNotification*>(lp);
                switch (hdr->code) {
                    case SCN_MODIFIED:
                        if (loading_ || !hasIndex_) break;
                        if (n->modificationType & SC_MOD_INSERTTEXT)
                            pe::trackInsert(index_, static_cast<size_t>(n->position), static_cast<size_t>(n->length));
                        if (n->modificationType & SC_MOD_DELETETEXT)
                            pe::trackDelete(index_, static_cast<size_t>(n->position), static_cast<size_t>(n->length));
                        if (n->modificationType & (SC_PERFORMED_UNDO | SC_PERFORMED_REDO)) undoSinceRefresh_ = true;
                        scheduleRefresh();
                        break;
                    case SCN_SAVEPOINTREACHED:
                    case SCN_SAVEPOINTLEFT:
                        updateTitle();
                        break;
                    case SCN_UPDATEUI:
                        if (n->updated & SC_UPDATE_V_SCROLL) InvalidateRect(overview_, nullptr, FALSE);
                        break;
                }
            } else if (hdr->hwndFrom == tree_ && hdr->code == TVN_SELCHANGEDW && !rebuildingTree_) {
                const auto* tv = reinterpret_cast<const NMTREEVIEWW*>(lp);
                const auto i = static_cast<size_t>(tv->itemNew.lParam);
                if (i < nodes_.size()) selectNode(nodes_[i]);
            } else if (hdr->hwndFrom == reviewList_ && hdr->code == NM_DBLCLK) {
                // Show the topic the lost range belongs to, for context.
                const int i = reinterpret_cast<const NMITEMACTIVATE*>(lp)->iItem;
                if (i >= 0 && i < static_cast<int>(lost_.size())) selectTreeNode(lost_[i].topic, lost_[i].sub);
            } else if (hdr->hwndFrom == tree_ && hdr->code == TVN_GETINFOTIPW) {
                auto* tip = reinterpret_cast<NMTVGETINFOTIPW*>(lp);
                const auto i = static_cast<size_t>(tip->lParam);
                const pe::Topic* t = i < nodes_.size() ? topicOf(nodes_[i]) : nullptr;
                if (t && !t->note.empty()) lstrcpynW(tip->pszText, widen(t->note).c_str(), tip->cchTextMax);
            }
            break;
        }

        case WM_APP_REFRESH:
            refresh();
            return 0;

        case WM_CLOSE:
            if (jobRunning_) {
                if (MessageBoxW(hwnd_, L"The model is still indexing. Stop it and exit?", kAppName,
                                MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
                    return 0;
                cancelJob();
                if (worker_.joinable()) worker_.join();
                // Apply the topics that finished before closing; the run stays resumable.
                MSG pending;
                if (PeekMessageW(&pending, hwnd_, WM_APP_JOB_DONE, WM_APP_JOB_DONE, PM_REMOVE))
                    onJobDone(reinterpret_cast<JobResult*>(pending.lParam));
                jobRunning_ = false;
            }
            if (confirmDiscard()) DestroyWindow(hwnd_);
            return 0;

        case WM_DESTROY:
            if (worker_.joinable()) {
                cancelJob();
                worker_.join();
            }
            if (uiFont_) DeleteObject(uiFont_);
            if (images_) ImageList_Destroy(images_);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
