#pragma once
#include <windows.h>
#include <shellapi.h>  // HDROP

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app/Settings.h"
#include "filesystem/ChangeWatcher.h"
#include "operations/FileOperationService.h"
#include "safety/PathProtectionService.h"
#include "scanner/IScanner.h"
#include "scanner/ScanSnapshot.h"
#include "scanner/TreeSync.h"

namespace ss {

class View;
class DashboardView;
class AnalyzerView;
class LargeFilesView;
class TempFilesView;
class SettingsView;
class TreemapView;

enum class DeleteMode { Ask, Recycle, Permanent };

struct DelItem {
    ScanNode* node;
    std::wstring path;
    uint64_t size;
};
struct DelJob {
    bool permanent = false;
    std::vector<DelItem> items;
    DeleteResult result;
};

class MainWindow {
public:
    static int Run(HINSTANCE hi, int nShow);
    ~MainWindow();

    // --- services for the views
    HWND hwnd() const { return hwnd_; }
    int S(int v) const { return MulDiv(v, static_cast<int>(dpi_), 96); }
    HFONT font() const { return font_; }
    HFONT boldFont() const { return bold_; }
    HFONT titleFont() const { return title_; }
    Settings& settings() { return settings_; }
    const PathProtectionService& protection() const { return prot_; }
    ScanResult* scan() const { return scan_.get(); }
    bool busy() const { return scanning_ || deleting_ || loading_; }
    // Header of the remembered scan, while the full tree is still loading (the Dashboard shows its stats meanwhile).
    const SnapshotSummary* SavedSummary() const { return saved_.get(); }
    void ClearSavedScan();                    // deletes lastscan.bin
    std::wstring SavedScanInfo() const;       // "18.2 MB, saved 12.03.2026 10:15" / "No saved scan."
    bool scanning() const { return scanning_; }
    std::wstring EmptyStateText() const;      // "No scan has been performed." / "Scan canceled."
    std::wstring LastScanSummary() const { return summary_; }

    void ApplyFont(HWND root) const;
    void ShowPage(int idx);
    void BrowseTo(ScanNode* node);            // Storage Analyzer at `node`
    void SettingsChanged();                   // persist + reload views
    void DataChanged();                       // reload all views
    void SetStatus(const std::wstring& s);
    void Toast(const std::wstring& text);                  // short confirmation banner (bottom right, fades after a few seconds)

    // "Basket": items collected from Analyzer / Large Files / Treemap, deleted together with one confirmation.
    void AddToBasket(const std::vector<ScanNode*>& nodes);
    void ClearBasket();
    void DeleteBasket();
    size_t BasketSize() const { return basket_.size(); }

    void StartScan(std::vector<std::wstring> roots);
    void StopScan();
    void RefreshFolder(ScanNode* dir);
    bool IsIgnoredPath(const std::wstring& path) const;   // inside (or equal to) an entry of the ignore list
    void IgnoreListChanged();                             // persist + hide newly ignored folders from current results
    void Rescan();                                       // rescan whatever was last scanned
    int PruneMissing(const std::vector<ScanNode*>& nodes); // drop nodes whose file/folder no longer exists; returns how many
    void SyncFolder(ScanNode* dir);                      // re-list one folder's direct contents (new/removed/changed entries)
    void RefreshNodes(const std::vector<ScanNode*>& nodes); // re-check just these items: drop vanished ones, re-list their folders (no rescan)
    void DeleteItems(std::vector<ScanNode*> nodes, DeleteMode mode);

    // Native dialogs (TaskDialog)
    void ShowMessage(const std::wstring& heading, const std::wstring& body, bool warning = false);
    bool Confirm(const std::wstring& heading, const std::wstring& body, const wchar_t* okLabel, bool danger,
                 const wchar_t* footer = nullptr);

private:
    friend struct NavAccess;
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK NavProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK ToastProc(HWND, UINT, WPARAM, LPARAM);
    void UpdateBasketUi();
    void SaveWindowState();
    void OnDropFiles(HDROP drop);
    LRESULT OnMessage(UINT m, WPARAM w, LPARAM l);
    bool OnCreate();
    void MakeFonts();
    void Layout();
    void PaintNav(HWND h);
    void UpdateScanUi();
    void OnScanProgress();
    void OnScanComplete();
    void OnDeleteDone();
    void Graft(ScanNode* target, std::unique_ptr<ScanResult> r);
    void DropReferences();
    void StartDelete(std::vector<DelItem> items, bool permanent);

