#pragma once
#include <vector>

#include "operations/RecycleBin.h"
#include "ui/View.h"

namespace ss {

// Lists what is in the Windows Recycle Bin. Tick items (or highlight them) to restore them or delete them for good.
class RecycleBinView : public View {
public:
    void OnCreate() override;
    void Layout(int w, int h) override;
    LRESULT OnMessage(UINT m, WPARAM w, LPARAM l) override;
    void OnShow() override { Refresh(); }
    void Reload() override { if (IsWindowVisible(hwnd_)) Refresh(); }  // drive space / bin content may have changed
    void OnBusyChanged() override { UpdateButtons(); }

private:
    void Refresh();
    void UpdateButtons();
    std::vector<size_t> Chosen() const;  // ticked rows, or the highlighted ones when none is ticked
    void Restore();
    void Purge();
    void CheckAll(bool on);
    void EmptyBin();

    HWND list_ = nullptr, info_ = nullptr, bRefresh_ = nullptr, bAll_ = nullptr, bNone_ = nullptr, bRestore_ = nullptr, bDelete_ = nullptr,
         bEmpty_ = nullptr, note_ = nullptr;
    RecycleBin bin_;
    bool filling_ = false;
};

}  // namespace ss
