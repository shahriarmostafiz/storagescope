#pragma once
#include <string>
#include <unordered_set>
#include <vector>

#include "scanner/ScanNode.h"
#include "scanner/SearchIndex.h"
#include "ui/SelKeep.h"
#include "ui/View.h"

namespace ss {

// Search page: finds files and folders in the current scan (no disk access) through a prebuilt name index.
// Two modes: the whole scan, or only chosen folders. The Filters panel adds type / extension / size / date filters,
// folders to skip and match options.
class SearchView : public View {
public:
    void OnCreate() override;
    void Layout(int w, int h) override;
    LRESULT OnMessage(UINT m, WPARAM w, LPARAM l) override;
    void OnShow() override;
    void Reload() override;  // the tree changed (deletes, live updates): the index is rebuilt on the next search
    void OnBusyChanged() override { UpdateButtons(); }
    void DropReferences() override;
    void SetQuery(const std::wstring& q);  // used by the Analyzer's search box (Enter)

private:
    void Run();
    void Sort();
    void SaveChoices();
    void UpdateButtons();
    void UpdateRemove();
    void Act(int id);
    void ToggleAll();
    void AddFolders(std::vector<std::wstring>& list, HWND lb, const wchar_t* title);
    void RemoveSelected(std::vector<std::wstring>& list, HWND lb);
    void FillList(std::vector<std::wstring>& list, HWND lb);
    void ShowPanels();
    std::vector<ScanNode*> Roots() const;  // the scanned drives / folders (children of "This PC"), empty for a single-root scan
    void FillDrives();
    std::vector<ScanNode*> Selected() const;
    std::wstring CellText(ScanNode* n, int col) const;
    std::wstring Text(HWND h) const;
    bool Checked(HWND h) const;

    HWND lblWhere_ = nullptr, cbWhere_ = nullptr, lblKind_ = nullptr, cbKind_ = nullptr, edQuery_ = nullptr, bFilters_ = nullptr;
    HWND lblScope_ = nullptr, lbScope_ = nullptr, bAddScope_ = nullptr, bRemScope_ = nullptr;
    HWND lblType_ = nullptr, cbType_ = nullptr, edInclude_ = nullptr, edExclude_ = nullptr, edMin_ = nullptr, edMax_ = nullptr, lblDate_ = nullptr,
         cbDate_ = nullptr, chkCase_ = nullptr, chkWord_ = nullptr, chkRegex_ = nullptr;
    HWND lblSkip_ = nullptr, lbSkip_ = nullptr, bAddSkip_ = nullptr, bRemSkip_ = nullptr;
    HWND list_ = nullptr, info_ = nullptr, empty_ = nullptr, note_ = nullptr;
    HWND bOpen_ = nullptr, bBasket_ = nullptr, bSelAll_ = nullptr, bRecycle_ = nullptr, bPerm_ = nullptr;

    SearchIndex index_;
    bool dirty_ = true;      // the index no longer matches the tree: rebuilt on the next search
    bool stale_ = false;     // the tree changed while this page was hidden: search again when shown
    bool advanced_ = false;  // Filters panel open
    std::vector<std::wstring> scope_, skip_;  // folders to search in / to skip (canonical paths)
    std::vector<ScanNode*> rows_;
    SelKeep keep_;
    std::unordered_set<const ScanNode*> checked_;
    int sortCol_ = 0;  // Name
    bool byRelevance_ = true;  // until a column header is clicked: the best match first (exact name, then starts with, then contains)
    void SortByRelevance(const std::wstring& query);
    bool sortAsc_ = true;
    int lastW_ = 0, lastH_ = 0;
    bool barShown_ = false;
    bool foldersMode_ = false;  // "Selected folders..." chosen in the Search in list
    std::wstring drive_;  // chosen drive root path ("" = all drives); not saved: the scan may differ next time
    bool loading_ = true;  // controls are being filled: do not treat their changes as the user's
};

}  // namespace ss
