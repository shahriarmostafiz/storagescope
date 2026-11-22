#include "ui/DuplicatesView.h"

#include <commctrl.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <unordered_map>

#include "filesystem/DriveManager.h"
#include "filesystem/PathUtils.h"
#include "operations/ExplorerService.h"
#include "scanner/FileTypes.h"
#include "ui/MainWindow.h"
#include "ui/UiCommon.h"
#include "utils/FormatUtils.h"

namespace ss {
namespace {

enum { ID_SIZE = 1, ID_TYPE, ID_FIND, ID_LIST, ID_INFO, ID_EMPTY, ID_OPEN, ID_KEEP, ID_RECYCLE, ID_PERM, ID_NOTE, ID_SELALL };
enum Col { C_NAME, C_SIZE, C_GROUP, C_MOD, C_LOC, C_DUP, C_COUNT };
constexpr UINT_PTR kTimer = 1;

}  // namespace

void DuplicatesView::OnCreate() {
    lblSize_ = Make(L"STATIC", L"Minimum size:", SS_LEFT | SS_CENTERIMAGE, 0);
    cbSize_ = Make(L"SSCombo", L"Minimum size", WS_TABSTOP, ID_SIZE);
    for (const wchar_t* s : {L"100 KB", L"1 MB", L"10 MB", L"100 MB"}) SendMessageW(cbSize_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
    SendMessageW(cbSize_, CB_SETCURSEL, 1, 0);
    lblType_ = Make(L"STATIC", L"Type:", SS_LEFT | SS_CENTERIMAGE, 0);
    cbType_ = Make(L"SSCombo", L"File type", WS_TABSTOP, ID_TYPE);
    SendMessageW(cbType_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"All types"));
    for (int i = 0; i < kCategoryCount; ++i)
        SendMessageW(cbType_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(CategoryName(static_cast<FileCategory>(i))));
    SendMessageW(cbType_, CB_SETCURSEL, 0, 0);
    bFind_ = Make(L"BUTTON", L"Find Duplicates", WS_TABSTOP | BS_OWNERDRAW, ID_FIND);
    bSelAll_ = Make(L"BUTTON", L"Select all duplicates", WS_TABSTOP | BS_OWNERDRAW, ID_SELALL);

    list_ = Make(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS, ID_LIST);
    SetWindowTheme(list_, L"Explorer", nullptr);
    ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP | LVS_EX_CHECKBOXES);
    ListView_SetCallbackMask(list_, LVIS_STATEIMAGEMASK);
    struct D {
        const wchar_t* t;
        int w, fmt;
    } defs[C_COUNT] = {{L"Name", 240, LVCFMT_LEFT},     {L"Size", 90, LVCFMT_RIGHT},        {L"Group", 60, LVCFMT_LEFT},
                       {L"Modified", 140, LVCFMT_LEFT}, {L"Location", 300, LVCFMT_LEFT},    {L"Duplicate of", 340, LVCFMT_LEFT}};
    for (int i = 0; i < C_COUNT; ++i) {
        LVCOLUMNW c{LVCF_TEXT | LVCF_WIDTH | LVCF_FMT};
        c.pszText = const_cast<wchar_t*>(defs[i].t);
        c.cx = mw_->S(defs[i].w * 118 / 100);
        c.fmt = defs[i].fmt;
        SendMessageW(list_, LVM_INSERTCOLUMNW, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(&c));
    }
    info_ = Make(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX, ID_INFO);
    empty_ = Make(L"STATIC", L"", SS_CENTER | SS_NOPREFIX, ID_EMPTY);
    bOpen_ = Make(L"BUTTON", L"Open File Location", WS_TABSTOP | BS_OWNERDRAW, ID_OPEN);
    bKeep_ = Make(L"BUTTON", L"Keep This One", WS_TABSTOP | BS_OWNERDRAW, ID_KEEP);
    bRecycle_ = Make(L"BUTTON", L"Move to Recycle Bin", WS_TABSTOP | BS_OWNERDRAW, ID_RECYCLE);
    bPerm_ = Make(L"BUTTON", L"Delete Permanently", WS_TABSTOP | BS_OWNERDRAW, ID_PERM);
    note_ = Make(L"SSInfo",
                 L"Two files are duplicates only when their contents are exactly the same; names and dates do not matter.\n\n"
                 L"In each group the oldest file is the original and is kept. The others are ticked, and each one says which file it is a copy of. "
                 L"Pick another original with \"Keep This One\". The original of a group can never be ticked, so one copy always stays.\n\n"
                 L"Only files already in the scan are checked. Windows and installed programs (Program Files) and the insides of .git, .svn and .hg folders are never listed. "
                 L"Two names for the same file (a hard link) look like copies, but removing one frees no space.",
                 WS_TABSTOP, ID_NOTE);
    emptyText_ = L"Press Find Duplicates to look for files with identical content in your scan.";
    Fill();
}

