#pragma once
#include <windows.h>
#include <commctrl.h>

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "scanner/ScanNode.h"
#include "ui/View.h"

namespace ss {

class AnalyzerView : public View {
public:
    void NavigateTo(ScanNode* n);

    void OnCreate() override;
    void Layout(int w, int h) override;
    LRESULT OnMessage(UINT m, WPARAM w, LPARAM l) override;
    void OnShow() override { Rebuild(); }
    void Reload() override { Rebuild(); }
    void OnBusyChanged() override { UpdateSelectionUi(); }
    void DropReferences() override;

private:
    enum Col { C_NAME, C_TYPE, C_SIZE, C_DISK, C_PCT, C_FILES, C_DIRS, C_MOD, C_STATUS, C_COUNT };

    void Rebuild();
    void Sort();
    void RunSearch();
    void GoUp();
    void UpdateSelectionUi();
    void UpdateSortArrow();
    void PaintBand();
    std::wstring CellText(ScanNode* n, int col) const;
    int IconFor(ScanNode* n);
    void DrawPercent(NMLVCUSTOMDRAW* cd);
    void Act(int id);
    void OpenNode(ScanNode* n);
    void SortChanged();
    void ContextMenu(POINT screenPt);
    LRESULT OnNotify(NMHDR* nm, LPARAM l);
    std::vector<ScanNode*> Selected() const;
    bool Visible(const ScanNode* n) const;

    HWND list_ = nullptr, up_ = nullptr, search_ = nullptr, bOpen_ = nullptr, bManage_ = nullptr,
         bRecycle_ = nullptr, bPerm_ = nullptr, bBasket_ = nullptr, bRefresh_ = nullptr, bApps_ = nullptr, status_ = nullptr, empty_ = nullptr,
         bScan_ = nullptr, infoIcon_ = nullptr;
    int crumbRight_ = 0, lastW_ = 0, lastH_ = 0;
    bool barShown_ = false, canDelete_ = false;  // action buttons are shown only while something is selected
    ScanNode* cur_ = nullptr;
    std::wstring keepPath_;
    std::unordered_set<const ScanNode*> checked_;  // rows ticked with the checkbox column (together with highlighted rows when none is ticked)
    bool searchMode_ = false, suppressSearch_ = false, truncated_ = false;
    std::vector<ScanNode*> rows_, searchRows_;
    int sortCol_ = C_SIZE;
    bool sortAsc_ = false;
    struct Crumb {
        RECT r;
        ScanNode* n;
    };
    std::vector<Crumb> crumbs_;
    RECT band_{}, searchFrame_{};
    std::unordered_map<std::wstring, int> iconCache_;
    int folderIcon_ = 0;
};

}  // namespace ss
