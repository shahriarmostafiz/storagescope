#include "ui/MainWindow.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "filesystem/DriveManager.h"
#include "filesystem/PathUtils.h"
#include "scanner/DirectoryScanner.h"
#include "ui/AnalyzerView.h"
#include "ui/DashboardView.h"
#include "ui/LargeFilesView.h"
#include "ui/RecycleBinView.h"
#include "ui/SettingsView.h"
#include "ui/TreemapView.h"
#include "ui/TempFilesView.h"
#include "ui/UiCommon.h"
#include "ui/View.h"
#include "utils/FormatUtils.h"
#include "utils/Logger.h"

namespace ss {
namespace {

constexpr int kStopId = 1;
constexpr int kBasketDelId = 2, kBasketClearId = 3, kDetailsId = 4;
const wchar_t* kNavItems[] = {L"Dashboard", L"Storage Analyzer", L"Treemap", L"Large Files", L"Temporary Files", L"Recycle Bin", L"Settings"};
constexpr int kNavCount = 7;

int Td(HWND owner, const std::wstring& main, const std::wstring& content, PCWSTR icon, DWORD common,
       std::vector<TASKDIALOG_BUTTON> btns, int defBtn, std::vector<TASKDIALOG_BUTTON> radios = {}, int defRadio = 0,
       int* radioOut = nullptr, const wchar_t* footer = nullptr, bool links = false) {
    TASKDIALOGCONFIG c{sizeof c};
    c.hwndParent = owner;
    c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | (links ? TDF_USE_COMMAND_LINKS : 0);
    c.dwCommonButtons = common;
    c.pszWindowTitle = L"StorageScope";
    c.pszMainIcon = icon;
    c.pszMainInstruction = main.c_str();
    c.pszContent = content.c_str();
    c.cButtons = static_cast<UINT>(btns.size());
    c.pButtons = btns.empty() ? nullptr : btns.data();
    c.nDefaultButton = defBtn;
    c.cRadioButtons = static_cast<UINT>(radios.size());
    c.pRadioButtons = radios.empty() ? nullptr : radios.data();
    c.nDefaultRadioButton = defRadio;
    c.pszFooter = footer;
    c.pszFooterIcon = footer ? TD_INFORMATION_ICON : nullptr;
    int pressed = IDCANCEL, radio = 0;
    if (FAILED(TaskDialogIndirect(&c, &pressed, &radio, nullptr))) return IDCANCEL;  // fail safe: never "yes"
    if (radioOut) *radioOut = radio;
    return pressed;
}

LRESULT CALLBACK ListHeaderSubclass(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (m == WM_NOTIFY) {
        auto* nm = reinterpret_cast<NMHDR*>(l);
        if (nm->code == NM_CUSTOMDRAW && nm->hwndFrom == ListView_GetHeader(h)) return HeaderCustomDraw(l);
    }
    return DefSubclassProc(h, m, w, l);
}

void FreeInBackground(std::unique_ptr<ScanResult> r) {
    if (r) std::thread([p = std::move(r)]() mutable { p.reset(); }).detach();  // freeing millions of nodes takes a while
}

std::wstring Describe(const std::vector<DelItem>& items, uint64_t total) {
    std::wstring s;
    if (items.size() == 1) {
        s = L"Size:\n" + FormatBytes(total) + L"\nLocation:\n" + path::Parent(path::Canonicalize(items[0].path));
    } else {
        s = L"Total size:\n" + FormatBytes(total);
    }
    return s;
}

}  // namespace

// ------------------------------------------------------------------ lifetime

int MainWindow::Run(HINSTANCE hi, int nShow) {
    MainWindow mw;
    mw.settings_.Load();
    mw.rememberOn_ = mw.settings_.rememberScan;

    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = static_cast<HICON>(LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
    wc.hIconSm = static_cast<HICON>(LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    wc.lpszClassName = L"StorageScopeMain";
    RegisterClassExW(&wc);
    WNDCLASSEXW nc{sizeof nc};
    nc.lpfnWndProc = NavProc;
    nc.hInstance = hi;
    nc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    nc.lpszClassName = L"SSNav";
    RegisterClassExW(&nc);
    WNDCLASSEXW tc{sizeof tc};
    tc.lpfnWndProc = ToastProc;
    tc.hInstance = hi;
    tc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    tc.lpszClassName = L"SSToast";
    RegisterClassExW(&tc);

    UINT sysDpi = GetDpiForSystem();
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = MulDiv(1240, sysDpi, 96), hh = MulDiv(780, sysDpi, 96);
    const Settings& st = mw.settings_;
    if (st.winW > 0) {  // restore the last size/position, but only if it is still on a monitor
        RECT r{st.winX, st.winY, st.winX + st.winW, st.winY + st.winH};
        if (MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) {
            x = st.winX;
            y = st.winY;
            w = st.winW;
            hh = st.winH;
        }
    }
    HWND h = CreateWindowExW(WS_EX_CONTROLPARENT | WS_EX_ACCEPTFILES, L"StorageScopeMain", L"StorageScope",
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, hh, nullptr, nullptr, hi, &mw);
    if (!h) return 1;
    ShowWindow(h, st.maximized ? SW_SHOWMAXIMIZED : nShow);
    UpdateWindow(h);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // Enter/Esc must reach list views and edits untouched; everything else gets Tab/arrow handling.
        bool keys = msg.message == WM_KEYDOWN && (msg.wParam == VK_RETURN || msg.wParam == VK_ESCAPE);
        if (!keys && IsDialogMessageW(h, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

MainWindow::~MainWindow() {
    scanThread_.request_stop();
    valThread_.request_stop();
    watcher_.Stop();
    if (loadThread_.joinable()) loadThread_.join();
    // Process is exiting: skip freeing the (possibly multi-GB) scan tree node by node.
    scan_.release();
    loaded_.release();
    if (font_) DeleteObject(font_);
    if (bold_) DeleteObject(bold_);
    if (title_) DeleteObject(title_);
}

LRESULT CALLBACK MainWindow::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        self->hwnd_ = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->OnMessage(m, w, l) : DefWindowProcW(h, m, w, l);
}

void MainWindow::MakeFonts() {
    NONCLIENTMETRICSW ncm{sizeof ncm};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi_);
    ncm.lfMessageFont.lfHeight = ncm.lfMessageFont.lfHeight * 6 / 5;  // +20%: larger, easier-to-read UI text
    HFONT f = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW b = ncm.lfMessageFont;
    b.lfWeight = FW_SEMIBOLD;
    HFONT fb = CreateFontIndirectW(&b);
    LOGFONTW t = ncm.lfMessageFont;
    t.lfWeight = FW_SEMIBOLD;
    t.lfHeight = t.lfHeight * 4 / 3;
    HFONT ft = CreateFontIndirectW(&t);
    if (font_) DeleteObject(font_);
    if (bold_) DeleteObject(bold_);
    if (title_) DeleteObject(title_);
    font_ = f;
    bold_ = fb;
    title_ = ft;
}

void MainWindow::ApplyFont(HWND root) const {
    EnumChildWindows(
        root,
        [](HWND c, LPARAM f) -> BOOL {
            SendMessageW(c, WM_SETFONT, static_cast<WPARAM>(f), TRUE);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(font_));
}

bool MainWindow::OnCreate() {
    dpi_ = GetDpiForWindow(hwnd_);
    MakeFonts();
    // Mica backdrop for the title bar (Windows 11 22H2+; harmlessly ignored elsewhere). The client area stays opaque.
    int backdrop = 2;  // DWMSBT_MAINWINDOW
    DwmSetWindowAttribute(hwnd_, 38 /*DWMWA_SYSTEMBACKDROP_TYPE*/, &backdrop, sizeof backdrop);
    SetDarkTheme(ResolveDark());
    nav_ = CreateWindowExW(0, L"SSNav", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10, hwnd_, nullptr,
                           GetModuleHandleW(nullptr), this);
    status_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_PATHELLIPSIS | SS_NOPREFIX, 0, 0, 10, 10, hwnd_,
                              nullptr, nullptr, nullptr);
    stats_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_PATHELLIPSIS | SS_NOPREFIX, 0, 0, 10, 10, hwnd_,
                             nullptr, nullptr, nullptr);
    prog_ = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | PBS_MARQUEE | PBS_SMOOTH, 0, 0, 10, 10, hwnd_, nullptr,
                            nullptr, nullptr);
    stop_ = CreateWindowExW(0, L"BUTTON", L"Stop Scan", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_DISABLED | BS_OWNERDRAW, 0, 0,
                            10, 10, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStopId)), nullptr, nullptr);
    details_ = CreateWindowExW(0, L"BUTTON", L"More info", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10, hwnd_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDetailsId)), nullptr, nullptr);
    basketText_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_PATHELLIPSIS | SS_NOPREFIX | SS_CENTERIMAGE, 0, 0, 10, 10, hwnd_,
                                  nullptr, nullptr, nullptr);
    bBasketDel_ = CreateWindowExW(0, L"BUTTON", L"Delete Basket...", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10, hwnd_,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBasketDelId)), nullptr, nullptr);
    bBasketClear_ = CreateWindowExW(0, L"BUTTON", L"Clear", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10, hwnd_,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBasketClearId)), nullptr, nullptr);
    toast_ = CreateWindowExW(0, L"SSToast", L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 10, 10, hwnd_, nullptr, GetModuleHandleW(nullptr), this);
    ApplyFont(hwnd_);

    auto dash = std::make_unique<DashboardView>();
    auto an = std::make_unique<AnalyzerView>();
    auto tm = std::make_unique<TreemapView>();
    auto lf = std::make_unique<LargeFilesView>();
    auto tf = std::make_unique<TempFilesView>();
    auto rb = std::make_unique<RecycleBinView>();
    auto st = std::make_unique<SettingsView>();
    dash_ = dash.get();
    analyzer_ = an.get();
    treemap_ = tm.get();
    large_ = lf.get();
    temp_ = tf.get();
    pages_[0] = std::move(dash);
    pages_[1] = std::move(an);
    pages_[2] = std::move(tm);
    pages_[3] = std::move(lf);
    pages_[4] = std::move(tf);
    pages_[5] = std::move(rb);
    pages_[6] = std::move(st);
    for (auto& p : pages_) p->Create(hwnd_, this);

    SetStatus(L"No scan has been performed.");
    Layout();
    ApplyTheme();
    ShowPage(settings_.lastPage >= 0 && settings_.lastPage < kNavCount ? settings_.lastPage : 0);
    Logger::Get().Info(L"application started");
    SetTimer(hwnd_, 1, 3000, nullptr);  // drives the live updates (OnSyncTick)
    SnapshotSummary probe;
    if (settings_.rememberScan && ReadSnapshotSummary(SnapshotPath(), probe)) {
        saved_ = std::make_unique<SnapshotSummary>(std::move(probe));
        LoadSavedScan();
    } else if (settings_.autoUpdate && !settings_.firstRunDone) {
        FirstRunScan();
    }
    return true;
}