void DuplicatesView::Layout(int w, int h) {
    lastW_ = w;
    lastH_ = h;
    int m = mw_->S(12), gap = mw_->S(8), ch = mw_->S(34), bh = mw_->S(32);
    int x = m, y = mw_->S(8);
    struct It {
        HWND h;
        int w;
    };
    auto group = [&](std::initializer_list<It> items) {  // a label never ends up alone at the end of a row
        int total = 0;
        for (auto& it : items) total += mw_->S(it.w) + gap;
        if (x + total - gap > w - m && x > m) {
            x = m;
            y += ch + gap;
        }
        for (auto& it : items) {
            MoveWindow(it.h, x, y, mw_->S(it.w), ch, TRUE);
            x += mw_->S(it.w) + gap;
        }
        x += mw_->S(14);
    };
    group({{lblSize_, 96}, {cbSize_, 110}});
    group({{lblType_, 44}, {cbType_, 150}});
    group({{bFind_, 170}});
    y += ch + gap;
    int sw = mw_->S(190);  // "Select all duplicates" sits at the right end of the result line
    MoveWindow(info_, m, y, std::max(mw_->S(100), w - 2 * m - sw - gap), mw_->S(24), TRUE);
    MoveWindow(bSelAll_, w - m - sw, y - mw_->S(2), sw, mw_->S(28), TRUE);
    int top = y + mw_->S(32);
    const FlowItem btns[] = {{bOpen_, mw_->S(170)}, {bKeep_, mw_->S(150)}, {bRecycle_, mw_->S(190)}, {bPerm_, mw_->S(180)}};
    int rows = FlowButtons(btns, 4, m, w, 0, bh, gap, false, false);
    int barH = barShown_ ? rows * (bh + gap) + gap : 0;
    int bottom = h - mw_->S(34) - barH;
    MoveWindow(list_, 0, top, w, std::max(0, bottom - top), TRUE);
    static const int kCols[C_COUNT] = {240, 90, 60, 150, 300, 340};
    FitColumns(list_, kCols, C_COUNT, w);
    MoveWindow(note_, m, bottom + mw_->S(6), mw_->S(kInfoIconSize), mw_->S(kInfoIconSize), TRUE);
    FlowButtons(btns, 4, m, w, h - barH + gap, bh, gap, true, barShown_);
    int ew = std::min(w - 2 * m, mw_->S(560));
    MoveWindow(empty_, (w - ew) / 2, top + mw_->S(60), ew, mw_->S(64), TRUE);
}

uint64_t DuplicatesView::MinSize() const {
    static const uint64_t kb = 1024;
    static const uint64_t v[] = {100 * kb, 1024 * kb, 10 * 1024 * kb, 100 * 1024 * kb};
    LRESULT s = SendMessageW(cbSize_, CB_GETCURSEL, 0, 0);
    return v[s >= 0 && s < 4 ? s : 1];
}

