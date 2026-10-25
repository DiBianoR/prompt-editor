#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>

#include <filesystem>
#include <string>
#include <vector>

#include "Scintilla.h"
#include "core/Anchor.h"
#include "core/Index.h"

// Two panes: topics from the index on the left, the document on the right.
// Selecting a topic highlights its ranges; Next/Previous steps through them.
class MainWindow {
public:
    bool create(HINSTANCE instance, int show);
    bool open(const std::filesystem::path& docPath, std::filesystem::path indexPath = {});
    HWND hwnd() const { return hwnd_; }
    HACCEL accelerators() const { return accel_; }

private:
    // A tree node: a topic (sub < 0), a subtopic, or the unindexed-text node.
    struct NodeRef {
        int topic;
        int sub;
    };
    static constexpr int kGapsNode = -2;

    // One highlighted span of the current selection, in document order.
    struct Hit {
        size_t start;
        size_t end;
        COLORREF color;
        const pe::Range* range;  // null for unindexed text
    };

    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK overviewProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

    void createChildren();
    void setupEditor();
    void updateFonts();
    void layout();
    int scale(int px) const { return MulDiv(px, dpi_, 96); }
    sptr_t sci(unsigned int msg, uptr_t wp = 0, sptr_t lp = 0) const { return sciFn_(sciPtr_, msg, wp, lp); }

    std::string documentText() const;
    bool save();
    bool confirmDiscard();
    void openDialog();
    void reloadIndex();

    void buildTree();
    void relabelTree();
    std::wstring nodeLabel(const NodeRef& node) const;
    const pe::Topic* topicOf(const NodeRef& node) const;

    void selectNode(const NodeRef& node);
    void collectHits();
    void applyIndicators();
    void gotoHit(int i);
    void next(int direction);
    void scheduleRefresh();
    void refresh();
    void reanchorAfterUndo(const std::string& doc);

    void updateTitle();
    void updateNavLabel();
    void updateStatus();

    void paintOverview(HDC dc, const RECT& rc);
    void overviewClick(int y, int height);

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND tree_ = nullptr;
    HWND editor_ = nullptr;
    HWND prevButton_ = nullptr;
    HWND nextButton_ = nullptr;
    HWND navLabel_ = nullptr;
    HWND status_ = nullptr;
    HWND overview_ = nullptr;
    HACCEL accel_ = nullptr;
    HFONT uiFont_ = nullptr;
    HIMAGELIST images_ = nullptr;
    SciFnDirect sciFn_ = nullptr;
    sptr_t sciPtr_ = 0;
    int dpi_ = 96;
    int treeWidth_ = 300;  // at 96 DPI
    bool draggingSplitter_ = false;

    std::filesystem::path docPath_;
    std::filesystem::path indexPath_;
    bool hasIndex_ = false;
    bool hadBom_ = false;
    bool loading_ = false;
    bool refreshPending_ = false;
    bool undoSinceRefresh_ = false;
    pe::Index index_;
    pe::AnchorSummary summary_;
    std::vector<pe::Gap> gaps_;

    std::vector<NodeRef> nodes_;  // tree item lParam indexes this
    std::vector<HTREEITEM> items_;
    NodeRef selected_{-1, -1};
    std::vector<Hit> hits_;
    int current_ = -1;
};