// ------------------------------------------------------------------ remembered scan + automatic updates

namespace {
uint64_t NowFt() {
    FILETIME f;
    GetSystemTimeAsFileTime(&f);
    return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
}
constexpr uint64_t kDay = 864000000000ull;  // FILETIME ticks per day
}  // namespace

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
    uint32_t today = static_cast<uint32_t>(NowFt() / kDay);
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
    ScanStats st = ComputeStats(scan_->root.get(), settings_.showHidden, false, 10);
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
    updateFt_ = NowFt();
    if (v.empty()) {
        UpdateSummary();
        SetStatus(L"Up to date (scan from " + FormatFileTime(dataFt_) + L").");
        return;
    }
    SetStatus(FormatCount(v.size()) + L" folders changed since the last scan: updating...");
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
    updateFt_ = NowFt();
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
    if (scanning_) Toast(L"First launch: scanning all local drives once. You can stop it any time.");
}

void MainWindow::Layout() {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    int w = rc.right, h = rc.bottom, navW = S(210), hdr = S(112), pad = S(16);
    MoveWindow(nav_, 0, 0, navW, h, TRUE);
    int stopW = S(110);
    {   // basket row (shown only while the basket holds items)
        int bw = S(190), cw = S(90), by = S(82), bh = S(26);
        MoveWindow(basketText_, navW + pad, by, std::max(S(100), w - navW - 4 * pad - bw - cw), bh, TRUE);
        MoveWindow(bBasketDel_, w - pad - bw - S(8) - cw, by, bw, bh, TRUE);
        MoveWindow(bBasketClear_, w - pad - cw, by, cw, bh, TRUE);
    }
    MoveWindow(status_, navW + pad, S(12), w - navW - 3 * pad - stopW, S(22), TRUE);
    MoveWindow(prog_, navW + pad, S(42), w - navW - 3 * pad - stopW, S(8), TRUE);
    MoveWindow(stats_, navW + pad, S(58), w - navW - 3 * pad - stopW, S(22), TRUE);
    MoveWindow(stop_, w - pad - stopW, S(12), stopW, S(32), TRUE);
    MoveWindow(details_, w - pad - stopW, S(50), stopW, S(26), TRUE);
    for (auto& p : pages_)
        if (p) MoveWindow(p->hwnd(), navW, hdr, std::max(0, w - navW), std::max(0, h - hdr), TRUE);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void MainWindow::ShowPage(int idx) {
    if (idx < 0 || idx >= kNavCount) return;
    for (int i = 0; i < kNavCount; ++i)
        if (pages_[i]) ShowWindow(pages_[i]->hwnd(), i == idx ? SW_SHOW : SW_HIDE);
    page_ = idx;
    pages_[idx]->OnShow();
    if (nav_) InvalidateRect(nav_, nullptr, FALSE);
}

void MainWindow::BrowseTo(ScanNode* node) {
    ShowPage(1);
    analyzer_->NavigateTo(node);
}

void MainWindow::SetStatus(const std::wstring& s) { SetWindowTextW(status_, s.c_str()); }

// ------------------------------------------------------------------ toast, basket, window state, drag and drop

LRESULT CALLBACK MainWindow::ToastProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            const Theme& t = Colors();
            HBRUSH b = CreateSolidBrush(t.accent);
            HPEN pen = CreatePen(PS_SOLID, 1, t.accent);
            HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, pen);
            FillRect(dc, &rc, BgBrush());  // corners outside the rounded rectangle
            int r = MulDiv(14, static_cast<int>(GetDpiForWindow(h)), 96);
            RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, r, r);
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(b);
            DeleteObject(pen);
            wchar_t text[256];
            GetWindowTextW(h, text, 256);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, t.dark ? RGB(0, 28, 20) : RGB(255, 255, 255));
            HGDIOBJ of = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(h, WM_GETFONT, 0, 0)));
            RECT tr = rc;
            InflateRect(&tr, -MulDiv(16, static_cast<int>(GetDpiForWindow(h)), 96), 0);
            DrawTextW(dc, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            SelectObject(dc, of);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_TIMER:
            KillTimer(h, 1);
            ShowWindow(h, SW_HIDE);
            return 0;
        case WM_LBUTTONDOWN:
            ShowWindow(h, SW_HIDE);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void MainWindow::Toast(const std::wstring& text) {
    if (!toast_) return;
    SetWindowTextW(toast_, text.c_str());
    SendMessageW(toast_, WM_SETFONT, reinterpret_cast<WPARAM>(bold_), FALSE);
    HDC dc = GetDC(toast_);
    HGDIOBJ of = SelectObject(dc, bold_);
    RECT calc{0, 0, 0, 0};
    DrawTextW(dc, text.c_str(), -1, &calc, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, of);
    ReleaseDC(toast_, dc);
    RECT rc;
    GetClientRect(hwnd_, &rc);
    int wd = std::min<int>(calc.right + S(48), rc.right - S(260)), ht = S(44);
    SetWindowPos(toast_, HWND_TOP, rc.right - wd - S(24), rc.bottom - ht - S(24), wd, ht, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    InvalidateRect(toast_, nullptr, TRUE);
    SetTimer(toast_, 1, 4500, nullptr);
}

void MainWindow::UpdateBasketUi() {
    basket_.erase(std::remove_if(basket_.begin(), basket_.end(), [](ScanNode* n) { return n->removed; }), basket_.end());
    std::unordered_set<ScanNode*> in(basket_.begin(), basket_.end());
    uint64_t total = 0;
    for (ScanNode* n : basket_) {  // an item inside another basket item is already counted by its parent
        bool nested = false;
        for (ScanNode* p = n->parent; p && !nested; p = p->parent) nested = in.count(p) != 0;
        if (!nested) total += n->size;
    }
    bool any = !basket_.empty();
    if (any)
        SetWindowTextW(basketText_, (L"Basket: " + FormatCount(basket_.size()) + (basket_.size() == 1 ? L" item, " : L" items, ") +
                                     FormatBytes(total) + L" selected for deletion")
                                        .c_str());
    for (HWND c : {basketText_, bBasketDel_, bBasketClear_}) ShowWindow(c, any ? SW_SHOW : SW_HIDE);
    EnableWindow(bBasketDel_, any && !busy());
}

void MainWindow::AddToBasket(const std::vector<ScanNode*>& nodes) {
    size_t before = basket_.size();
    int skipped = 0;
    for (ScanNode* n : nodes) {
        if (n->removed || std::find(basket_.begin(), basket_.end(), n) != basket_.end()) continue;
        if (prot_.Classify(BuildPath(n)).level >= ProtectionLevel::NoDelete) {  // protected items never enter the basket
            ++skipped;
            continue;
        }
        basket_.push_back(n);
    }
    UpdateBasketUi();
    size_t added = basket_.size() - before;
    if (added) Toast(L"Added " + FormatCount(added) + (added == 1 ? L" item" : L" items") + L" to the basket");
    else if (skipped) Toast(L"Protected items cannot be added to the basket");
}

void MainWindow::ClearBasket() {
    basket_.clear();
    UpdateBasketUi();
}

void MainWindow::DeleteBasket() {
    if (busy() || basket_.empty()) return;
    DeleteItems(basket_, DeleteMode::Ask);  // same protection checks and confirmations as any other delete
}

void MainWindow::SaveWindowState() {
    WINDOWPLACEMENT wp{sizeof wp};
    if (!GetWindowPlacement(hwnd_, &wp)) return;
    settings_.winX = wp.rcNormalPosition.left;
    settings_.winY = wp.rcNormalPosition.top;
    settings_.winW = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
    settings_.winH = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
    settings_.maximized = wp.showCmd == SW_SHOWMAXIMIZED;
    settings_.lastPage = page_ < 0 ? 0 : page_;
    settings_.Save();
}

void MainWindow::OnDropFiles(HDROP drop) {
    std::vector<std::wstring> dirs;
    UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < n; ++i) {
        UINT len = DragQueryFileW(drop, i, nullptr, 0);
        std::wstring p(len, L'\0');
        DragQueryFileW(drop, i, p.data(), len + 1);
        DWORD a = GetFileAttributesW(path::ToExtended(path::Canonicalize(p)).c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) dirs.push_back(p);
    }
    DragFinish(drop);
    if (dirs.empty()) {
        Toast(L"Drop a folder or drive to scan it");
        return;
    }
    if (busy()) {
        Toast(L"Please wait for the current operation to finish");
        return;
    }
    StartScan(dirs);
}

std::wstring MainWindow::EmptyStateText() const { return lastCanceled_ ? L"Scan canceled." : L"No scan has been performed."; }

void MainWindow::DataChanged() {
    UpdateBasketUi();
    for (auto& p : pages_)
        if (p) p->Reload();
}

void MainWindow::SettingsChanged() {
    settings_.Save();
    ApplyTheme();
    if (settings_.rememberScan != rememberOn_) {
        rememberOn_ = settings_.rememberScan;
        if (!rememberOn_) {  // switching it off also removes what was stored
            DeleteFileW(SnapshotPath().c_str());
            cleared_ = true;
        } else {
            cleared_ = false;
            treeDirty_ = true;  // saved again on exit
        }
    }
    if (!settings_.autoUpdate) {
        watcher_.Stop();
    } else if (scan_ && !watcher_.Running() && !busy()) {
        StartWatcher();
        StartRecheck();
    }
    DataChanged();
}

bool MainWindow::ResolveDark() const {
    if (settings_.theme == 2) return true;
    if (settings_.theme == 1) return false;
    DWORD v = 1, sz = sizeof v;  // follow Windows "app mode"
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                 RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v == 0;
}

void MainWindow::ApplyTheme() {
    bool dark = ResolveDark();
    SetDarkTheme(dark);
    BOOL d = dark;
    DwmSetWindowAttribute(hwnd_, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &d, sizeof d);
    COLORREF cap = dark ? RGB(0, 0, 0) : 0xFFFFFFFF;  // 0xFFFFFFFF = DWMWA_COLOR_DEFAULT (restores Mica)
    COLORREF txt = dark ? RGB(255, 255, 255) : 0xFFFFFFFF;
    DwmSetWindowAttribute(hwnd_, 35 /*DWMWA_CAPTION_COLOR*/, &cap, sizeof cap);
    DwmSetWindowAttribute(hwnd_, 36 /*DWMWA_TEXT_COLOR*/, &txt, sizeof txt);
    DwmSetWindowAttribute(hwnd_, 34 /*DWMWA_BORDER_COLOR*/, &cap, sizeof cap);
    // dark context menus (undocumented uxtheme ordinals 135/136, present on Windows 10 1903+; skipped if missing)
    static HMODULE ux = LoadLibraryW(L"uxtheme.dll");
    if (ux) {
        if (auto f = reinterpret_cast<int(WINAPI*)(int)>(GetProcAddress(ux, MAKEINTRESOURCEA(135)))) f(dark ? 2 : 3);
        if (auto g = reinterpret_cast<void(WINAPI*)()>(GetProcAddress(ux, MAKEINTRESOURCEA(136)))) g();
    }
    EnumChildWindows(
        hwnd_,
        [](HWND c, LPARAM l) -> BOOL {
            bool dk = l != 0;
            const Theme& t = Colors();
            wchar_t cls[64];
            GetClassNameW(c, cls, 64);
            if (!wcscmp(cls, L"SysListView32")) {
                SetWindowTheme(c, dk ? L"DarkMode_Explorer" : L"Explorer", nullptr);
                ListView_SetBkColor(c, t.panel);
                ListView_SetTextBkColor(c, t.panel);
                ListView_SetTextColor(c, t.text);
                if (HWND hdr = ListView_GetHeader(c)) SetWindowTheme(hdr, dk ? L"DarkMode_ItemsView" : L"Explorer", nullptr);
                SetWindowSubclass(c, ListHeaderSubclass, 1, 0);  // header custom-draw goes to the list, not to our page
            } else if (!wcscmp(cls, L"SSView")) {
                SetWindowTheme(c, dk ? L"DarkMode_Explorer" : nullptr, nullptr);  // dark scrollbar on scrolling pages
            } else if (!wcscmp(cls, L"ComboBox") || !wcscmp(cls, L"Edit") || !wcscmp(cls, L"ListBox")) {
                SetWindowTheme(c, dk ? L"DarkMode_CFD" : nullptr, nullptr);
            } else if (!wcscmp(cls, L"Button") && (GetWindowLongW(c, GWL_STYLE) & BS_TYPEMASK) != BS_OWNERDRAW) {
                SetWindowTheme(c, dk ? L"DarkMode_Explorer" : nullptr, nullptr);
            }
            return TRUE;
        },
        dark ? 1 : 0);
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_FRAME | RDW_UPDATENOW);
}