void DuplicatesView::OnShow() {
    if (stale_) Prune();
    UpdateButtons();
}

void DuplicatesView::Reload() {
    stale_ = true;
    if (IsWindowVisible(hwnd_)) Prune();
    else UpdateButtons();
}

void DuplicatesView::Prune() {  // forget files that no longer exist in the scan; a group with fewer than two files is no longer a group
    stale_ = false;
    if (rev_ == mw_->treeRev() || groups_.empty()) {
        rev_ = mw_->treeRev();
        if (groups_.empty()) Fill();  // the message depends on whether a scan exists now
        else UpdateButtons();
        return;
    }
    rev_ = mw_->treeRev();
    ScanResult* sc = mw_->scan();
    size_t n = 0;
    for (auto& g : groups_) n += g.items.size();
    if (!sc || !sc->root) {
        groups_.clear();
        checked_.clear();
        emptyText_ = mw_->EmptyStateText();
    } else if (n <= 20000) {  // a lookup per file; beyond this the list is left alone until the next search
        for (auto& g : groups_)
            g.items.erase(std::remove_if(g.items.begin(), g.items.end(),
                                         [&](const Item& it) {
                                             ScanNode* node = FindNodeByPath(sc->root.get(), it.path);
                                             return !node || node->removed;
                                         }),
                          g.items.end());
        groups_.erase(std::remove_if(groups_.begin(), groups_.end(), [](const Group& g) { return g.items.size() < 2; }), groups_.end());
        std::unordered_set<std::wstring> alive;
        for (auto& g : groups_)
            for (size_t i = 1; i < g.items.size(); ++i) alive.insert(g.items[i].path);
        for (auto it = checked_.begin(); it != checked_.end();) it = alive.count(*it) ? std::next(it) : checked_.erase(it);
        if (groups_.empty()) emptyText_ = L"No duplicates are left. Press Find Duplicates to search again.";
    }
    Fill();
}

void DuplicatesView::Fill() {
    rows_.clear();
    for (uint32_t g = 0; g < groups_.size(); ++g)
        for (uint32_t i = 0; i < groups_[g].items.size(); ++i) rows_.push_back({g, i});
    ListView_SetItemCountEx(list_, static_cast<int>(rows_.size()), 0);
    InvalidateRect(list_, nullptr, FALSE);
    bool none = rows_.empty();
    ShowWindow(list_, none ? SW_HIDE : SW_SHOW);
    ShowWindow(empty_, none ? SW_SHOW : SW_HIDE);
    if (none) SetWindowTextW(empty_, job_ ? L"" : mw_->scan() ? emptyText_.c_str() : mw_->EmptyStateText().c_str());
    UpdateButtons();
}

std::wstring DuplicatesView::CellText(const Row& r, int col) const {
    const Item& it = groups_[r.g].items[r.i];
    switch (col) {
        case C_NAME: return path::Leaf(it.path);
        case C_SIZE: return FormatBytes(it.size);
        case C_GROUP: return L"#" + std::to_wstring(r.g + 1);
        case C_MOD: return FormatFileTime(it.modified);
        case C_LOC: return path::Parent(it.path);
        case C_DUP: return IsOriginal(r) ? L"Original - kept" : L"Copy of " + groups_[r.g].items[0].path;
    }
    return L"";
}

std::vector<std::wstring> DuplicatesView::Selected() const {
    std::vector<std::wstring> out;
    if (!checked_.empty())  // ticked copies win; otherwise the highlighted ones
        for (const Row& r : rows_)
            if (!IsOriginal(r) && checked_.count(groups_[r.g].items[r.i].path)) out.push_back(groups_[r.g].items[r.i].path);
    if (!out.empty()) return out;
    for (int i = ListView_GetNextItem(list_, -1, LVNI_SELECTED); i >= 0; i = ListView_GetNextItem(list_, i, LVNI_SELECTED))
        if (i < static_cast<int>(rows_.size()) && !IsOriginal(rows_[static_cast<size_t>(i)]))
            out.push_back(groups_[rows_[static_cast<size_t>(i)].g].items[rows_[static_cast<size_t>(i)].i].path);
    return out;
}

