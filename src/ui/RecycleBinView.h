#pragma once
#include <string>
#include <thread>
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
    enum class Job { Restore, Purge, Empty };
    void RunJob(Job job, const std::vector<size_t>& idx);  // the shell work can take many seconds: never on the UI thread
    void OnJobDone();

    HWND list_ = nullptr, info_ = nullptr, bRefresh_ = nullptr, bAll_ = nullptr, bNone_ = nullptr, bRestore_ = nullptr, bDelete_ = nullptr,
         bEmpty_ = nullptr, note_ = nullptr, none_ = nullptr;
    RecycleBin bin_;
    bool filling_ = false, working_ = false;
    std::vector<std::wstring> keys_;  // identity (folder|name|deleted time) of each listed row, to keep ticks across refreshes
    std::jthread worker_;
    Job doneJob_ = Job::Restore;  // result of the finished job (written by the worker before it posts WM_APP_BIN_DONE)
    size_t doneCount_ = 0;
    bool doneOk_ = true;
};

}  // namespace ss
