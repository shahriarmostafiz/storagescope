#include "ui/AnalyzerView.h"

#include <commctrl.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>

#include "filesystem/PathUtils.h"
#include "operations/ExplorerService.h"
#include "ui/MainWindow.h"
#include "ui/UiCommon.h"
#include "utils/FormatUtils.h"

namespace ss {
namespace {

enum {
    ID_LIST = 1, ID_UP, ID_SEARCH, ID_OPEN, ID_MANAGE, ID_DELETE, ID_RECYCLE, ID_PERM, ID_REFRESH, ID_APPS, ID_STATUS, ID_EMPTY,
    ID_SCAN, ID_SORT, ID_DIR, ID_BASKET,
    // context menu
    M_OPEN = 100, M_LOCATION, M_COPY, M_PROPS, M_RECYCLE, M_PERM, M_BASKET
};
constexpr UINT_PTR kSelTimer = 1, kSearchTimer = 2;
constexpr size_t kMaxSearchResults = 20000;
constexpr int kMaxPreCheck = 300;  // classify at most this many selected items to enable/disable buttons

bool ContainsNoCase(const std::wstring& hay, const std::wstring& lowerNeedle) {
    size_t n = hay.size(), m = lowerNeedle.size();
    if (m > n) return false;
    auto low = [](wchar_t c) -> wchar_t {  // ASCII fast path: towlower() is slow and this runs ~100M times on a big scan
        return c < 128 ? (c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c + 32) : c) : static_cast<wchar_t>(towlower(c));
    };
    wchar_t first = lowerNeedle[0];
    for (size_t i = 0; i + m <= n; ++i) {
        if (low(hay[i]) != first) continue;
        size_t j = 1;
        while (j < m && low(hay[i + j]) == lowerNeedle[j]) ++j;
        if (j == m) return true;
    }
    return false;
}

int CmpName(const ScanNode* a, const ScanNode* b) {
    return CompareStringOrdinal(a->name.c_str(), static_cast<int>(a->name.size()), b->name.c_str(),
                                static_cast<int>(b->name.size()), TRUE) - CSTR_EQUAL;
}

std::wstring ExtOf(const ScanNode* n) {
    size_t d = n->name.rfind(L'.');
    return (d == std::wstring::npos || d == 0 || d + 1 >= n->name.size()) ? L"" : n->name.substr(d);
}

template <class T>
int Cmp(T a, T b) { return a < b ? -1 : (a > b ? 1 : 0); }

}  // namespace

// ---------------------------------------------------------------- creation / layout