void DuplicatesView::ShowProgress() {
    if (!job_) return;
    int stage = job_->prog.stage;
    uint64_t done = job_->prog.done, total = job_->prog.total;
    std::wstring t = stage == 0 ? L"Sorting " + FormatCount(job_->in.size()) + L" files by size..."
                                : std::wstring(stage == 1 ? L"Step 1 of 2: quick check, " : L"Step 2 of 2: comparing contents, ") +
                                      FormatCount(std::min(done, total)) + L" of " + FormatCount(total) + L" files";
    SetWindowTextW(info_, t.c_str());
}

void DuplicatesView::UpdateButtons() {
    bool running = job_ != nullptr;
    ScanResult* sc = mw_->scan();
    SetWindowTextW(bFind_, running ? L"Stop" : L"Find Duplicates");
    InvalidateRect(bFind_, nullptr, TRUE);
    EnableWindow(bFind_, running || (sc && sc->root && !mw_->busy()));
    EnableWindow(cbSize_, !running);
    EnableWindow(cbType_, !running);
    size_t copies = 0, ticked = 0;
    uint64_t reclaim = 0, pick = 0;
    for (auto& g : groups_) {
        copies += g.items.size() - 1;
        reclaim += g.items[0].size * (g.items.size() - 1);
        for (size_t i = 1; i < g.items.size(); ++i)
            if (checked_.count(g.items[i].path)) {
                ++ticked;
                pick += g.items[i].size;
            }
    }
    bool all = copies > 0 && ticked == copies;
    SetWindowTextW(bSelAll_, all ? L"Clear selection" : L"Select all duplicates");
    EnableWindow(bSelAll_, copies > 0 && !running);
    InvalidateRect(bSelAll_, nullptr, TRUE);
    auto sel = Selected();
    bool ok = !sel.empty() && !mw_->busy() && !running;
    if (barShown_ != !sel.empty()) {  // the action bar is only there while something is selected
        barShown_ = !sel.empty();
        Layout(lastW_, lastH_);
    }
    int hi = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
    bool oneRow = hi >= 0 && hi < static_cast<int>(rows_.size()) && ListView_GetNextItem(list_, hi, LVNI_SELECTED) < 0;
    EnableWindow(bOpen_, oneRow);
    EnableWindow(bKeep_, oneRow && !IsOriginal(rows_[static_cast<size_t>(hi)]) && !running);
    EnableWindow(bRecycle_, ok);
    EnableWindow(bPerm_, ok);
    if (running) return ShowProgress();
    if (groups_.empty()) {
        SetWindowTextW(info_, L"");
        return;
    }
    std::wstring t = Plural(groups_.size(), L"group", L"groups") + L", " + Plural(copies, L"duplicate file", L"duplicate files") + L", " +
                     FormatBytes(reclaim) + L" can be freed";
    if (ticked) t += L"   -   " + FormatCount(ticked) + L" selected (" + FormatBytes(pick) + L")";
    SetWindowTextW(info_, t.c_str());
}

void DuplicatesView::ToggleAll() {
    size_t copies = 0;
    for (auto& g : groups_) copies += g.items.size() - 1;
    bool all = copies > 0 && checked_.size() == copies;
    checked_.clear();
    if (!all)
        for (auto& g : groups_)
            for (size_t i = 1; i < g.items.size(); ++i) checked_.insert(g.items[i].path);
    InvalidateRect(list_, nullptr, FALSE);
    UpdateButtons();
}