    // --- remembered scan + automatic updates
    static std::wstring SnapshotPath();
    void LoadSavedScan();
    void OnSnapshotLoaded();
    void SaveScanNow();                       // on exit, if the tree changed since it was saved
    void StartWatcher();
    void StartRecheck();                      // compare folder times with the disk on a worker, then sync what changed
    void OnValidated();
    void SyncPaths(std::vector<std::wstring> paths);
    void PumpSync();
    void OnSyncDone();
    void OnSyncTick();
    void UpdateSummary();
    void FirstRunScan();
    void HideIgnoredInTree();                 // detach folders of the ignore list from the loaded tree

    HWND hwnd_ = nullptr, nav_ = nullptr, status_ = nullptr, stats_ = nullptr, prog_ = nullptr, stop_ = nullptr, details_ = nullptr;
    bool showDetails_ = false;  // "More info" while scanning: show the folder being scanned
    UINT dpi_ = 96;
    HFONT font_ = nullptr, bold_ = nullptr, title_ = nullptr;
    int page_ = -1;
    bool ResolveDark() const;
    void ApplyTheme();

    Settings settings_;
    PathProtectionService prot_{PathProtectionService::ConfigFromEnvironment()};

    std::unique_ptr<View> pages_[7];
    DashboardView* dash_ = nullptr;
    TreemapView* treemap_ = nullptr;
    HWND toast_ = nullptr, basketText_ = nullptr, bBasketDel_ = nullptr, bBasketClear_ = nullptr;
    std::vector<ScanNode*> basket_;
    AnalyzerView* analyzer_ = nullptr;
    LargeFilesView* large_ = nullptr;
    TempFilesView* temp_ = nullptr;

    std::unique_ptr<ScanResult> scan_;
    std::wstring summary_;
    std::vector<std::wstring> lastRoots_;
    bool lastCanceled_ = false;
    ScanNode* refreshTarget_ = nullptr;
    std::atomic<bool> scanning_{false}, deleting_{false}, loading_{false}, syncing_{false};
    std::mutex shared_;  // guards progress_ + pending_ + the hand-over slots below
    ScanProgress progress_;
    std::unique_ptr<ScanResult> pending_;
    std::shared_ptr<DelJob> delJob_;

    std::unique_ptr<SnapshotSummary> saved_;
    std::unique_ptr<ScanResult> loaded_;                   // hand-over slot: load thread -> UI
    std::vector<std::wstring> validated_;                  // hand-over slot: re-check thread -> UI
    struct SyncResult {
        std::wstring path;
        std::vector<DirEntry> list;
        ListStatus st = ListStatus::Ok;
        uint64_t modified = 0;  // the folder's own time, read before listing
    };
    std::vector<SyncResult> syncOut_;                      // hand-over slot: listing thread -> UI
    std::vector<std::wstring> syncQueue_, syncJob_;        // UI thread only
    uint64_t gen_ = 0, syncGen_ = 0;                       // bumped whenever scan_ is replaced: late results are dropped
    int syncHold_ = 0;                                     // >0 while a dialog holds node pointers (no tree edits then)
    bool treeDirty_ = false, cleared_ = false, rememberOn_ = true;
    uint64_t dataFt_ = 0, updateFt_ = 0;                   // when the data was scanned / last confirmed current (FILETIME)
    ChangeWatcher watcher_;

    // Declared last: destroyed (stopped + joined) first.
    std::jthread scanThread_;
    std::jthread delThread_;
    std::jthread loadThread_;
    std::jthread valThread_;
    std::jthread syncThread_;
};

}  // namespace ss