void AnalyzerView::OnCreate() {
    up_ = Make(L"BUTTON", L"Up", WS_TABSTOP | BS_OWNERDRAW, ID_UP);
    search_ = Make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, ID_SEARCH);  // borderless: the rounded frame is painted by the page
    SendMessageW(search_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Search files and folders"));
    sortCb_ = Make(L"SSCombo", L"Sort by", WS_TABSTOP, ID_SORT);
    for (const wchar_t* s : {L"Sort: Name", L"Sort: Type", L"Sort: Size", L"Sort: Date modified", L"Sort: Files", L"Sort: Folders"})
        SendMessageW(sortCb_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
    dirBtn_ = Make(L"BUTTON", L"Descending", WS_TABSTOP | BS_OWNERDRAW, ID_DIR);
    list_ = Make(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS, ID_LIST);
    SetWindowTheme(list_, L"Explorer", nullptr);
    ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP | LVS_EX_HEADERDRAGDROP);

    SHFILEINFOW sfi{};
    HIMAGELIST il = reinterpret_cast<HIMAGELIST>(SHGetFileInfoW(L"folder", FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof sfi,
                                                                SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES));
    if (il) ListView_SetImageList(list_, il, LVSIL_SMALL);  // shared system list: never destroyed by us
    folderIcon_ = sfi.iIcon;

    struct D {
        const wchar_t* t;
        int w, fmt;
    } defs[C_COUNT] = {{L"Name", 230, LVCFMT_LEFT},   {L"Type", 90, LVCFMT_LEFT},   {L"Size", 85, LVCFMT_RIGHT},
                       {L"On disk", 85, LVCFMT_RIGHT}, {L"% of Parent", 115, LVCFMT_LEFT}, {L"Files", 75, LVCFMT_RIGHT},
                       {L"Folders", 70, LVCFMT_RIGHT}, {L"Last Modified", 135, LVCFMT_LEFT}, {L"Status", 160, LVCFMT_LEFT}};
    for (int i = 0; i < C_COUNT; ++i) {
        LVCOLUMNW c{LVCF_TEXT | LVCF_WIDTH | LVCF_FMT};
        c.pszText = const_cast<wchar_t*>(defs[i].t);
        c.cx = mw_->S(defs[i].w * 118 / 100);
        c.fmt = defs[i].fmt;
        SendMessageW(list_, LVM_INSERTCOLUMNW, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(&c));
    }

    bOpen_ = Make(L"BUTTON", L"Open", WS_TABSTOP | BS_OWNERDRAW, ID_OPEN);
    bManage_ = Make(L"BUTTON", L"Manage", WS_TABSTOP | BS_OWNERDRAW, ID_MANAGE);
    bDelete_ = Make(L"BUTTON", L"Delete...", WS_TABSTOP | BS_OWNERDRAW, ID_DELETE);
    bRecycle_ = Make(L"BUTTON", L"Recycle Selected", WS_TABSTOP | BS_OWNERDRAW, ID_RECYCLE);
    bPerm_ = Make(L"BUTTON", L"Permanent Delete Selected", WS_TABSTOP | BS_OWNERDRAW, ID_PERM);
    bBasket_ = Make(L"BUTTON", L"Add to Basket", WS_TABSTOP | BS_OWNERDRAW, ID_BASKET);
    bRefresh_ = Make(L"BUTTON", L"Refresh", WS_TABSTOP | BS_OWNERDRAW, ID_REFRESH);  // rescans the folder being shown (also F5)
    bApps_ = Make(L"BUTTON", L"Open Installed Apps", WS_TABSTOP | BS_OWNERDRAW, ID_APPS);
    ShowWindow(bApps_, SW_HIDE);
    status_ = Make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_PATHELLIPSIS, ID_STATUS);
    empty_ = Make(L"STATIC", L"", SS_CENTER | SS_NOPREFIX, ID_EMPTY);
    bScan_ = Make(L"BUTTON", L"Scan a Drive", WS_TABSTOP | BS_OWNERDRAW, ID_SCAN);
    UpdateSortArrow();
}