void DuplicatesView::KeepThis() {  // make the highlighted copy the original of its group
    int hi = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
    if (hi < 0 || hi >= static_cast<int>(rows_.size()) || IsOriginal(rows_[static_cast<size_t>(hi)])) return;
    Row r = rows_[static_cast<size_t>(hi)];
    auto& items = groups_[r.g].items;
    std::wstring oldKeep = items[0].path;
    checked_.erase(items[r.i].path);
    checked_.insert(oldKeep);  // the file that used to be kept is now a copy
    std::rotate(items.begin(), items.begin() + r.i, items.begin() + r.i + 1);
    Fill();
    int first = hi - static_cast<int>(r.i);  // the group's first row now holds the new original: highlight it
    ListView_SetItemState(list_, -1, 0, LVIS_SELECTED);
    ListView_SetItemState(list_, first, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
}

void DuplicatesView::StartFind() {
    ScanResult* sc = mw_->scan();
    if (!sc || !sc->root || mw_->busy() || job_) return;
    auto job = std::make_shared<Job>();
    job->id = ++jobSeq_;
    job->minSize = MinSize();
    int cat = static_cast<int>(SendMessageW(cbType_, CB_GETCURSEL, 0, 0));  // 0 = all, else FileCategory + 1
    // Same visibility rules as Large Files: no system files, no hidden files unless asked for, no app data, no Windows / Program Files.
    auto blocked = [&](const ScanNode& c) {
        if (c.type != NodeType::Directory || !(c.scanRoot || (c.parent && c.parent->scanRoot))) return false;
        std::wstring p = BuildPath(&c);
        return !path::IsDriveRoot(p) && mw_->protection().IsSystemLocation(p);
    };
    std::vector<ScanNode*> stack;
    if (!blocked(*sc->root)) stack.push_back(sc->root.get());
    while (!stack.empty()) {
        ScanNode* n = stack.back();
        stack.pop_back();
        if (!n->dir) continue;
        for (auto& c : n->dir->children) {
            if (c.removed || (c.attrs & FILE_ATTRIBUTE_SYSTEM)) continue;
            if (!mw_->settings().showHidden && (c.attrs & FILE_ATTRIBUTE_HIDDEN)) continue;
            if (c.type == NodeType::Directory) {
                if ((!mw_->settings().showAppData && IsAppDataRoot(&c)) || blocked(c)) continue;
                if (_wcsicmp(c.name.c_str(), L".git") == 0 || _wcsicmp(c.name.c_str(), L".svn") == 0 || _wcsicmp(c.name.c_str(), L".hg") == 0)
                    continue;  // repository internals are full of look-alike objects, and removing one can corrupt the repository
                stack.push_back(&c);
            } else if (c.type == NodeType::File && c.size >= job->minSize && (cat == 0 || static_cast<int>(Categorize(c.name)) == cat - 1)) {
                job->in.push_back({BuildPath(&c), c.size, c.modified});
            }
        }
    }
    groups_.clear();
    checked_.clear();
    if (job->in.size() < 2) {
        emptyText_ = L"There are not enough files of this kind and size in the scan to have duplicates.";
        return Fill();
    }
    job_ = job;
    Fill();  // clears the old results and shows the progress
    SetTimer(hwnd_, kTimer, 300, nullptr);
    th_ = std::jthread([this, job](std::stop_token st) {
        bool seen[26] = {}, hdd = false;  // one reader per file is much faster than several on a spinning disk
        for (const auto& f : job->in) {
            wchar_t d = f.path.empty() ? 0 : static_cast<wchar_t>(towupper(f.path[0]));
            if (d >= L'A' && d <= L'Z' && !seen[d - L'A']) {
                seen[d - L'A'] = true;
                if (DriveManager::DetectMedia(d) == MediaKind::Hdd) hdd = true;
            }
        }
        int threads = hdd ? 1 : static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 1u, 4u));
        job->out = FindDuplicateGroups(job->in, threads, st, &job->prog);
        if (!st.stop_requested()) PostMessageW(hwnd_, WM_APP_DUP_DONE, 0, static_cast<LPARAM>(job->id));
    });
}