void MainWindow::DropReferences() {
    basket_.clear();  // the nodes are about to disappear
    UpdateBasketUi();
    for (auto& p : pages_)
        if (p) p->DropReferences();
}

// ------------------------------------------------------------------ dialogs

void MainWindow::ShowMessage(const std::wstring& heading, const std::wstring& body, bool warning) {
    Td(hwnd_, heading, body, warning ? TD_WARNING_ICON : TD_INFORMATION_ICON, TDCBF_OK_BUTTON, {}, IDOK);
}

bool MainWindow::Confirm(const std::wstring& heading, const std::wstring& body, const wchar_t* okLabel, bool danger,
                         const wchar_t* footer) {
    return Td(hwnd_, heading, body, danger ? TD_ERROR_ICON : TD_WARNING_ICON, TDCBF_CANCEL_BUTTON, {{201, okLabel}},
              danger ? IDCANCEL : 201, {}, 0, nullptr, footer, danger) == 201;
}

// ------------------------------------------------------------------ scanning

void MainWindow::StartScan(std::vector<std::wstring> roots) {
    if (busy() || roots.empty()) return;
    {   // roots that are (inside) an ignored folder or drive are never scanned
        std::vector<std::wstring> kept;
        for (auto& r : roots)
            if (!IsIgnoredPath(r)) kept.push_back(r);
        if (kept.empty()) {
            ShowMessage(L"This location is in your ignore list.",
                        L"Remove it under Settings > Ignored folders and drives if you want to scan it.", false);
            return;
        }
        roots = std::move(kept);
    }
    for (auto& r : roots)
        if (!prot_.CanScan(r)) {
            ShowMessage(L"This location cannot be scanned.", L"The path is not a valid local folder or drive.", true);
            return;
        }
    scanning_ = true;
    lastCanceled_ = false;
    watcher_.Stop();  // a scan reads everything anyway; the watcher restarts when it finishes
    valThread_.request_stop();
    if (!refreshTarget_) lastRoots_ = roots;  // what the "Refresh" button on Large Files rescans
    {
        std::lock_guard lk(shared_);
        progress_ = ScanProgress{};
        progress_.currentPath = L"Starting...";
        pending_.reset();
    }
    Logger::Get().Info(L"scan start: roots=%zu", roots.size());
    unsigned fixedWorkers = settings_.workers;
    // A finished full scan is saved on the scan thread, before the tree is shared with the UI (no locking needed).
    bool save = settings_.rememberScan && !refreshTarget_, hidden = settings_.showHidden;
    uint64_t ignHash = HashIgnored(settings_.ignored);
    scanThread_ = std::jthread([this, roots, fixedWorkers, ignored = settings_.ignored, save, hidden, ignHash](std::stop_token st) {
        DirectoryScanner ds;
        ScanOptions opt;
        opt.workers = fixedWorkers ? fixedWorkers : DriveManager::AutoWorkers(roots);
        opt.ignored = ignored;
        ScanCallbacks cb;
        cb.onProgress = [this](const ScanProgress& p) {
            {
                std::lock_guard lk(shared_);
                progress_ = p;
            }
            PostMessageW(hwnd_, WM_APP_SCAN_PROGRESS, 0, 0);
        };
        cb.onFinished = [this, &roots, save, hidden, ignHash](std::unique_ptr<ScanResult> r) {
            if (save && r && !r->canceled && r->root) {
                ScanStats s = ComputeStats(r->root.get(), hidden, false, 10);
                SaveSnapshot(*r, roots, ignHash, s, SnapshotPath());
            }
            {
                std::lock_guard lk(shared_);
                pending_ = std::move(r);
            }
            PostMessageW(hwnd_, WM_APP_SCAN_COMPLETE, 0, 0);
        };
        ds.Scan(roots, opt, cb, st);
    });
    UpdateScanUi();
    OnScanProgress();
}

