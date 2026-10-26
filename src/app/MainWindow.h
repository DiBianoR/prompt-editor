#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "Scintilla.h"
#include "Settings.h"
#include "core/Anchor.h"
#include "core/Index.h"
#include "core/Pipeline.h"
#include "core/Text.h"

class LlmClient;
struct RunProgress;

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
        int tier;                // 0 = not rated
    };

    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK overviewProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK treeKeyProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
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
    std::wstring nodeLabel(const NodeRef& node, const std::string& doc) const;
    // The node's ranges as spans with tiers (a topic includes its subtopics).
    std::vector<pe::TierSpan> nodeSpans(const NodeRef& node) const;

    // Editing the index by hand.
    pe::Range* currentRange();
    void addSelectionToTopic();
    void removeCurrentRange();
    void fitCurrentRange();
    void setCurrentTier(int tier);
    void deleteSelectedTopic();
    void indexChangedByHand(const std::wstring& what);
    void showCoverageChart();
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
    void drawNavLabel(const DRAWITEMSTRUCT& di);
    void updateStatus();

    void paintOverview(HDC dc, const RECT& rc);
    void overviewClick(int y, int height);

    // LLM indexing runs on a worker thread; results come back as messages.
    struct Job {
        enum Kind { Build, Discover } kind = Build;
        std::vector<pe::TopicRequest> topics;  // the topics to index in this job
        bool replaceMode = false;               // rebuild these topics, keep the others
        std::filesystem::path resumeFolder;     // continue this interrupted run
        std::wstring topicsText;                // Discover: the topic list to add proposals to
    };
    struct JobResult;
    void openSettings();
    bool offerResume(std::wstring& topicsText);
    void applyRun(const RunProgress& progress);
    void rebuildIndexDialog(std::wstring topicsText = {});
    void rebuildSelectedTopic();
    void startJob(Job job);
    void onJobDone(JobResult* result);
    void cancelJob();
    void appendLog(const std::wstring& line);
    void showLog(bool show);
    void writeIndexFile();

    // Lost ranges: named by the model or the index but not found in the
    // document. Listed under the tree for the user to place or discard.
    struct LostRef {
        int topic;
        int sub;  // -1: the topic itself
        size_t range;
        bool operator==(const LostRef& o) const { return topic == o.topic && sub == o.sub && range == o.range; }
    };
    pe::Topic& nodeTopic(int topic, int sub);
    void updateReview(bool force = false);
    void placeLost();
    void discardLost();
    void selectTreeNode(int topic, int sub);
    void showAnalysis();

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
    bool rebuildingTree_ = false;
    std::set<std::string> expanded_;  // names of the topics open in the tree
    // The line above the editor: text, the current passage's tier (on a chip), more text.
    std::tuple<std::wstring, std::wstring, std::wstring> navText_;
    COLORREF navTierColor_ = 0;

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
    std::wstring indexSource_;  // how the current index came to be, for the status bar

    AppSettings settings_;
    std::thread worker_;
    std::atomic<bool> cancel_{false};
    std::shared_ptr<LlmClient> client_;
    bool jobRunning_ = false;
    bool logVisible_ = false;
    HWND logTitle_ = nullptr;
    HWND logEdit_ = nullptr;
    HWND logCancel_ = nullptr;
    HWND logClose_ = nullptr;

    std::vector<LostRef> lost_;
    HWND reviewLabel_ = nullptr;
    HWND reviewList_ = nullptr;
    HWND reviewPlace_ = nullptr;
    HWND reviewDiscard_ = nullptr;
};
