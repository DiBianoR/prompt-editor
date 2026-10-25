#include "MainWindow.h"

#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <fstream>
#include <sstream>

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
    IDC_TREE = 200,
    IDC_EDITOR,
    IDC_PREV_BUTTON,
    IDC_NEXT_BUTTON,
    IDC_NAV_LABEL,
    IDC_STATUS,
    IDC_OVERVIEW,
};

constexpr UINT WM_APP_REFRESH = WM_APP + 1;
constexpr int kIndicTopic = 8;    // every range of the selection, in its topic color
constexpr int kIndicCurrent = 9;  // the hit Next/Previous is on
constexpr int kIndicReview = 10;  // fuzzy or ambiguous anchors: verify these
constexpr COLORREF kGapColor = RGB(0xB0, 0xB0, 0xB0);

const wchar_t kMainClass[] = L"PromptEditorMain";
const wchar_t kOverviewClass[] = L"PromptEditorOverview";
const wchar_t kAppName[] = L"Prompt Editor";

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

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
    AppendMenuW(nav, MF_STRING, IDM_NEXT, L"&Next match\tF3 / Alt+Down");
    AppendMenuW(nav, MF_STRING, IDM_PREV, L"&Previous match\tShift+F3 / Alt+Up");
    AppendMenuW(nav, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(nav, MF_STRING, IDM_FOCUS_TOPICS, L"Go to &topics\tCtrl+1");
    AppendMenuW(nav, MF_STRING, IDM_FOCUS_EDITOR, L"Go to &document\tCtrl+2");
    HMENU bar = CreateMenu();
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(nav), L"&Navigate");

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

    sci(SCI_INDICSETSTYLE, kIndicTopic, INDIC_STRAIGHTBOX);
    sci(SCI_INDICSETFLAGS, kIndicTopic, SC_INDICFLAG_VALUEFORE);
    sci(SCI_INDICSETALPHA, kIndicTopic, 90);
    sci(SCI_INDICSETOUTLINEALPHA, kIndicTopic, 0);
    sci(SCI_INDICSETUNDER, kIndicTopic, 1);

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
    for (HWND w : {tree_, prevButton_, nextButton_, navLabel_, status_})
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
    const int w = rc.right, h = rc.bottom - statusH;
    const int tw = std::clamp(scale(treeWidth_), scale(120), std::max(scale(120), w - scale(200)));
    const int split = scale(5), navH = scale(36), ovW = scale(14), pad = scale(4);
    const int x = tw + split;

    int parts[] = {w / 2, w * 3 / 4 + scale(60), -1};
    SendMessageW(status_, SB_SETPARTS, 3, reinterpret_cast<LPARAM>(parts));

    HDWP dwp = BeginDeferWindowPos(6);
    dwp = DeferWindowPos(dwp, tree_, nullptr, 0, 0, tw, h, SWP_NOZORDER);
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

std::wstring MainWindow::nodeLabel(const NodeRef& node) const {
    if (node.topic == kGapsNode) return L"Unindexed text (" + std::to_wstring(gaps_.size()) + L")";
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
    std::wstring label = widen(t->name) + L"  (" + std::to_wstring(live) + L")";
    if (review) label += L"  • " + std::to_wstring(review) + L" to verify";
    if (lost) label += L"  • " + std::to_wstring(lost) + L" lost";
    return label;
}

void MainWindow::buildTree() {
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
    auto insert = [&](HTREEITEM parent, const NodeRef& node, COLORREF color) {
        nodes_.push_back(node);
        const std::wstring label = nodeLabel(node);
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
    SendMessageW(tree_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(tree_, nullptr, TRUE);
}

void MainWindow::relabelTree() {
    for (size_t i = 0; i < items_.size(); ++i) {
        const std::wstring label = nodeLabel(nodes_[i]);
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
        for (const pe::Gap& g : gaps_) hits_.push_back({g.start, g.end, kGapColor, nullptr});
        return;
    }
    const pe::Topic* t = topicOf(selected_);
    if (!t) return;
    auto add = [&](const pe::Topic& topic) {
        const COLORREF color = parseColor(topic.color, kGapColor);
        for (const pe::Range& r : topic.ranges)
            if (r.status != pe::AnchorStatus::Orphaned) hits_.push_back({r.start, r.end, color, &r});
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
    for (int indic : {kIndicTopic, kIndicCurrent, kIndicReview}) {
        sci(SCI_SETINDICATORCURRENT, indic);
        sci(SCI_INDICATORCLEARRANGE, 0, length);
    }
    for (int i = 0; i < static_cast<int>(hits_.size()); ++i) {
        const Hit& h = hits_[i];
        const sptr_t end = std::min<sptr_t>(static_cast<sptr_t>(h.end), length);
        const sptr_t start = std::min<sptr_t>(static_cast<sptr_t>(h.start), end);
        sci(SCI_SETINDICATORCURRENT, kIndicTopic);
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
    InvalidateRect(overview_, nullptr, FALSE);
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
    std::wstring load;
    if (hasIndex_)
        load = summary_.hashMatched ? L"Index matched the file" : L"Index re-anchored";
    SendMessageW(status_, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(path.c_str()));
    SendMessageW(status_, SB_SETTEXTW, 1, reinterpret_cast<LPARAM>(anchors.c_str()));
    SendMessageW(status_, SB_SETTEXTW, 2, reinterpret_cast<LPARAM>(load.c_str()));
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
        HBRUSH fill = CreateSolidBrush(i == current_ ? darken(h.color, 0.55) : h.color);
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
            }
            break;

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
            } else if (hdr->hwndFrom == tree_ && hdr->code == TVN_SELCHANGEDW) {
                const auto* tv = reinterpret_cast<const NMTREEVIEWW*>(lp);
                const auto i = static_cast<size_t>(tv->itemNew.lParam);
                if (i < nodes_.size()) selectNode(nodes_[i]);
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
            if (confirmDiscard()) DestroyWindow(hwnd_);
            return 0;

        case WM_DESTROY:
            if (uiFont_) DeleteObject(uiFont_);
            if (images_) ImageList_Destroy(images_);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