void DuplicatesView::StopFind() {
    if (!job_) return;
    th_.request_stop();
    if (th_.joinable()) th_.join();
    job_.reset();
    KillTimer(hwnd_, kTimer);
    emptyText_ = L"Stopped. Press Find Duplicates to start again.";
    Fill();
}

void DuplicatesView::OnFound() {
    if (th_.joinable()) th_.join();
    KillTimer(hwnd_, kTimer);
    auto job = std::move(job_);
    groups_.clear();
    checked_.clear();
    for (auto& g : job->out) {
        Group grp;
        for (uint32_t i : g) grp.items.push_back({job->in[i].path, job->in[i].size, job->in[i].modified});
        for (size_t i = 1; i < grp.items.size(); ++i) checked_.insert(grp.items[i].path);  // every copy is ticked, the original is not
        groups_.push_back(std::move(grp));
    }
    rev_ = mw_->treeRev();
    emptyText_ = L"No duplicates found among " + Plural(job->in.size(), L"file", L"files") + L" of " + FormatBytes(job->minSize) + L" or more.";
    Fill();
}

void DuplicatesView::Act(int id) {
    if (id == ID_FIND) return job_ ? StopFind() : StartFind();
    if (id == ID_SELALL) return ToggleAll();
    if (id == ID_KEEP) return KeepThis();
    if (id == ID_OPEN) {
        int hi = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
        if (hi >= 0 && hi < static_cast<int>(rows_.size()))
            explorer::Reveal(groups_[rows_[static_cast<size_t>(hi)].g].items[rows_[static_cast<size_t>(hi)].i].path, false);
        return;
    }
    auto sel = Selected();
    ScanResult* sc = mw_->scan();
    if (sel.empty() || !sc || !sc->root) return;
    std::unordered_set<std::wstring> want(sel.begin(), sel.end());
    for (const Row& r : rows_) {  // each group must still have its original on disk, or deleting the copies could remove every file
        if (IsOriginal(r) || !want.count(groups_[r.g].items[r.i].path)) continue;
        const Item& keep = groups_[r.g].items[0];
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (!GetFileAttributesExW(path::ToExtended(keep.path).c_str(), GetFileExInfoStandard, &fa)) {
            mw_->ShowMessage(L"An original file is missing.", L"\"" + keep.path + L"\" is no longer on the disk, so its copies are not removed. Press Find Duplicates again.", true);
            return;
        }
    }
    std::vector<ScanNode*> nodes;
    for (const auto& p : sel)
        if (ScanNode* n = FindNodeByPath(sc->root.get(), p); n && !n->removed && n->type == NodeType::File) nodes.push_back(n);
    if (nodes.empty()) return Prune();
    mw_->DeleteItems(std::move(nodes), id == ID_PERM ? DeleteMode::Permanent : DeleteMode::Recycle);
}

