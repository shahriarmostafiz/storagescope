#pragma once
#include <unordered_set>
#include <vector>

#include "scanner/ScanNode.h"
#include "ui/View.h"

namespace ss {

class LargeFilesView : public View {
public:
    void OnCreate() override;
    void Layout(int w, int h) override;
    LRESULT OnMessage(UINT m, WPARAM w, LPARAM l) override;
    void OnShow() override { Rebuild(); }
    void Reload() override { Rebuild(); }
    void OnBusyChanged() override { UpdateButtons(); }
    void DropReferences() override;

private:
    void Rebuild();
    void UpdateButtons();
    void Act(int id);
    std::vector<ScanNode*> Selected() const;
    std::wstring CellText(ScanNode* n, int col) const;
    uint64_t Threshold() const;

    HWND lblSize_ = nullptr, cbSize_ = nullptr, edCustom_ = nullptr, lblMb_ = nullptr, lblType_ = nullptr, cbType_ = nullptr,
         list_ = nullptr, info_ = nullptr, empty_ = nullptr, bOpen_ = nullptr, bRecycle_ = nullptr, bPerm_ = nullptr,
         note_ = nullptr, bRefresh_ = nullptr, lblStale_ = nullptr, cbStale_ = nullptr, bBasket_ = nullptr;
    void SyncSortUi();
    std::vector<ScanNode*> rows_;
    std::unordered_set<const ScanNode*> checked_;  // rows ticked with the checkbox column
    int sortCol_ = 1;  // Size
    bool sortAsc_ = false;
    int lastW_ = 0, lastH_ = 0;
    bool barShown_ = false;  // the action buttons are shown only while something is selected
};

}  // namespace ss