void MainWindow::StopScan() {
    if (scanning_) {
        scanThread_.request_stop();
        SetStatus(L"Stopping scan...");
        EnableWindow(stop_, FALSE);
    }
}

void MainWindow::RefreshFolder(ScanNode* dir) {
    if (busy() || !dir || dir->type != NodeType::Directory) return;
    refreshTarget_ = dir;
    StartScan({BuildPath(dir)});
    if (!scanning_) refreshTarget_ = nullptr;
}

void MainWindow::UpdateScanUi() {
    bool s = scanning_;
    EnableWindow(stop_, s);
    ShowWindow(prog_, s ? SW_SHOW : SW_HIDE);
    ShowWindow(details_, s ? SW_SHOW : SW_HIDE);
    if (!s) {
        showDetails_ = false;
        SetWindowTextW(details_, L"More info");
    }
    if (s) SendMessageW(prog_, PBM_SETMARQUEE, TRUE, 30);  // indeterminate: total work is unknown up front
    else SendMessageW(prog_, PBM_SETMARQUEE, FALSE, 0);
    for (auto& p : pages_)
        if (p) p->OnBusyChanged();
}

void MainWindow::OnScanProgress() {
    ScanProgress p;
    {
        std::lock_guard lk(shared_);
        p = progress_;
    }
    if (!scanning_) return;
    SetStatus(showDetails_ ? L"Scanning: " + p.currentPath : L"Scanning...");
    SetWindowTextW(stats_, (L"Files: " + FormatCount(p.files) + L"     Folders: " + FormatCount(p.dirs) +
                            L"     Size analyzed: " + FormatBytes(p.bytes) + L"     Elapsed: " + FormatDuration(p.elapsedMs) +
                            (p.errors ? L"     Inaccessible: " + FormatCount(p.errors) : L""))
                               .c_str());
}