void AnalyzerView::Layout(int w, int h) {
    int m = mw_->S(12), band = mw_->S(46), bh = mw_->S(32);
    band_ = {0, 0, w, band};
    MoveWindow(up_, m, mw_->S(7), mw_->S(64), bh, TRUE);
    MoveWindow(bRefresh_, m + mw_->S(64) + mw_->S(8), mw_->S(7), mw_->S(100), bh, TRUE);
    int sw = mw_->S(250), cbw = mw_->S(170), dw = mw_->S(104);
    int sx = w - m - sw;
    searchFrame_ = {sx, mw_->S(6), sx + sw, mw_->S(6) + mw_->S(34)};
    int eh = mw_->S(24), pad = mw_->S(12);
    MoveWindow(search_, sx + pad, searchFrame_.top + (mw_->S(34) - eh) / 2, sw - 2 * pad, eh, TRUE);
    MoveWindow(dirBtn_, sx - m - dw, mw_->S(7), dw, bh, TRUE);
    MoveWindow(sortCb_, sx - m - dw - m - cbw, mw_->S(9), cbw, mw_->S(240), TRUE);
    crumbRight_ = sx - m - dw - m - cbw - m;
    int listBottom = h - mw_->S(88);
    MoveWindow(list_, 0, band, w, std::max(0, listBottom - band), TRUE);
    MoveWindow(status_, m, listBottom + mw_->S(6), w - 2 * m, mw_->S(22), TRUE);
    int y = h - mw_->S(48), x = m;
    struct B {
        HWND h;
        int w;
    } bs[] = {{bOpen_, 80}, {bManage_, 90}, {bDelete_, 100}, {bRecycle_, 150}, {bPerm_, 210}, {bBasket_, 150}};
    for (auto& b : bs) {
        MoveWindow(b.h, x, y, mw_->S(b.w), bh, TRUE);
        x += mw_->S(b.w) + mw_->S(8);
    }
    MoveWindow(bApps_, x, y, mw_->S(170), bh, TRUE);
    int ew = std::min(w - 2 * m, mw_->S(520));
    MoveWindow(empty_, (w - ew) / 2, band + mw_->S(70), ew, mw_->S(48), TRUE);
    MoveWindow(bScan_, (w - mw_->S(150)) / 2, band + mw_->S(126), mw_->S(150), bh, TRUE);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void AnalyzerView::DropReferences() {
    rows_.clear();
    searchRows_.clear();
    if (cur_ && !cur_->removed) keepPath_ = BuildPath(cur_);  // Rebuild() returns to this folder in the updated tree
    cur_ = nullptr;
    ListView_SetItemCountEx(list_, 0, 0);
}

// ---------------------------------------------------------------- model

bool AnalyzerView::Visible(const ScanNode* n) const {
    const Settings& s = mw_->settings();
    if (!s.showHidden && (n->attrs & FILE_ATTRIBUTE_HIDDEN)) return false;
    if (!s.showSystem && (n->attrs & FILE_ATTRIBUTE_SYSTEM)) return false;
    return true;
}

void AnalyzerView::NavigateTo(ScanNode* n) {
    if (!n) return;
    cur_ = n;
    searchMode_ = false;
    suppressSearch_ = true;
    SetWindowTextW(search_, L"");
    suppressSearch_ = false;
    Rebuild();
    if (!rows_.empty()) ListView_EnsureVisible(list_, 0, FALSE);
    SetFocus(list_);
}

void AnalyzerView::GoUp() {
    if (searchMode_) {
        suppressSearch_ = true;
        SetWindowTextW(search_, L"");
        suppressSearch_ = false;
        searchMode_ = false;
        Rebuild();
    } else if (cur_ && cur_->parent) {
        NavigateTo(cur_->parent);
    }
}

void AnalyzerView::Rebuild() {
    rows_.clear();
    ScanResult* sc = mw_->scan();
    if (!sc) {
        cur_ = nullptr;
        searchMode_ = false;
        searchRows_.clear();
    } else if (!cur_) {
        if (!keepPath_.empty()) cur_ = FindNodeByPath(sc->root.get(), keepPath_);
        if (!cur_) cur_ = sc->root.get();
    }
    keepPath_.clear();
    if (searchMode_) {
        for (ScanNode* n : searchRows_)
            if (!n->removed && Visible(n)) rows_.push_back(n);
    } else if (cur_ && cur_->dir) {
        for (auto& c : cur_->dir->children)
            if (!c.removed && Visible(&c)) rows_.push_back(&c);
    }
    Sort();
    ListView_SetItemCountEx(list_, static_cast<int>(rows_.size()), 0);
    InvalidateRect(list_, nullptr, FALSE);

    bool none = rows_.empty();
    ShowWindow(list_, none ? SW_HIDE : SW_SHOW);
    ShowWindow(empty_, none ? SW_SHOW : SW_HIDE);
    ShowWindow(bScan_, (!sc) ? SW_SHOW : SW_HIDE);
    if (none) {
        SetWindowTextW(empty_, !sc ? mw_->EmptyStateText().c_str()
                                    : (searchMode_ ? L"No files matching this filter were found." : L"This folder is empty."));
    }
    UpdateSelectionUi();
    InvalidateRect(hwnd_, &band_, FALSE);
}

void AnalyzerView::Sort() {
    auto key = [&](const ScanNode* a, const ScanNode* b) -> int {
        switch (sortCol_) {
            case C_NAME: return CmpName(a, b);
            case C_TYPE: {
                int t = static_cast<int>(a->type) - static_cast<int>(b->type);
                if (t) return t;
                std::wstring ea = ExtOf(a), eb = ExtOf(b);
                return CompareStringOrdinal(ea.c_str(), static_cast<int>(ea.size()), eb.c_str(), static_cast<int>(eb.size()), TRUE) - CSTR_EQUAL;
            }
            case C_SIZE:
            case C_PCT: return Cmp(a->size, b->size);
            case C_DISK: return Cmp(a->allocated, b->allocated);
            case C_FILES: return Cmp(a->files, b->files);
            case C_DIRS: return Cmp(a->dirs, b->dirs);
            case C_MOD: return Cmp(a->modified, b->modified);
            default: return static_cast<int>(a->status) - static_cast<int>(b->status);
        }
    };
    std::sort(rows_.begin(), rows_.end(), [&](ScanNode* a, ScanNode* b) {
        int c = key(a, b);
        if (c == 0) c = CmpName(a, b);
        return sortAsc_ ? c < 0 : c > 0;
    });
    UpdateSortArrow();
}

void AnalyzerView::SortChanged() {
    Sort();
    InvalidateRect(list_, nullptr, FALSE);
    if (!rows_.empty()) ListView_EnsureVisible(list_, 0, FALSE);
}

void AnalyzerView::UpdateSortArrow() {
    static const int kCols[] = {C_NAME, C_TYPE, C_SIZE, C_MOD, C_FILES, C_DIRS};
    for (int i = 0; i < 6; ++i)
        if (kCols[i] == sortCol_ || (sortCol_ == C_PCT && kCols[i] == C_SIZE) || (sortCol_ == C_DISK && kCols[i] == C_SIZE))
            SendMessageW(sortCb_, CB_SETCURSEL, static_cast<WPARAM>(i), 0);
    SetWindowTextW(dirBtn_, sortAsc_ ? L"Ascending" : L"Descending");
    HWND hdr = ListView_GetHeader(list_);
    for (int i = 0; i < C_COUNT; ++i) {
        HDITEMW it{HDI_FORMAT};
        if (!Header_GetItem(hdr, i, &it)) continue;
        it.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (i == sortCol_) it.fmt |= sortAsc_ ? HDF_SORTUP : HDF_SORTDOWN;
        Header_SetItem(hdr, i, &it);
    }
}

void AnalyzerView::RunSearch() {
    wchar_t buf[512];
    GetWindowTextW(search_, buf, 512);
    std::wstring q = buf;
    while (!q.empty() && q.back() == L' ') q.pop_back();
    while (!q.empty() && q.front() == L' ') q.erase(q.begin());
    if (q.empty() || !mw_->scan()) {
        if (searchMode_) {
            searchMode_ = false;
            searchRows_.clear();
            Rebuild();
        }
        return;
    }
    for (auto& ch : q) ch = static_cast<wchar_t>(towlower(ch));
    bool pathMode = q.find(L'\\') != std::wstring::npos || q.find(L':') != std::wstring::npos;
    searchRows_.clear();
    truncated_ = false;
    // Searches the existing index (no disk access). The directory path is carried down the walk, so a path query costs
    // one string join per entry instead of rebuilding the whole path from the parent chain.
    ScanNode* top = mw_->scan()->root.get();
    std::vector<std::pair<ScanNode*, std::wstring>> stack;
    stack.emplace_back(top, pathMode ? BuildPath(top) : std::wstring());
    while (!stack.empty() && !truncated_) {
        auto [n, dirPath] = std::move(stack.back());
        stack.pop_back();
        if (!n->dir) continue;
        for (auto& c : n->dir->children) {
            if (c.removed) continue;
            std::wstring childPath;
            if (pathMode) childPath = path::Join(dirPath, c.name);
            bool hit = pathMode ? ContainsNoCase(childPath, q) : ContainsNoCase(c.name, q);
            if (hit) {
                searchRows_.push_back(&c);
                if (searchRows_.size() >= kMaxSearchResults) {
                    truncated_ = true;
                    break;
                }
            }
            if (c.type == NodeType::Directory) stack.emplace_back(&c, std::move(childPath));
        }
    }
    searchMode_ = true;
    Rebuild();
    if (truncated_) SetWindowTextW(status_, L"Showing the first 20,000 matches. Refine the search to see more.");
}

// ---------------------------------------------------------------- cells

std::wstring AnalyzerView::CellText(ScanNode* n, int col) const {
    switch (col) {
        case C_NAME: return searchMode_ ? BuildPath(n) : n->name;
        case C_TYPE:
            switch (n->type) {
                case NodeType::Directory: return L"Folder";
                case NodeType::Virtual: return L"Computer";
                case NodeType::Reparse:
                    return n->status == NodeStatus::Junction ? L"Junction" : n->status == NodeStatus::Symlink ? L"Symbolic link" : L"Reparse point";
                default: {
                    std::wstring e = ExtOf(n);
                    if (e.empty()) return L"File";
                    for (auto& ch : e) ch = static_cast<wchar_t>(towupper(ch));
                    return e.substr(1) + L" File";
                }
            }
        case C_SIZE: return n->type == NodeType::Reparse ? L"" : FormatBytes(n->size);
        case C_DISK: return n->type == NodeType::Reparse ? L"" : FormatBytes(n->allocated);
        case C_PCT: {
            uint64_t total = n->parent ? n->parent->size : n->size;
            wchar_t b[32];
            swprintf_s(b, L"%.1f%%", total ? 100.0 * static_cast<double>(n->size) / static_cast<double>(total) : 0.0);
            return b;
        }
        case C_FILES: return n->type == NodeType::Directory ? FormatCount(n->files) : L"";
        case C_DIRS: return n->type == NodeType::Directory ? FormatCount(n->dirs) : L"";
        case C_MOD: return FormatFileTime(n->modified);
        case C_STATUS: return StatusText(n);
    }
    return L"";
}

int AnalyzerView::IconFor(ScanNode* n) {
    if (n->type != NodeType::File) return folderIcon_;
    std::wstring e = ExtOf(n);
    for (auto& ch : e) ch = static_cast<wchar_t>(towlower(ch));
    auto it = iconCache_.find(e);
    if (it != iconCache_.end()) return it->second;
    SHFILEINFOW sfi{};
    std::wstring probe = L"file" + e;
    SHGetFileInfoW(probe.c_str(), FILE_ATTRIBUTE_NORMAL, &sfi, sizeof sfi, SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
    iconCache_[e] = sfi.iIcon;
    return sfi.iIcon;
}

void AnalyzerView::DrawPercent(NMLVCUSTOMDRAW* cd) {
    int idx = static_cast<int>(cd->nmcd.dwItemSpec);
    if (idx < 0 || idx >= static_cast<int>(rows_.size())) return;
    ScanNode* n = rows_[static_cast<size_t>(idx)];
    RECT r;
    ListView_GetSubItemRect(list_, idx, C_PCT, LVIR_BOUNDS, &r);
    HDC dc = cd->nmcd.hdc;
    bool sel = (ListView_GetItemState(list_, idx, LVIS_SELECTED) & LVIS_SELECTED) != 0;
    HBRUSH bg = CreateSolidBrush(sel ? Colors().accentSoft : Colors().panel);
    FillRect(dc, &r, bg);
    DeleteObject(bg);
    uint64_t total = n->parent ? n->parent->size : n->size;
    double frac = total ? static_cast<double>(n->size) / static_cast<double>(total) : 0.0;
    RECT bar{r.left + 4, r.top + 4, r.right - 4, r.bottom - 4};
    RECT fill = bar;
    fill.right = bar.left + static_cast<LONG>((bar.right - bar.left) * frac);
    HBRUSH fb = CreateSolidBrush(sel ? Colors().barFillSel : Colors().barFill);
    FillRect(dc, &fill, fb);
    DeleteObject(fb);
    std::wstring t = CellText(n, C_PCT);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, Colors().text);
    RECT tr{r.left + 8, r.top, r.right - 4, r.bottom};
    DrawTextW(dc, t.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

// ---------------------------------------------------------------- selection & actions

std::vector<ScanNode*> AnalyzerView::Selected() const {
    std::vector<ScanNode*> out;
    for (int i = ListView_GetNextItem(list_, -1, LVNI_SELECTED); i >= 0; i = ListView_GetNextItem(list_, i, LVNI_SELECTED))
        if (i < static_cast<int>(rows_.size())) out.push_back(rows_[static_cast<size_t>(i)]);
    return out;
}

void AnalyzerView::UpdateSelectionUi() {
    auto sel = Selected();
    bool idle = !mw_->busy();
    bool canDelete = !sel.empty() && idle;
    bool noDelete = false;
    std::wstring note;
    if (sel.size() <= kMaxPreCheck) {  // the service re-checks everything again at delete time
        for (ScanNode* n : sel) {
            ProtectionInfo pi = mw_->protection().Classify(BuildPath(n));
            if (pi.level >= ProtectionLevel::NoDelete) {
                canDelete = false;
                if (note.empty()) note = pi.reason;
                if (pi.level == ProtectionLevel::NoDelete) noDelete = true;
            } else if (pi.level == ProtectionLevel::Caution && note.empty()) {
                note = pi.reason;
            }
        }
    }
    uint64_t total = 0;
    for (auto* n : sel) total += n->size;
    std::wstring s;
    if (sel.empty()) s = rows_.empty() ? L"" : FormatCount(rows_.size()) + (rows_.size() == 1 ? L" item" : L" items");
    else if (sel.size() == 1) s = FormatBytes(total) + L"   " + (note.empty() ? L"" : note);
    else s = FormatCount(sel.size()) + L" items selected, total " + FormatBytes(total) + L"   " + (note.empty() ? L"" : note);
    if (!(truncated_ && searchMode_)) SetWindowTextW(status_, s.c_str());
    else if (!sel.empty()) SetWindowTextW(status_, s.c_str());

    EnableWindow(bOpen_, sel.size() == 1);
    EnableWindow(bManage_, sel.size() == 1);
    EnableWindow(bDelete_, canDelete);
    EnableWindow(bRecycle_, canDelete);
    EnableWindow(bPerm_, canDelete);
    EnableWindow(bBasket_, canDelete);
    EnableWindow(bRefresh_, idle && mw_->scan() && cur_ && (cur_->type == NodeType::Directory || cur_->type == NodeType::Virtual));
    ShowWindow(bApps_, noDelete ? SW_SHOW : SW_HIDE);
}

void AnalyzerView::OpenNode(ScanNode* n) {
    if (n->type == NodeType::Directory) NavigateTo(n);
    else if (n->type == NodeType::Reparse) explorer::OpenFolder(BuildPath(n));
    else if (n->type == NodeType::File) explorer::OpenItem(BuildPath(n), false);
}

void AnalyzerView::Act(int id) {
    auto sel = Selected();
    if (id == ID_REFRESH) {
        if (cur_ && cur_->type == NodeType::Virtual) mw_->Rescan();  // "This PC": rescan everything that was scanned
        else if (cur_) mw_->RefreshFolder(cur_);
        return;
    }
    if (sel.empty()) return;
    ScanNode* n = sel[0];
    std::wstring p = BuildPath(n);
    bool dirLike = n->type != NodeType::File;
    switch (id) {
        case ID_OPEN:
        case M_OPEN: OpenNode(n); break;
        case ID_MANAGE:
        case M_LOCATION: explorer::Reveal(p, id == ID_MANAGE ? dirLike : false); break;
        case M_COPY: explorer::CopyText(mw_->hwnd(), p); break;
        case M_PROPS: explorer::ShowProperties(mw_->hwnd(), p); break;
        case ID_DELETE: mw_->DeleteItems(sel, DeleteMode::Ask); break;
        case ID_RECYCLE:
        case M_RECYCLE: mw_->DeleteItems(sel, DeleteMode::Recycle); break;
        case ID_PERM:
        case M_PERM: mw_->DeleteItems(sel, DeleteMode::Permanent); break;
        case ID_BASKET:
        case M_BASKET: mw_->AddToBasket(sel); break;
    }
}

void AnalyzerView::ContextMenu(POINT pt) {
    auto sel = Selected();
    if (sel.empty()) return;
    bool canDelete = !mw_->busy();
    if (canDelete)
        for (size_t i = 0; i < sel.size() && i < kMaxPreCheck; ++i)
            if (mw_->protection().Classify(BuildPath(sel[i])).level >= ProtectionLevel::NoDelete) canDelete = false;
    HMENU m = CreatePopupMenu();
    bool one = sel.size() == 1;
    AppendMenuW(m, MF_STRING | (one ? 0 : MF_GRAYED), M_OPEN, L"Open");
    AppendMenuW(m, MF_STRING | (one ? 0 : MF_GRAYED), M_LOCATION, L"Open File Location");
    AppendMenuW(m, MF_STRING | (one ? 0 : MF_GRAYED), M_COPY, L"Copy Path");
    AppendMenuW(m, MF_STRING | (one ? 0 : MF_GRAYED), M_PROPS, L"Properties");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (canDelete ? 0 : MF_GRAYED), M_RECYCLE, L"Move to Recycle Bin");
    AppendMenuW(m, MF_STRING | (canDelete ? 0 : MF_GRAYED), M_PERM, L"Delete Permanently");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (canDelete ? 0 : MF_GRAYED), M_BASKET, L"Add to Basket");
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(m);
    if (cmd) Act(cmd);
}

// ---------------------------------------------------------------- painting & messages

void AnalyzerView::PaintBand() {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd_, &ps);
    FillRect(dc, &band_, BgBrush());  // repaints are not erased by the system: clear the band or old text shows through
    {   // rounded search box frame (the edit control sits inside it)
        const Theme& t = Colors();
        bool focus = GetFocus() == search_;
        HBRUSH b = CreateSolidBrush(t.panel);
        HPEN pen = CreatePen(PS_SOLID, focus ? 2 : 1, focus ? t.accent : t.border);
        HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, pen);
        int r = mw_->S(10);
        RoundRect(dc, searchFrame_.left, searchFrame_.top, searchFrame_.right, searchFrame_.bottom, r, r);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(b);
        DeleteObject(pen);
    }
    SetBkMode(dc, TRANSPARENT);
    crumbs_.clear();
    ScanResult* sc = mw_->scan();
    int x0 = mw_->S(12) + mw_->S(64) + mw_->S(8) + mw_->S(100) + mw_->S(12);
    int x1 = crumbRight_;
    int y0 = 0, y1 = band_.bottom;
    SelectObject(dc, mw_->font());
    if (sc && x1 > x0) {
        if (searchMode_) {
            std::wstring t = L"Search results (" + FormatCount(rows_.size()) + L" items)";
            SetTextColor(dc, Colors().muted);
            RECT r{x0, y0, x1, y1};
            DrawTextW(dc, t.c_str(), -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else if (cur_) {
            std::vector<ScanNode*> chain;
            for (ScanNode* p = cur_; p; p = p->parent) chain.push_back(p);
            std::reverse(chain.begin(), chain.end());
            const int gap = mw_->S(18);
            std::vector<int> widths;
            int total = 0;
            for (ScanNode* n : chain) {
                std::wstring t = DisplayName(n);
                SelectObject(dc, n == cur_ ? mw_->boldFont() : mw_->font());  // measure with the font it is drawn in
                SIZE sz{};
                GetTextExtentPoint32W(dc, t.c_str(), static_cast<int>(t.size()), &sz);
                widths.push_back(sz.cx);
                total += sz.cx + gap;
            }
            size_t start = 0;
            while (start + 1 < chain.size() && total > x1 - x0) total -= widths[start++] + gap;  // drop leftmost crumbs
            int x = x0;
            for (size_t i = start; i < chain.size(); ++i) {
                std::wstring t = DisplayName(chain[i]);
                bool last = i + 1 == chain.size();
                SelectObject(dc, last ? mw_->boldFont() : mw_->font());
                SetTextColor(dc, last ? Colors().text : Colors().accent);
                RECT r{x, y0, std::min<LONG>(x1, x + widths[i] + 4), y1};
                DrawTextW(dc, t.c_str(), -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                crumbs_.push_back({r, chain[i]});
                x += widths[i];
                if (!last) {
                    SelectObject(dc, mw_->font());
                    SetTextColor(dc, Colors().muted);
                    RECT s{x, y0, x + gap, y1};
                    DrawTextW(dc, L" > ", -1, &s, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    x += gap;
                }
            }
        }
    }
    HPEN pen = CreatePen(PS_SOLID, 1, Colors().border);
    HGDIOBJ old = SelectObject(dc, pen);
    MoveToEx(dc, 0, band_.bottom - 1, nullptr);
    LineTo(dc, band_.right, band_.bottom - 1);
    SelectObject(dc, old);
    DeleteObject(pen);
    EndPaint(hwnd_, &ps);
}

LRESULT AnalyzerView::OnNotify(NMHDR* nm, LPARAM l) {
    if (nm->code == NM_CUSTOMDRAW && nm->hwndFrom == ListView_GetHeader(list_)) return HeaderCustomDraw(l);
    if (nm->hwndFrom != list_) return 0;
    switch (nm->code) {
        case LVN_GETDISPINFOW: {
            auto* d = reinterpret_cast<NMLVDISPINFOW*>(l);
            if (d->item.iItem < 0 || d->item.iItem >= static_cast<int>(rows_.size())) break;
            ScanNode* n = rows_[static_cast<size_t>(d->item.iItem)];
            if (d->item.mask & LVIF_TEXT) wcsncpy_s(d->item.pszText, static_cast<size_t>(d->item.cchTextMax), CellText(n, d->item.iSubItem).c_str(), _TRUNCATE);
            if ((d->item.mask & LVIF_IMAGE) && d->item.iSubItem == 0) d->item.iImage = IconFor(n);
            break;
        }
        case LVN_ODFINDITEMW: return -1;
        case LVN_COLUMNCLICK: {
            int c = reinterpret_cast<NMLISTVIEW*>(l)->iSubItem;
            if (c == sortCol_) sortAsc_ = !sortAsc_;
            else {
                sortCol_ = c;
                sortAsc_ = (c == C_NAME || c == C_TYPE || c == C_STATUS);
            }
            Sort();
            InvalidateRect(list_, nullptr, FALSE);
            break;
        }
        case LVN_ITEMCHANGED:
        case LVN_ODSTATECHANGED: SetTimer(hwnd_, kSelTimer, 60, nullptr); break;
        case NM_DBLCLK: {  // open the row under the cursor directly; do not depend on selection state
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(list_, &pt);
            LVHITTESTINFO ht{};
            ht.pt = pt;
            int i = ListView_HitTest(list_, &ht);
            if (i < 0) i = reinterpret_cast<NMITEMACTIVATE*>(l)->iItem;
            if (i >= 0 && i < static_cast<int>(rows_.size())) OpenNode(rows_[static_cast<size_t>(i)]);
            break;
        }
        case LVN_KEYDOWN: {
            auto* k = reinterpret_cast<NMLVKEYDOWN*>(l);
            bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
            if (k->wVKey == VK_F5) Act(ID_REFRESH);
            else if (k->wVKey == VK_RETURN) Act(ID_OPEN);
            else if (k->wVKey == VK_BACK) GoUp();
            else if (k->wVKey == VK_DELETE && IsWindowEnabled(bDelete_)) Act(shift ? ID_PERM : ID_RECYCLE);
            else if (k->wVKey == 'C' && ctrl) Act(M_COPY);
            break;
        }
        case NM_CUSTOMDRAW: {
            auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(l);
            switch (cd->nmcd.dwDrawStage) {
                case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW;
                case CDDS_ITEMPREPAINT: return CDRF_NOTIFYSUBITEMDRAW;
                case CDDS_ITEMPREPAINT | CDDS_SUBITEM:
                    if (cd->iSubItem == C_PCT) {
                        DrawPercent(cd);
                        return CDRF_SKIPDEFAULT;
                    }
                    return CDRF_DODEFAULT;
            }
            break;
        }
    }
    return 0;
}

LRESULT AnalyzerView::OnMessage(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_PAINT: PaintBand(); return 0;
        case WM_NOTIFY: return OnNotify(reinterpret_cast<NMHDR*>(l), l);
        case WM_TIMER:
            KillTimer(hwnd_, w);
            if (w == kSelTimer) UpdateSelectionUi();
            else if (w == kSearchTimer) RunSearch();
            return 0;
        case WM_LBUTTONDOWN: {
            POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            for (auto& c : crumbs_)
                if (PtInRect(&c.r, p) && c.n != cur_) {
                    NavigateTo(c.n);
                    break;
                }
            return 0;
        }
        case WM_CONTEXTMENU:
            if (reinterpret_cast<HWND>(w) == list_) {
                POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                if (pt.x == -1) {
                    int i = ListView_GetNextItem(list_, -1, LVNI_FOCUSED);
                    RECT r{};
                    if (i >= 0) ListView_GetItemRect(list_, i, &r, LVIR_LABEL);
                    pt = {r.left, r.bottom};
                    ClientToScreen(list_, &pt);
                } else {
                    POINT c = pt;
                    ScreenToClient(list_, &c);
                    LVHITTESTINFO ht{};
                    ht.pt = c;
                    int i = ListView_HitTest(list_, &ht);
                    if (i >= 0 && !(ListView_GetItemState(list_, i, LVIS_SELECTED) & LVIS_SELECTED)) {
                        ListView_SetItemState(list_, -1, 0, LVIS_SELECTED);
                        ListView_SetItemState(list_, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                    } else if (i < 0) {
                        return 0;
                    }
                }
                ContextMenu(pt);
                return 0;
            }
            break;
        case WM_COMMAND:
            switch (LOWORD(w)) {
                case ID_UP: GoUp(); break;
                case ID_DIR:
                    sortAsc_ = !sortAsc_;
                    SortChanged();
                    break;
                case ID_SORT:
                    if (HIWORD(w) == CBN_SELCHANGE) {
                        static const int kCols[] = {C_NAME, C_TYPE, C_SIZE, C_MOD, C_FILES, C_DIRS};
                        int i = static_cast<int>(SendMessageW(sortCb_, CB_GETCURSEL, 0, 0));
                        if (i >= 0 && i < 6) {
                            sortCol_ = kCols[i];
                            sortAsc_ = (sortCol_ == C_NAME || sortCol_ == C_TYPE);  // text ascending; size/date/count largest/newest first
                            SortChanged();
                        }
                    }
                    break;
                case ID_APPS: explorer::OpenInstalledApps(); break;
                case ID_SCAN: mw_->ShowPage(0); break;
                case ID_SEARCH:
                    if (HIWORD(w) == EN_CHANGE && !suppressSearch_) SetTimer(hwnd_, kSearchTimer, 350, nullptr);  // debounce
                    else if (HIWORD(w) == EN_SETFOCUS || HIWORD(w) == EN_KILLFOCUS) InvalidateRect(hwnd_, &searchFrame_, FALSE);
                    break;
                default:
                    if (HIWORD(w) == BN_CLICKED) Act(LOWORD(w));
            }
            return 0;
    }
    return View::OnMessage(m, w, l);
}

}  // namespace ss
