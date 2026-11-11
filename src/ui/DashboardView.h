#pragma once
#include <string>
#include <vector>

#include "filesystem/DriveManager.h"
#include "scanner/ScanStats.h"
#include "ui/View.h"

namespace ss {

// Drives, Recycle Bin, and (after a scan) usage by file type plus the biggest folders and files.
class DashboardView : public View {
public:
    void OnCreate() override;
    void Layout(int w, int h) override;
    LRESULT OnMessage(UINT m, WPARAM w, LPARAM l) override;
    void OnShow() override { Refresh(); }
    void Reload() override;
    void OnBusyChanged() override { UpdateButtons(); }
    void DropReferences() override;

private:
    struct Trend {
        int64_t delta = 0;
        int age = 0;
        bool valid = false;
    };
    void Refresh();
    void RefreshStats();
    void QueryRecycleBin();
    void UpdateButtons();
    void Paint();
    void ScanSelected();
    void ScanFolder();
    void ScanAll();
    void EmptyBin();
    void OpenTop(HWND list);

    std::vector<DriveInfo> drives_;
    std::vector<Trend> trends_;
    std::vector<RECT> cards_;
    int sel_ = -1;
    HWND bScan_ = nullptr, bFolder_ = nullptr, bAll_ = nullptr, bRefresh_ = nullptr, note_ = nullptr, binText_ = nullptr,
         bBin_ = nullptr, listFolders_ = nullptr, listFiles_ = nullptr;
    uint64_t binBytes_ = 0, binItems_ = 0;

    bool dirty_ = true;      // stats need recomputing when the page is next shown
    bool hasStats_ = false;  // a scan exists: show the statistics sections
    ScanStats stats_;
    std::vector<ScanNode*> folderNodes_, fileNodes_;
    // layout results (client coordinates, scroll already applied)
    RECT titleDrives_{}, titleTypes_{}, barRect_{}, titleFolders_{}, titleFiles_{};
    std::vector<RECT> legend_;
    std::vector<int> legendCat_;  // category index per legend rect, largest first
};

}  // namespace ss