void MainWindow::OnScanComplete() {
    std::unique_ptr<ScanResult> r;
    {
        std::lock_guard lk(shared_);
        r = std::move(pending_);
    }
    if (scanThread_.joinable()) scanThread_.join();
    scanning_ = false;
    ScanNode* target = refreshTarget_;
    refreshTarget_ = nullptr;
    if (!r) {
        UpdateScanUi();
        return;
    }
    Logger::Get().Info(L"scan end: canceled=%d files=%llu dirs=%llu bytes=%llu inaccessible=%llu reparse_skipped=%llu ms=%llu",
                       r->canceled ? 1 : 0, r->files, r->dirs, r->bytes, r->errors, r->skippedReparse, r->elapsedMs);
    std::wstring stats = L"Files: " + FormatCount(r->files) + L"     Folders: " + FormatCount(r->dirs) +
                         L"     Size analyzed: " + FormatBytes(r->bytes) + L"     Elapsed: " + FormatDuration(r->elapsedMs) +
                         (r->errors ? L"     Inaccessible: " + FormatCount(r->errors) : L"");
    if (r->canceled) {
        lastCanceled_ = !scan_;
        SetStatus(L"Scan canceled.");
        SetWindowTextW(stats_, stats.c_str());
        FreeInBackground(std::move(r));
        UpdateScanUi();
        DataChanged();
        StartWatcher();  // the previous results (if any) are still current
        return;
    }
    std::wstring name = r->root->type == NodeType::Virtual ? L"All local drives" : BuildPath(r->root.get());
    SetStatus(L"Scan complete: " + name);
    SetWindowTextW(stats_, stats.c_str());
    DropReferences();
    syncQueue_.clear();
    ++gen_;  // listings still in flight belong to the old tree
    updateFt_ = NowFt();
    if (target) {
        Graft(target, std::move(r));
        treeDirty_ = true;  // the saved file predates this partial refresh: saved on exit
        UpdateSummary();
        UpdateScanUi();
        DataChanged();
        analyzer_->NavigateTo(target);
        StartWatcher();
        return;
    }
    FreeInBackground(std::move(scan_));
    scan_ = std::move(r);
    dataFt_ = updateFt_;
    treeDirty_ = false;  // already saved by the scan thread
    cleared_ = false;
    settings_.lastFullScanDay = static_cast<uint32_t>(dataFt_ / kDay);
    settings_.Save();
    UpdateSummary();
    UpdateScanUi();
    DataChanged();
    BrowseTo(scan_->root.get());
    StartWatcher();
}