LRESULT DuplicatesView::OnMessage(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_APP_DUP_DONE:
            if (job_ && job_->id == static_cast<uint64_t>(l)) OnFound();
            return 0;
        case WM_TIMER:
            if (w == kTimer) ShowProgress();
            return 0;
        case WM_NOTIFY: {
            auto* nm = reinterpret_cast<NMHDR*>(l);
            if (nm->code == NM_CUSTOMDRAW && nm->hwndFrom == ListView_GetHeader(list_)) return HeaderCustomDraw(l);
            if (nm->hwndFrom != list_) break;
            switch (nm->code) {
                case LVN_GETDISPINFOW: {
                    auto* d = reinterpret_cast<NMLVDISPINFOW*>(l);
                    if (d->item.iItem >= 0 && d->item.iItem < static_cast<int>(rows_.size())) {
                        if (d->item.mask & LVIF_TEXT)
                            wcsncpy_s(d->item.pszText, static_cast<size_t>(d->item.cchTextMax),
                                      CellText(rows_[static_cast<size_t>(d->item.iItem)], d->item.iSubItem).c_str(), _TRUNCATE);
                        if ((d->item.mask & LVIF_STATE) && (d->item.stateMask & LVIS_STATEIMAGEMASK))
                            d->item.state &= ~static_cast<UINT>(LVIS_STATEIMAGEMASK);
                    }
                    break;
                }
                case NM_CUSTOMDRAW: {
                    auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(l);
                    if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                    if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) return CDRF_NOTIFYPOSTPAINT;
                    if (cd->nmcd.dwDrawStage == CDDS_ITEMPOSTPAINT) {
                        int i = static_cast<int>(cd->nmcd.dwItemSpec);
                        if (i >= 0 && i < static_cast<int>(rows_.size()) && !IsOriginal(rows_[static_cast<size_t>(i)]))  // an original has no tick box
                            DrawListTick(list_, cd->nmcd.hdc, i, checked_.count(groups_[rows_[static_cast<size_t>(i)].g].items[rows_[static_cast<size_t>(i)].i].path) != 0);
                    }
                    return CDRF_DODEFAULT;
                }
                case LVN_ODFINDITEMW: return -1;
                case NM_CLICK: {  // a virtual list does not report checkbox clicks: toggle the tick ourselves
                    POINT pt;
                    GetCursorPos(&pt);
                    ScreenToClient(list_, &pt);
                    int hit = HitListTick(list_, pt);
                    if (hit >= 0 && hit < static_cast<int>(rows_.size()) && !IsOriginal(rows_[static_cast<size_t>(hit)])) {
                        const std::wstring& p = groups_[rows_[static_cast<size_t>(hit)].g].items[rows_[static_cast<size_t>(hit)].i].path;
                        if (!checked_.erase(p)) checked_.insert(p);
                        ListView_RedrawItems(list_, hit, hit);
                        UpdateButtons();
                    }
                    break;
                }
                case LVN_ITEMCHANGED:
                case LVN_ODSTATECHANGED: UpdateButtons(); break;
                case NM_DBLCLK: Act(ID_OPEN); break;
                case LVN_KEYDOWN: {
                    auto* k = reinterpret_cast<NMLVKEYDOWN*>(l);
                    if (k->wVKey == VK_DELETE && IsWindowEnabled(bRecycle_)) Act(GetKeyState(VK_SHIFT) < 0 ? ID_PERM : ID_RECYCLE);
                    else if (k->wVKey == VK_RETURN) Act(ID_OPEN);
                    else if (k->wVKey == VK_SPACE) {  // tick / untick the highlighted copies
                        for (int i = ListView_GetNextItem(list_, -1, LVNI_SELECTED); i >= 0; i = ListView_GetNextItem(list_, i, LVNI_SELECTED))
                            if (i < static_cast<int>(rows_.size()) && !IsOriginal(rows_[static_cast<size_t>(i)])) {
                                const std::wstring& p = groups_[rows_[static_cast<size_t>(i)].g].items[rows_[static_cast<size_t>(i)].i].path;
                                if (!checked_.erase(p)) checked_.insert(p);
                            }
                        InvalidateRect(list_, nullptr, FALSE);
                        UpdateButtons();
                    } else if ((k->wVKey == 'A' && GetKeyState(VK_CONTROL) < 0) || k->wVKey == VK_ESCAPE) {
                        checked_.clear();
                        if (k->wVKey == 'A')
                            for (auto& g : groups_)
                                for (size_t i = 1; i < g.items.size(); ++i) checked_.insert(g.items[i].path);
                        InvalidateRect(list_, nullptr, FALSE);
                        UpdateButtons();
                    }
                    break;
                }
            }
            return 0;
        }
        case WM_COMMAND:
            if (HIWORD(w) == BN_CLICKED) Act(LOWORD(w));
            return 0;
    }
    return View::OnMessage(m, w, l);
}

}  // namespace ss
