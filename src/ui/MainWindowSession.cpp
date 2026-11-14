// MainWindow, part 2: the remembered scan (load / save), the start-up re-check, live updates from the change watcher,
// re-listing single folders, and the automatic first scan. The window, scan and delete code stays in MainWindow.cpp.
#include "ui/MainWindow.h"

#include <commctrl.h>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "filesystem/DriveManager.h"
#include "filesystem/PathUtils.h"
#include "scanner/DirectoryScanner.h"
#include "ui/AnalyzerView.h"
#include "ui/DashboardView.h"
#include "ui/LargeFilesView.h"
#include "ui/TempFilesView.h"
#include "ui/TreemapView.h"
#include "ui/UiCommon.h"
#include "utils/FormatUtils.h"
#include "utils/Logger.h"

namespace ss {

// ------------------------------------------------------------------ remembered scan + automatic updates

std::wstring MainWindow::SnapshotPath() { return Settings::AppDataDir() + L"\\lastscan.bin"; }

std::wstring MainWindow::SavedScanInfo() const {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(SnapshotPath().c_str(), GetFileExInfoStandard, &d)) return L"No saved scan.";
    uint64_t sz = (static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
    uint64_t ft = (static_cast<uint64_t>(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
    return L"Saved scan: " + FormatBytes(sz) + L", from " + FormatFileTime(ft);
}

void MainWindow::ClearSavedScan() {
    DeleteFileW(SnapshotPath().c_str());
    saved_.reset();
    cleared_ = true;  // not written again on exit; the next scan saves a new one
    DataChanged();
    Toast(L"Saved scan cleared");
}

void MainWindow::UpdateSummary() {
    if (!scan_ || !scan_->root) return;
    const ScanNode* r = scan_->root.get();
    scan_->files = r->files;
    scan_->dirs = r->dirs;
    scan_->bytes = r->size;
    std::wstring name = r->type == NodeType::Virtual ? L"All local drives" : BuildPath(r);
    summary_ = L"Last scan: " + name + L" - " + FormatCount(r->files) + L" files, " + FormatCount(r->dirs) + L" folders, " + FormatBytes(r->size);
    if (dataFt_) summary_ += L"  -  scanned " + FormatFileTime(dataFt_);
    if (settings_.autoUpdate && updateFt_) summary_ += L", checked " + FormatAge(updateFt_);
}

void MainWindow::LoadSavedScan() {
    lastRoots_ = saved_->roots;
    dataFt_ = saved_->created;
    summary_ = L"Saved scan from " + FormatFileTime(saved_->created) + L": " + FormatCount(saved_->files) + L" files, " +
               FormatCount(saved_->dirs) + L" folders, " + FormatBytes(saved_->bytes);
    SetStatus(L"Loading the saved scan from " + FormatFileTime(saved_->created) + L"...");
    loading_ = true;
    UpdateScanUi();
    DataChanged();  // the Dashboard shows the saved statistics right away
    loadThread_ = std::jthread([this](std::stop_token) {
        auto r = LoadSnapshot(SnapshotPath());
        {
            std::lock_guard lk(shared_);
            loaded_ = std::move(r);
        }
        PostMessageW(hwnd_, WM_APP_SNAPSHOT_LOADED, 0, 0);
    });
}

void MainWindow::HideIgnoredInTree() {
    if (!scan_ || settings_.ignored.empty()) return;
    DetachMatching(scan_->root.get(), [&](const std::wstring& p) {
        for (const auto& ig : settings_.ignored)
            if (path::EqualsNoCase(p, ig)) return true;
        return false;
    });
}

void MainWindow::OnSnapshotLoaded() {
    if (loadThread_.joinable()) loadThread_.join();
    loading_ = false;
    std::unique_ptr<ScanResult> r;
    {
        std::lock_guard lk(shared_);
        r = std::move(loaded_);
    }
    if (!r) {
        DeleteFileW(SnapshotPath().c_str());  // unreadable or from another version: scan again
        saved_.reset();
        summary_.clear();
        lastRoots_.clear();
        SetStatus(L"The saved scan could not be read and was discarded.");
        UpdateScanUi();
        DataChanged();
        return;
    }
    uint64_t hash = saved_ ? saved_->ignoredHash : 0;
    saved_.reset();
    scan_ = std::move(r);
    ++gen_;
    if (hash != HashIgnored(settings_.ignored)) {
        HideIgnoredInTree();
        treeDirty_ = true;
    }
    updateFt_ = 0;
    UpdateSummary();
    UpdateScanUi();
    DataChanged();
    StartWatcher();
    uint32_t today = static_cast<uint32_t>(NowFileTime() / kFileTimeDay);
    if (settings_.autoUpdate && settings_.lastFullScanDay && today > settings_.lastFullScanDay + 7) {
        SetStatus(L"The saved scan is over a week old: scanning again...");
        StartScan(lastRoots_);
    } else if (settings_.autoUpdate) {
        StartRecheck();
    } else {
        SetStatus(L"Saved scan from " + FormatFileTime(dataFt_) + L" (automatic updates are off).");
    }
}

void MainWindow::SaveScanNow() {
    if (!settings_.rememberScan || cleared_ || !scan_ || !treeDirty_ || scanning_) return;
    SetStatus(L"Saving the scan...");
    UpdateWindow(status_);
    ScanStats st = ComputeStats(scan_->root.get(), settings_.showHidden, false, 10, settings_.showAppData);
    bool ok = SaveSnapshot(*scan_, lastRoots_, HashIgnored(settings_.ignored), st, SnapshotPath());
    Logger::Get().Info(L"scan saved on exit: ok=%d", ok);
    treeDirty_ = false;
}

void MainWindow::StartWatcher() {
    watcher_.Stop();
    if (!settings_.autoUpdate || !scan_ || lastRoots_.empty()) return;
    std::vector<std::wstring> roots;
    for (const auto& r : lastRoots_) {
        std::wstring c = path::Canonicalize(r);
        if (!c.empty()) roots.push_back(c);
    }
    watcher_.Start(roots, settings_.ignored, [] {});  // polled by the timer
}

void MainWindow::StartRecheck() {
    if (!scan_ || !settings_.autoUpdate || busy()) return;
    if (valThread_.joinable()) {
        valThread_.request_stop();
        valThread_.join();
    }
    std::vector<DirCheck> checks;
    CollectDirChecks(scan_->root.get(), checks);
    SetStatus(L"Checking for changes since the last scan...");
    valThread_ = std::jthread([this, checks = std::move(checks)](std::stop_token st) {
        auto changed = ValidateDirs(checks, 4, st);
        if (st.stop_requested()) return;
        {
            std::lock_guard lk(shared_);
            validated_ = std::move(changed);
        }
        PostMessageW(hwnd_, WM_APP_VALIDATED, 0, 0);
    });
}

void MainWindow::OnValidated() {
    if (valThread_.joinable()) valThread_.join();
    std::vector<std::wstring> v;
    {
        std::lock_guard lk(shared_);
        v = std::move(validated_);
    }
    updateFt_ = NowFileTime();
    if (v.empty()) {
        UpdateSummary();
        SetStatus(L"Up to date (scan from " + FormatFileTime(dataFt_) + L").");
        return;
    }
    SetStatus(Plural(v.size(), L"folder", L"folders") + L" changed since the last scan: updating...");
    SyncPaths(std::move(v));
}

void MainWindow::SyncPaths(std::vector<std::wstring> paths) {
    for (auto& p : paths) syncQueue_.push_back(std::move(p));
    PumpSync();
}

void MainWindow::PumpSync() {
    if (syncing_ || syncQueue_.empty() || !scan_ || busy() || syncHold_) return;
    size_t n = std::min<size_t>(syncQueue_.size(), 200);
    syncJob_.assign(std::make_move_iterator(syncQueue_.begin()), std::make_move_iterator(syncQueue_.begin() + static_cast<ptrdiff_t>(n)));
    syncQueue_.erase(syncQueue_.begin(), syncQueue_.begin() + static_cast<ptrdiff_t>(n));
    syncing_ = true;
    syncGen_ = gen_;
    std::vector<std::wstring> ignored;  // the scanner skips these folders, so a re-listing must too
    for (const auto& s : settings_.ignored) {
        std::wstring c = path::Canonicalize(s);
        if (!c.empty()) ignored.push_back(std::move(c));
    }
    syncThread_ = std::jthread([this, jobs = syncJob_, ignored = std::move(ignored)](std::stop_token) {
        std::vector<SyncResult> out;
        std::unordered_map<std::wstring, uint32_t> cluster;  // per drive
        for (const auto& p : jobs) {
            std::wstring key = p.substr(0, 3);
            auto it = cluster.find(key);
            if (it == cluster.end()) it = cluster.emplace(key, ClusterSizeOf(p)).first;
            SyncResult r;
            r.path = p;
            WIN32_FILE_ATTRIBUTE_DATA fa;
            if (GetFileAttributesExW(path::ToExtended(p).c_str(), GetFileExInfoStandard, &fa))
                r.modified = (static_cast<uint64_t>(fa.ftLastWriteTime.dwHighDateTime) << 32) | fa.ftLastWriteTime.dwLowDateTime;
            r.st = ListDirectory(p, it->second, r.list);
            RemoveIgnoredDirs(p, ignored, r.list);
            out.push_back(std::move(r));
        }
        {
            std::lock_guard lk(shared_);
            syncOut_ = std::move(out);
        }
        PostMessageW(hwnd_, WM_APP_SYNC_DONE, 0, 0);
    });
}

void MainWindow::OnSyncDone() {
    if (syncThread_.joinable()) syncThread_.join();
    syncing_ = false;
    std::vector<SyncResult> out;
    {
        std::lock_guard lk(shared_);
        out = std::move(syncOut_);
    }
    std::vector<std::wstring> paths = std::move(syncJob_);
    if (!scan_ || syncGen_ != gen_) {  // the tree was replaced meanwhile: these listings are for the old one
        PumpSync();
        return;
    }
    if (busy() || syncHold_) {  // a delete or dialog holds node pointers: try again on a later tick
        syncQueue_.insert(syncQueue_.begin(), paths.begin(), paths.end());
        return;
    }
    ScanNode* root = scan_->root.get();
    bool structural = false;
    for (const auto& r : out) {
        ScanNode* n = FindNodeByPath(root, r.path);
        if (n && n->type == NodeType::Directory && r.st == ListStatus::Ok && NeedsStructuralChange(n, r.list)) {
            structural = true;
            break;
        }
    }
    if (structural) DropReferences();  // child vectors get rebuilt: views must not keep node pointers (they restore their folder by path)
    SyncStats total;
    for (const auto& r : out) {
        ScanNode* n = FindNodeByPath(root, r.path);  // looked up again: an earlier listing may have rebuilt this node's parent
        if (!n || n->type != NodeType::Directory) continue;
        SyncStats s = ApplyListing(n, r.list, r.st, r.modified);
        total.added += s.added;
        total.removed += s.removed;
        total.updated += s.updated;
        for (auto& d : s.newDirs) syncQueue_.push_back(std::move(d));
    }
    updateFt_ = NowFileTime();
    if (total.Changed() || structural) {
        treeDirty_ = true;
        UpdateSummary();
        DataChanged();
        SetStatus(L"Auto-updated " + FormatFileTime(updateFt_));
    }
    PumpSync();
}

void MainWindow::OnSyncTick() {
    if (!settings_.autoUpdate || !scan_ || busy() || syncHold_) return;
    if (watcher_.TakeOverflow()) {  // events were lost: do not guess, compare everything with the disk
        StartRecheck();
        return;
    }
    if (syncing_) return;
    auto dirty = watcher_.TakeDirty(100);
    if (!dirty.empty()) SyncPaths(std::move(dirty));
    else PumpSync();
}

void MainWindow::FirstRunScan() {
    std::vector<std::wstring> roots;
    for (const auto& d : DriveManager::Enumerate())
        if (d.IsFixed() && d.ready) roots.push_back(d.root);
    settings_.firstRunDone = true;
    settings_.Save();
    if (roots.empty()) return;
    StartScan(roots);
    if (scanning_) Toast(L"First launch: scanning all local drives once. You can stop it at any time.");
}

int MainWindow::PruneMissing(const std::vector<ScanNode*>& nodes) {
    int n = DetachMissing(nodes);
    if (n) DataChanged();
    return n;
}

void MainWindow::SyncFolder(ScanNode* dir) {
    if (busy() || !scan_ || !dir) return;
    SyncPaths({BuildPath(dir)});
    Toast(L"Folder refreshed");
}

// Light "Refresh": the listed items are verified against the disk. Vanished ones are dropped, and each distinct parent
// folder is listed once (shallow) so changed sizes and new files show up. No subtree rescan.
void MainWindow::RefreshNodes(const std::vector<ScanNode*>& nodes) {
    if (busy() || !scan_) return;
    std::vector<std::wstring> parents;
    bool capped = false;
    {
        std::unordered_set<std::wstring> seen;
        for (const ScanNode* n : nodes) {
            if (n->removed) continue;
            std::wstring p = path::Parent(BuildPath(n));
            if (!p.empty() && seen.insert(path::Canonicalize(p)).second) parents.push_back(std::move(p));
            if (parents.size() >= 3000) {  // each is one directory listing
                capped = true;
                break;
            }
        }
    }
    // One disk call per row: fine for a few thousand rows, a visible freeze for hundreds of thousands. The folder re-listing
    // below removes vanished files on a worker thread anyway, so big lists skip this quick pass.
    int gone = nodes.size() <= 5000 ? PruneMissing(nodes) : 0;
    size_t folders = parents.size();
    SyncPaths(std::move(parents));  // vanished files, changed sizes and new files are applied when the listings arrive
    std::wstring msg = L"Refreshing " + FormatCount(folders) + (folders == 1 ? L" folder..." : L" folders...");
    if (gone) msg = Plural(static_cast<uint64_t>(gone), L"missing item", L"missing items") + L" removed. " + msg;
    if (capped) msg += L" (the rest is kept current by the live watcher)";
    Toast(msg);
}

}  // namespace ss