// Replaces the subtree below `t` with the freshly scanned one and fixes every ancestor's totals.
void MainWindow::Graft(ScanNode* t, std::unique_ptr<ScanResult> r) {
    ScanNode* nr = r->root.get();
    if (!nr || nr->type != NodeType::Directory || !nr->dir) {
        FreeInBackground(std::move(r));
        return;
    }
    std::unique_ptr<DirData> old = ReplaceSubtree(t, *nr);
    if (old) std::thread([o = std::move(old)]() mutable { o.reset(); }).detach();  // free the replaced nodes off-thread
}

// ------------------------------------------------------------------ deleting

void MainWindow::DeleteItems(std::vector<ScanNode*> nodes, DeleteMode mode) {
    if (busy() || nodes.empty()) return;
    struct Hold {  // dialogs below run a message loop: no automatic tree edits while this function holds node pointers
        int& c;
        explicit Hold(int& x) : c(x) { ++c; }
        ~Hold() { --c; }
    } hold(syncHold_);
    std::unordered_set<ScanNode*> sel(nodes.begin(), nodes.end());
    std::vector<DelItem> items;
    int blocked = 0;
    bool warn = false;
    std::wstring reason;
    uint64_t total = 0;
    for (ScanNode* n : nodes) {
        bool nested = n->removed;
        for (ScanNode* p = n->parent; p && !nested; p = p->parent) nested = sel.count(p) != 0;  // parent already covers it
        if (nested) continue;
        std::wstring path = BuildPath(n);
        ProtectionInfo pi = prot_.Classify(path);  // layer 1 (the service re-checks again before acting)
        if (pi.level >= ProtectionLevel::NoDelete) {
            ++blocked;
            if (reason.empty()) reason = pi.reason;
            continue;
        }
        {   // deleting a folder would also delete an ignored folder inside it, which the user asked to leave alone
            std::wstring canon = path::Canonicalize(path);
            bool holdsIgnored = false;
            for (const auto& ig : settings_.ignored)
                if (!path::EqualsNoCase(ig, canon) && path::IsSameOrUnder(ig, canon)) holdsIgnored = true;
            if (holdsIgnored) {
                ++blocked;
                if (reason.empty())
                    reason = L"This folder contains a folder from your ignore list, and deleting it would delete that too. "
                             L"Remove the entry in Settings first, or delete items inside it instead.";
                continue;
            }
        }
        if (pi.level == ProtectionLevel::Caution) warn = true;
        total += n->size;
        items.push_back({n, path, n->size});
    }
    if (items.empty()) {
        ShowMessage(L"These items cannot be deleted.", reason.empty() ? L"They are protected." : reason, true);
        return;
    }

    std::wstring head = items.size() == 1 ? L"\"" + path::Leaf(items[0].path) + L"\"" : FormatCount(items.size()) + L" items";
    std::wstring extra;
    if (warn) extra += L"\n\nWarning: this location is in AppData/ProgramData and may contain important application data.";
    if (blocked) extra += L"\n\n" + std::to_wstring(blocked) + L" protected item(s) were excluded.";

    bool permanent = false;
    if (mode == DeleteMode::Ask) {
        int radio = 0;
        int r = Td(hwnd_, L"Delete " + head + L"?",
                   Describe(items, total) + L"\n\nChoose how you want to remove " + (items.size() == 1 ? L"this item:" : L"these items:") + extra,
                   TD_WARNING_ICON, TDCBF_CANCEL_BUTTON, {{201, L"Continue"}}, 201,
                   {{101, L"Move to Recycle Bin"}, {102, L"Permanently Delete"}}, 101, &radio);
        if (r != 201) return;
        permanent = radio == 102;
        if (permanent && settings_.requireSecondPermanentConfirm) {
            if (!Confirm(L"Permanently delete " + (items.size() == 1 ? head : FormatCount(items.size()) + L" items") + L"?",
                         head + L"\n" + FormatBytes(total) + L"\n\nThis cannot be undone.", L"Delete Permanently\nThe items do not go to the Recycle Bin", true))
                return;
        }
    } else if (mode == DeleteMode::Recycle) {
        if (settings_.askBeforeRecycle &&
            !Confirm(L"Move " + (items.size() == 1 ? head : FormatCount(items.size()) + L" items") + L" to the Recycle Bin?",
                     Describe(items, total) + extra, L"Move to Recycle Bin", false,
                     L"Items in the Recycle Bin still use disk space until it is emptied."))
            return;
    } else {
        permanent = true;
        if (!Confirm(L"Permanently delete " + (items.size() == 1 ? L"this item" : FormatCount(items.size()) + L" items") + L"?",
                     head + L"\n" + FormatBytes(total) + extra + L"\n\nThis cannot be undone.", L"Delete Permanently\nThe items do not go to the Recycle Bin", true))
            return;
    }
    StartDelete(std::move(items), permanent);
}

void MainWindow::StartDelete(std::vector<DelItem> items, bool permanent) {
    auto job = std::make_shared<DelJob>();
    job->permanent = permanent;
    job->items = std::move(items);
    delJob_ = job;
    deleting_ = true;
    SetStatus(permanent ? L"Deleting..." : L"Moving to the Recycle Bin...");
    UpdateScanUi();
    delThread_ = std::jthread([this, job](std::stop_token) {
        std::vector<std::wstring> paths;
        for (auto& i : job->items) paths.push_back(i.path);
        FileOperationService svc(prot_);  // enforces protection again, independent of the UI
        job->result = job->permanent ? svc.PermanentlyDelete(paths) : svc.MoveToRecycleBin(paths, hwnd_);
        PostMessageW(hwnd_, WM_APP_DELETE_DONE, 0, 0);
    });
}

void MainWindow::OnDeleteDone() {
    if (delThread_.joinable()) delThread_.join();
    deleting_ = false;
    auto job = delJob_;
    if (!job) return;
    size_t ok = 0, gone = 0;
    uint64_t freed = 0;
    std::vector<std::wstring> parents;  // re-listed afterwards so the tree matches the disk, not just the delete result
    for (const auto& it : job->items) parents.push_back(path::Parent(path::Canonicalize(it.path)));
    std::sort(parents.begin(), parents.end());
    parents.erase(std::unique(parents.begin(), parents.end()), parents.end());
    for (size_t i = 0; i < job->items.size() && i < job->result.items.size(); ++i) {
        FileOperationError e = job->result.items[i].error;
        if (e == FileOperationError::None || e == FileOperationError::PathNotFound) {
            DetachFromTree(job->items[i].node);  // update parents / counts without rescanning
            if (e == FileOperationError::None) {
                ++ok;
                freed += job->items[i].size;
            } else {
                ++gone;
            }
        }
    }
    // Trust the disk, not the result code: an item reported as failed may be gone (recycled but reported an odd code),
    // and a folder that was only partly deleted needs a rescan to show its real size.
    std::vector<ScanNode*> leftovers;
    ScanNode* partial = nullptr;
    for (size_t i = 0; i < job->items.size() && i < job->result.items.size(); ++i) {
        FileOperationError e = job->result.items[i].error;
        if (e == FileOperationError::None || e == FileOperationError::PathNotFound) continue;
        leftovers.push_back(job->items[i].node);
    }
    PruneMissing(leftovers);
    for (ScanNode* n : leftovers)
        if (!n->removed && n->type == NodeType::Directory && job->permanent && !partial) partial = n;  // rescan just that folder
    std::wstring failures;
    for (FileOperationError e : {FileOperationError::AccessDenied, FileOperationError::FileInUse, FileOperationError::ProtectedPath,
                                 FileOperationError::DriveUnavailable, FileOperationError::Canceled, FileOperationError::Unknown}) {
        size_t n = job->result.CountOf(e);
        if (n) failures += std::to_wstring(n) + (n == 1 ? L" item" : L" items") + L" could not be deleted because " + ErrorText(e) + L".\n";
    }
    std::wstring done;
    if (ok) {
        done = (job->permanent ? L"Permanently deleted " : L"Moved ") + FormatCount(ok) + (ok == 1 ? L" item (" : L" items (") +
               FormatBytes(freed) + (job->permanent ? L" freed)." : L") to the Recycle Bin. Disk space is only released when the Recycle Bin is emptied.");
    }
    if (gone) done += (done.empty() ? L"" : L" ") + std::to_wstring(gone) + L" item(s) no longer existed and were removed from the list.";
    if (done.empty()) done = L"Nothing was deleted.";
    SetStatus(done);
    DataChanged();
    if (ok && failures.empty()) Toast(job->permanent ? L"Deleted " + FormatCount(ok) + (ok == 1 ? L" item" : L" items") + L" (" + FormatBytes(freed) + L" freed)"
                                                     : L"Moved " + FormatCount(ok) + (ok == 1 ? L" item" : L" items") + L" to the Recycle Bin");
    if (!failures.empty()) ShowMessage(L"Some items could not be deleted.", failures + L"\nThe list is being refreshed.", true);
    delJob_.reset();
    UpdateScanUi();
    dash_->Reload();                       // drive free space changes
    if (partial) RefreshFolder(partial);   // some files of a folder were deleted, others (locked) were not
    treeDirty_ = true;
    if (settings_.autoUpdate) SyncPaths(std::move(parents));
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
    if (gone) msg = FormatCount(static_cast<size_t>(gone)) + L" missing item(s) removed. " + msg;
    if (capped) msg += L" (the rest is kept current by the live watcher)";
    Toast(msg);
}

bool MainWindow::IsIgnoredPath(const std::wstring& p) const {
    std::wstring c = path::Canonicalize(p);
    if (c.empty()) return false;
    for (const auto& ig : settings_.ignored)
        if (path::IsSameOrUnder(c, ig)) return true;
    return false;
}

// Called after the ignore list in Settings changed: save it and hide newly ignored folders from the results already on
// screen (no rescan needed). Removed entries come back with the next scan/refresh.
void MainWindow::IgnoreListChanged() {
    settings_.Save();
    if (!busy() && scan_ && !settings_.ignored.empty()) {
        DropReferences();
        HideIgnoredInTree();
        treeDirty_ = true;
    }
    if (scan_ && !busy()) StartWatcher();  // new ignore list
    DataChanged();
    Toast(settings_.ignored.empty() ? L"Ignore list is empty" : L"Ignore list updated: " + FormatCount(settings_.ignored.size()) + L" entries");
}

void MainWindow::Rescan() {
    if (!busy() && !lastRoots_.empty()) StartScan(lastRoots_);
}

// ------------------------------------------------------------------ window procedures

LRESULT MainWindow::OnMessage(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_CREATE:
            return OnCreate() ? 0 : -1;
        case WM_SIZE:
            if (w != SIZE_MINIMIZED && nav_) Layout();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mi = reinterpret_cast<MINMAXINFO*>(l);
            mi->ptMinTrackSize = {S(920), S(600)};
            return 0;
        }
        case WM_DPICHANGED: {
            dpi_ = HIWORD(w);
            auto* r = reinterpret_cast<RECT*>(l);
            SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            MakeFonts();
            ApplyFont(hwnd_);
            Layout();
            DataChanged();
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (l && settings_.theme == 0 && !wcscmp(reinterpret_cast<const wchar_t*>(l), L"ImmersiveColorSet")) ApplyTheme();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd_, &ps);
            RECT rc;
            GetClientRect(hwnd_, &rc);
            RECT hdr{S(210), 0, rc.right, S(112)};
            FillRect(dc, &hdr, PanelBrush());
            HPEN pen = CreatePen(PS_SOLID, 1, Colors().border);
            HGDIOBJ old = SelectObject(dc, pen);
            MoveToEx(dc, hdr.left, hdr.bottom - 1, nullptr);
            LineTo(dc, hdr.right, hdr.bottom - 1);
            SelectObject(dc, old);
            DeleteObject(pen);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(w);
            SetTextColor(dc, reinterpret_cast<HWND>(l) == stats_ ? Colors().muted : Colors().text);
            SetBkColor(dc, Colors().panel);
            return reinterpret_cast<LRESULT>(PanelBrush());
        }
        case WM_DRAWITEM:
            if (l && reinterpret_cast<DRAWITEMSTRUCT*>(l)->CtlType == ODT_BUTTON) {
                DrawFlatButton(reinterpret_cast<DRAWITEMSTRUCT*>(l));
                return TRUE;
            }
            break;
        case WM_COMMAND:
            if (LOWORD(w) == kStopId) StopScan();
            else if (LOWORD(w) == kDetailsId) {
                showDetails_ = !showDetails_;
                SetWindowTextW(details_, showDetails_ ? L"Less info" : L"More info");
                InvalidateRect(details_, nullptr, TRUE);
                OnScanProgress();
            }
            else if (LOWORD(w) == kBasketDelId) DeleteBasket();
            else if (LOWORD(w) == kBasketClearId) ClearBasket();
            return 0;
        case WM_DROPFILES:
            OnDropFiles(reinterpret_cast<HDROP>(w));
            return 0;
        case WM_SETFOCUS:
            if (nav_) SetFocus(nav_);
            return 0;
        case WM_APP_SCAN_PROGRESS:
            OnScanProgress();
            return 0;
        case WM_APP_SCAN_COMPLETE:
            OnScanComplete();
            return 0;
        case WM_APP_DELETE_DONE:
            OnDeleteDone();
            return 0;
        case WM_APP_SNAPSHOT_LOADED:
            OnSnapshotLoaded();
            return 0;
        case WM_APP_VALIDATED:
            OnValidated();
            return 0;
        case WM_APP_SYNC_DONE:
            OnSyncDone();
            return 0;
        case WM_TIMER:
            if (w == 1) OnSyncTick();
            return 0;
        case WM_CLOSE:
            SaveWindowState();
            if (scanning_) scanThread_.request_stop();
            SaveScanNow();
            DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd_, 1);
            Logger::Get().Info(L"application exit");
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd_, m, w, l);
}

void MainWindow::PaintNav(HWND h) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    {  // soft "glass" sheen: vertical gradient
        auto vtx = [](LONG x, LONG y, COLORREF c) {
            return TRIVERTEX{x, y, static_cast<COLOR16>(GetRValue(c) << 8), static_cast<COLOR16>(GetGValue(c) << 8),
                             static_cast<COLOR16>(GetBValue(c) << 8), 0};
        };
        TRIVERTEX tv[2] = {vtx(0, 0, Colors().navTop), vtx(rc.right, rc.bottom, Colors().navBot)};
        GRADIENT_RECT gr{0, 1};
        GradientFill(mem, tv, 2, &gr, 1, GRADIENT_FILL_RECT_V);
    }
    SetBkMode(mem, TRANSPARENT);

    HGDIOBJ oldFont = SelectObject(mem, title_);
    SetTextColor(mem, Colors().accent);
    RECT t{S(20), S(18), rc.right - S(8), S(60)};
    DrawTextW(mem, L"StorageScope", -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    int itemH = S(42), y0 = S(76);
    for (int i = 0; i < kNavCount; ++i) {
        RECT r{S(8), y0 + i * itemH, rc.right - S(8), y0 + (i + 1) * itemH - S(4)};
        bool sel = i == page_;
        if (sel) {
            HBRUSH b = CreateSolidBrush(Colors().accentSoft);
            FillRect(mem, &r, b);
            DeleteObject(b);
            RECT bar{r.left, r.top + S(8), r.left + S(3), r.bottom - S(8)};
            HBRUSH ab = CreateSolidBrush(Colors().accent);
            FillRect(mem, &bar, ab);
            DeleteObject(ab);
        }
        SelectObject(mem, sel ? bold_ : font_);
        SetTextColor(mem, Colors().text);
        RECT tr{r.left + S(16), r.top, r.right, r.bottom};
        DrawTextW(mem, kNavItems[i], -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        if (sel && GetFocus() == h) {
            RECT fr = r;
            InflateRect(&fr, -1, -1);
            DrawFocusRect(mem, &fr);
        }
    }
    SelectObject(mem, oldFont);
    // right border
    HPEN pen = CreatePen(PS_SOLID, 1, Colors().border);
    HGDIOBJ oldPen = SelectObject(mem, pen);
    MoveToEx(mem, rc.right - 1, 0, nullptr);
    LineTo(mem, rc.right - 1, rc.bottom);
    SelectObject(mem, oldPen);
    DeleteObject(pen);
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(h, &ps);
}

LRESULT CALLBACK MainWindow::NavProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (m) {
        case WM_NCCREATE:
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            if (self) self->PaintNav(h);
            return 0;
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            if (self) {
                int i = (GET_Y_LPARAM(l) - self->S(76)) / self->S(42);
                if (GET_Y_LPARAM(l) >= self->S(76) && i >= 0 && i < kNavCount) self->ShowPage(i);
                SetFocus(h);
            }
            return 0;
        case WM_KEYDOWN:
            if (self && (w == VK_UP || w == VK_DOWN)) {
                int i = self->page_ + (w == VK_DOWN ? 1 : -1);
                self->ShowPage(std::clamp(i, 0, kNavCount - 1));
                return 0;
            }
            break;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace ss
