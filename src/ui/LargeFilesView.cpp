#include "ui/LargeFilesView.h"

#include <commctrl.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cwctype>

#include "filesystem/PathUtils.h"
#include "operations/ExplorerService.h"
#include "ui/MainWindow.h"
#include "ui/UiCommon.h"
#include "utils/FormatUtils.h"

namespace ss {
namespace {

enum { ID_SIZE = 1, ID_CUSTOM, ID_TYPE, ID_LIST, ID_INFO, ID_EMPTY, ID_OPEN, ID_RECYCLE, ID_PERM, ID_NOTE, ID_SORT, ID_DIR, ID_REFRESH, ID_STALE, ID_BASKET, ID_SELALL };
enum Col { C_NAME, C_SIZE, C_TYPE, C_LOC, C_MOD, C_COUNT };

std::wstring LowerExt(const ScanNode* n) {
    size_t d = n->name.rfind(L'.');
    if (d == std::wstring::npos || d == 0) return L"";
    std::wstring e = n->name.substr(d);
    for (auto& c : e) c = static_cast<wchar_t>(towlower(c));
    return e;
}


}  // namespace

void LargeFilesView::OnCreate() {
    lblSize_ = Make(L"STATIC", L"Minimum size:", SS_LEFT | SS_CENTERIMAGE, 0);
    cbSize_ = Make(L"SSCombo", L"Minimum size", WS_TABSTOP, ID_SIZE);
    for (const wchar_t* s : old_ ? std::initializer_list<const wchar_t*>{L"1 MB", L"10 MB", L"100 MB", L"1 GB", L"Custom"}
                                 : std::initializer_list<const wchar_t*>{L"100 MB", L"500 MB", L"1 GB", L"5 GB", L"Custom"})
        SendMessageW(cbSize_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
    SendMessageW(cbSize_, CB_SETCURSEL, old_ ? mw_->settings().oldSize : 0, 0);
    edCustom_ = Make(L"EDIT", L"250", WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, ID_CUSTOM, WS_EX_CLIENTEDGE);
    lblMb_ = Make(L"STATIC", L"MB", SS_LEFT | SS_CENTERIMAGE, 0);
    EnableWindow(edCustom_, FALSE);
    ShowWindow(edCustom_, SW_HIDE);  // only shown while "Custom" is chosen
    ShowWindow(lblMb_, SW_HIDE);
    lblType_ = Make(L"STATIC", L"Type:", SS_LEFT | SS_CENTERIMAGE, 0);
    cbType_ = Make(L"SSCombo", L"File type", WS_TABSTOP, ID_TYPE);
    SendMessageW(cbType_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"All types"));
    for (int i = 0; i < kCategoryCount; ++i)
        SendMessageW(cbType_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(CategoryName(static_cast<FileCategory>(i))));
    SendMessageW(cbType_, CB_SETCURSEL, 0, 0);
    lblStale_ = Make(L"STATIC", old_ ? L"Not modified in:" : L"Last modified:",SS_LEFT | SS_CENTERIMAGE, 0);
    cbStale_ = Make(L"SSCombo", L"Last modified", WS_TABSTOP, ID_STALE);  // old and big = the safest cleanup candidates
    for (const wchar_t* s : old_ ? std::initializer_list<const wchar_t*>{L"Older than 1 month", L"Older than 3 months", L"Older than 6 months", L"Older than 1 year", L"Older than 2 years"}
                                 : std::initializer_list<const wchar_t*>{L"Any time", L"Older than 6 months", L"Older than 1 year", L"Older than 2 years"})
        SendMessageW(cbStale_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
    SendMessageW(cbStale_, CB_SETCURSEL, old_ ? mw_->settings().oldAge : 0, 0);
    bBasket_ = Make(L"BUTTON", L"Add to Basket", WS_TABSTOP | BS_OWNERDRAW, ID_BASKET);
    bSelAll_ = Make(L"BUTTON", L"Select all", WS_TABSTOP | BS_OWNERDRAW, ID_SELALL);

    list_ = Make(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS, ID_LIST);
    SetWindowTheme(list_, L"Explorer", nullptr);
    ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP | LVS_EX_CHECKBOXES);
    ListView_SetCallbackMask(list_, LVIS_STATEIMAGEMASK);  // the checkbox state of a virtual list comes from checked_
    struct D {
        const wchar_t* t;
        int w, fmt;
    } defs[C_COUNT] = {{L"Name", 280, LVCFMT_LEFT}, {L"Size", 100, LVCFMT_RIGHT}, {L"Type", 110, LVCFMT_LEFT},
                       {L"Location", 460, LVCFMT_LEFT}, {L"Modified", 140, LVCFMT_LEFT}};
    for (int i = 0; i < C_COUNT; ++i) {
        LVCOLUMNW c{LVCF_TEXT | LVCF_WIDTH | LVCF_FMT};
        c.pszText = const_cast<wchar_t*>(defs[i].t);
        c.cx = mw_->S(defs[i].w * 118 / 100);
        c.fmt = defs[i].fmt;
        SendMessageW(list_, LVM_INSERTCOLUMNW, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(&c));
    }
    SyncSortUi();
    info_ = Make(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX, ID_INFO);
    empty_ = Make(L"STATIC", L"", SS_CENTER | SS_NOPREFIX, ID_EMPTY);
    bOpen_ = Make(L"BUTTON", L"Open File Location", WS_TABSTOP | BS_OWNERDRAW, ID_OPEN);
    bRecycle_ = Make(L"BUTTON", L"Move to Recycle Bin", WS_TABSTOP | BS_OWNERDRAW, ID_RECYCLE);
    bPerm_ = Make(L"BUTTON", L"Delete Permanently", WS_TABSTOP | BS_OWNERDRAW, ID_PERM);
    bRefresh_ = Make(L"BUTTON", L"Refresh", WS_TABSTOP | BS_OWNERDRAW, ID_REFRESH);  // drops vanished files now, then rescans (also F5)
    note_ = Make(L"SSInfo", L"File size and extension do not tell you whether a file is safe to delete. Check before removing anything.", WS_TABSTOP, ID_NOTE);
}

void LargeFilesView::Layout(int w, int h) {
    lastW_ = w;
    lastH_ = h;
    int m = mw_->S(12), gap = mw_->S(8), ch = mw_->S(34), bh = mw_->S(32);
    bool custom = SendMessageW(cbSize_, CB_GETCURSEL, 0, 0) == 4;
    ShowWindow(edCustom_, custom ? SW_SHOW : SW_HIDE);
    ShowWindow(lblMb_, custom ? SW_SHOW : SW_HIDE);
    // Filters flow left to right and wrap as a group (a label never ends up alone at the end of a row).
    int x = m, y = mw_->S(8);
    struct It {
        HWND h;
        int w;
    };
    auto group = [&](std::initializer_list<It> items) {
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
    if (custom) group({{edCustom_, 70}, {lblMb_, 30}});
    group({{lblType_, 44}, {cbType_, 150}});
    group({{lblStale_, old_ ? 130 : 112}, {cbStale_, 230}});
    group({{bRefresh_, 100}});
    y += ch + gap;
    int sw = mw_->S(150);  // "Select all" sits at the right end of the result line
    MoveWindow(info_, m, y, std::max(mw_->S(100), w - 2 * m - sw - gap), mw_->S(24), TRUE);
    MoveWindow(bSelAll_, w - m - sw, y - mw_->S(2), sw, mw_->S(28), TRUE);
    int top = y + mw_->S(32);
    // Action buttons appear only while something is selected; they wrap in a narrow window.
    const FlowItem btns[] = {{bOpen_, mw_->S(170)}, {bBasket_, mw_->S(150)}, {bRecycle_, mw_->S(190)}, {bPerm_, mw_->S(180)}};
    int rows = FlowButtons(btns, 4, m, w, 0, bh, gap, false, false);
    int barH = barShown_ ? rows * (bh + gap) + gap : 0;
    int bottom = h - mw_->S(34) - barH;
    MoveWindow(list_, 0, top, w, std::max(0, bottom - top), TRUE);
    static const int kCols[C_COUNT] = {300, 90, 100, 300, 160};
    FitColumns(list_, kCols, C_COUNT, w);
    MoveWindow(note_, m, bottom + mw_->S(6), mw_->S(kInfoIconSize), mw_->S(kInfoIconSize), TRUE);  // the safety note is this icon's tooltip
    FlowButtons(btns, 4, m, w, h - barH + gap, bh, gap, true, barShown_);
    int ew = std::min(w - 2 * m, mw_->S(520));
    MoveWindow(empty_, (w - ew) / 2, top + mw_->S(60), ew, mw_->S(48), TRUE);
}

void LargeFilesView::SyncSortUi() {  // the sort order is shown as an arrow in the column header (click a header to sort)
    HWND hdr = ListView_GetHeader(list_);
    if (!hdr) return;
    for (int i = 0; i < C_COUNT; ++i) {
        HDITEMW it{HDI_FORMAT};
        if (!Header_GetItem(hdr, i, &it)) continue;
        it.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (i == sortCol_) it.fmt |= sortAsc_ ? HDF_SORTUP : HDF_SORTDOWN;
        Header_SetItem(hdr, i, &it);
    }
}

uint64_t LargeFilesView::Mod(const ScanNode* n) const {  // folders: the newest file inside (Old Files page); files: their own time
    if (n->type == NodeType::Directory) {
        auto it = newest_.find(n);
        if (it != newest_.end()) return it->second;
    }
    return n->modified;
}

uint64_t LargeFilesView::Fingerprint() const {
    ScanResult* sc = mw_->scan();
    if (!sc || !sc->root) return 1;
    const ScanNode& r = *sc->root;
    uint64_t h = reinterpret_cast<uintptr_t>(sc) * 0x9E3779B97F4A7C15ull;
    for (uint64_t v : {mw_->treeRev(), r.size, r.allocated, static_cast<uint64_t>(r.files), static_cast<uint64_t>(r.dirs),
                       static_cast<uint64_t>(mw_->settings().showHidden) | (static_cast<uint64_t>(mw_->settings().showAppData) << 1)})
        h = (h ^ v) * 0x100000001B3ull + (h >> 29);
    return h | 1;  // never 0 (0 = "not built yet")
}

void LargeFilesView::DropReferences() {
    fp_ = 0;
    keep_.Remember(rows_, checked_, list_, true);  // by path: the nodes are about to be rebuilt
    newest_.clear();
    rows_.clear();
    checked_.clear();
    ListView_SetItemCountEx(list_, 0, 0);
}

uint64_t LargeFilesView::Threshold() const {
    static const uint64_t mb = 1024ull * 1024ull;
    static const uint64_t kLarge[] = {100, 500, 1024, 5 * 1024}, kOld[] = {1, 10, 100, 1024};
    LRESULT sel = SendMessageW(cbSize_, CB_GETCURSEL, 0, 0);
    switch (sel) {
        case 0: case 1: case 2: case 3: return (old_ ? kOld : kLarge)[sel] * mb;
        default: {
            wchar_t b[16];
            GetWindowTextW(edCustom_, b, 16);
            return std::max<uint64_t>(1, _wcstoui64(b, nullptr, 10)) * mb;
        }
    }
}

void LargeFilesView::Rebuild() {
    fp_ = Fingerprint();
    if (!keep_.Pending()) keep_.Remember(rows_, checked_, list_, false);
    rows_.clear();
    ScanResult* sc = mw_->scan();
    int cat = static_cast<int>(SendMessageW(cbType_, CB_GETCURSEL, 0, 0));  // 0 = all, else FileCategory + 1
    uint64_t total = 0;
    uint64_t staleCutoff = ~0ull;  // files modified at or before this FILETIME qualify
    {
        static const int kDaysLarge[] = {0, 183, 365, 730}, kDaysOld[] = {30, 91, 183, 365, 730};
        const int* kDays = old_ ? kDaysOld : kDaysLarge;
        int si = static_cast<int>(SendMessageW(cbStale_, CB_GETCURSEL, 0, 0));
        if (si >= 0 && si < (old_ ? 5 : 4) && kDays[si] > 0) {
            FILETIME now;
            GetSystemTimeAsFileTime(&now);
            uint64_t t = (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
            staleCutoff = t - static_cast<uint64_t>(kDays[si]) * 864000000000ull;
        }
    }
    if (sc) {
        uint64_t minSize = Threshold();
        // Windows and installed programs (Program Files...) are never listed: nothing there can be deleted from this app anyway.
        // Only the top level of a scan is classified (everything below inherits it), so this costs a few lookups, not one per folder.
        auto blocked = [&](const ScanNode& c) {
            if (c.type != NodeType::Directory || !(c.scanRoot || (c.parent && c.parent->scanRoot))) return false;
            std::wstring p = BuildPath(&c);
            return !path::IsDriveRoot(p) && mw_->protection().IsSystemLocation(p);
        };
        auto visible = [&](const ScanNode& c) {
            return !(c.attrs & FILE_ATTRIBUTE_SYSTEM) && (mw_->settings().showHidden || !(c.attrs & FILE_ATTRIBUTE_HIDDEN)) &&
                   !(c.type == NodeType::Directory && !mw_->settings().showAppData && IsAppDataRoot(&c)) && !blocked(c);
        };
        newest_.clear();
        if (old_) {  // newest modified time inside each folder, so a whole untouched folder can be listed as one row
            std::vector<ScanNode*> order{sc->root.get()}, st{sc->root.get()};  // a parent always precedes its children
            while (!st.empty()) {
                ScanNode* n = st.back();
                st.pop_back();
                if (!n->dir) continue;
                for (auto& c : n->dir->children)
                    if (!c.removed && c.type == NodeType::Directory && visible(c)) {
                        order.push_back(&c);
                        st.push_back(&c);
                    }
            }
            for (size_t i = order.size(); i-- > 0;) {  // children first
                ScanNode* d = order[i];
                uint64_t m = d->modified;
                if (d->dir)
                    for (auto& c : d->dir->children) {
                        if (c.removed) continue;
                        if (c.type == NodeType::File) m = std::max(m, c.modified);
                        else if (c.type == NodeType::Directory) {
                            auto it = newest_.find(&c);
                            if (it != newest_.end()) m = std::max(m, it->second);
                        }
                    }
                newest_[d] = m;
            }
        }
        std::vector<ScanNode*> stack;  // walks the existing scan index; no disk access
        if (!blocked(*sc->root)) stack.push_back(sc->root.get());
        while (!stack.empty()) {
            ScanNode* n = stack.back();
            stack.pop_back();
            if (!n->dir) continue;
            for (auto& c : n->dir->children) {
                if (c.removed) continue;
                if (c.attrs & FILE_ATTRIBUTE_SYSTEM) continue;  // system files are never shown
                if (!mw_->settings().showHidden && (c.attrs & FILE_ATTRIBUTE_HIDDEN)) continue;
                if (c.type == NodeType::Directory) {
                    if (!mw_->settings().showAppData && IsAppDataRoot(&c)) continue;  // AppData / ProgramData: advanced
                    if (blocked(c)) continue;
                    if (old_ && cat == 0 && c.files > 0 && c.size >= minSize && Mod(&c) <= staleCutoff) {  // untouched as a whole: one row
                        rows_.push_back(&c);
                        total += c.size;
                        continue;
                    }
                    stack.push_back(&c);
                }
                else if (c.type == NodeType::File && c.size >= minSize && c.modified <= staleCutoff &&
                         (cat == 0 || static_cast<int>(Categorize(c.name)) == cat - 1)) {
                    rows_.push_back(&c);
                    total += c.size;
                }
            }
        }
    }
    if (sortCol_ == C_LOC) {  // build each location once; building it per comparison froze the UI on big lists
        std::vector<std::pair<std::wstring, ScanNode*>> keyed;
        keyed.reserve(rows_.size());
        for (ScanNode* n : rows_) keyed.emplace_back(path::Parent(BuildPath(n)), n);
        std::sort(keyed.begin(), keyed.end(), [&](const auto& a, const auto& b) {
            int c = CompareStringOrdinal(a.first.c_str(), -1, b.first.c_str(), -1, TRUE) - CSTR_EQUAL;
            return sortAsc_ ? c < 0 : c > 0;
        });
        for (size_t i = 0; i < keyed.size(); ++i) rows_[i] = keyed[i].second;
    } else
    std::sort(rows_.begin(), rows_.end(), [&](ScanNode* a, ScanNode* b) {
        int c = 0;
        switch (sortCol_) {
            case C_NAME: c = CompareStringOrdinal(a->name.c_str(), -1, b->name.c_str(), -1, TRUE) - CSTR_EQUAL; break;
            case C_TYPE: { auto ea = LowerExt(a), eb = LowerExt(b); c = ea < eb ? -1 : ea > eb ? 1 : 0; break; }
            case C_MOD: { uint64_t ma = Mod(a), mb = Mod(b); c = ma < mb ? -1 : ma > mb ? 1 : 0; break; }
            default: c = a->size < b->size ? -1 : a->size > b->size ? 1 : 0;
        }
        return sortAsc_ ? c < 0 : c > 0;
    });
    ListView_SetItemCountEx(list_, static_cast<int>(rows_.size()), 0);
    keep_.Restore(rows_, checked_, list_);
    InvalidateRect(list_, nullptr, FALSE);
    bool none = rows_.empty();
    ShowWindow(list_, none ? SW_HIDE : SW_SHOW);
    ShowWindow(empty_, none ? SW_SHOW : SW_HIDE);
    if (none) SetWindowTextW(empty_, !sc ? mw_->EmptyStateText().c_str() : old_ ? L"Nothing older than this was found. Try a shorter period or a smaller size." : L"No files matching this filter were found.");
    std::wstring info = sc ? (old_ ? Plural(rows_.size(), L"item", L"items") : Plural(rows_.size(), L"file", L"files")) +L", " + FormatBytes(total) + L" in total" : L"";
    std::wstring tip = std::wstring(L"Windows and installed programs (Program Files) are never listed here.\n\n") + (old_ ? L"Age is the last-modified date. Windows does not reliably record when a file was last opened, so a file you open often "
                              L"but never edit can appear here. Check before removing anything."
                            : L"File size and extension do not tell you whether a file is safe to delete. Check before removing anything.");
    if (sc && !mw_->settings().showAppData)
        if (uint64_t hiddenApp = AppDataBytes(sc->root.get())) {
            info += L"   -   " + FormatBytes(hiddenApp) + L" of app data hidden";
            tip += L"\n\nApp data (AppData, ProgramData and tool folders like .vscode) is hidden from this list by default. Turn on \"Show app data\" in Settings > Scan to include it.";
        }
    SetWindowTextW(info_, info.c_str());
    SetWindowTextW(note_, tip.c_str());  // the tooltip of the "i" icon
    UpdateButtons();
}

std::wstring LargeFilesView::CellText(ScanNode* n, int col) const {
    switch (col) {
        case C_NAME: return n->name;
        case C_SIZE: return FormatBytes(n->size);
        case C_TYPE: return n->type == NodeType::Directory ? L"Folder" : CategoryName(Categorize(n->name));
        case C_LOC: return path::Parent(BuildPath(n));
        case C_MOD: return FormatFileTime(Mod(n));
    }
    return L"";
}

std::vector<ScanNode*> LargeFilesView::Selected() const {
    std::vector<ScanNode*> out;
    if (!checked_.empty())  // ticked rows win; otherwise the highlighted ones
        for (ScanNode* n : rows_)
            if (checked_.count(n) && !n->removed) out.push_back(n);
    if (!out.empty()) return out;
    for (int i = ListView_GetNextItem(list_, -1, LVNI_SELECTED); i >= 0; i = ListView_GetNextItem(list_, i, LVNI_SELECTED))
        if (i < static_cast<int>(rows_.size())) out.push_back(rows_[static_cast<size_t>(i)]);
    return out;
}

void LargeFilesView::UpdateButtons() {
    bool all = !rows_.empty() && checked_.size() == rows_.size();
    SetWindowTextW(bSelAll_, all ? L"Clear selection" : L"Select all");
    EnableWindow(bSelAll_, !rows_.empty());
    InvalidateRect(bSelAll_, nullptr, TRUE);
    auto sel = Selected();
    bool ok = !sel.empty() && !mw_->busy();
    if (ok && sel.size() <= 300)
        for (auto* n : sel)
            if (mw_->protection().Classify(BuildPath(n)).level >= ProtectionLevel::NoDelete) ok = false;
    if (barShown_ != !sel.empty()) {  // the action bar is only there while something is selected
        barShown_ = !sel.empty();
        Layout(lastW_, lastH_);
    }
    EnableWindow(bOpen_, sel.size() == 1);
    EnableWindow(bRecycle_, ok);
    EnableWindow(bPerm_, ok);
    EnableWindow(bRefresh_, mw_->scan() && !mw_->busy());
    EnableWindow(bBasket_, ok);
}

void LargeFilesView::ToggleAll() {
    bool all = !rows_.empty() && checked_.size() == rows_.size();
    checked_.clear();
    if (!all) checked_.insert(rows_.begin(), rows_.end());
    InvalidateRect(list_, nullptr, FALSE);
    UpdateButtons();
}

void LargeFilesView::Act(int id) {
    if (id == ID_SELALL) return ToggleAll();
    if (id == ID_REFRESH) {
        mw_->RefreshNodes(rows_);  // verify the listed files and re-list their folders; no full rescan (use the Dashboard for that)
        return;
    }
    auto sel = Selected();
    if (sel.empty()) return;
    if (id == ID_BASKET) {
        mw_->AddToBasket(sel);
        return;
    }
    if (id == ID_OPEN) explorer::Reveal(BuildPath(sel[0]), false);
    else if (id == ID_RECYCLE) mw_->DeleteItems(sel, DeleteMode::Recycle);
    else if (id == ID_PERM) mw_->DeleteItems(sel, DeleteMode::Permanent);
}

LRESULT LargeFilesView::OnMessage(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_NOTIFY: {
            auto* nm = reinterpret_cast<NMHDR*>(l);
            if (nm->code == NM_CUSTOMDRAW && nm->hwndFrom == ListView_GetHeader(list_)) return HeaderCustomDraw(l);
            if (nm->hwndFrom != list_) break;
            switch (nm->code) {
                case LVN_GETDISPINFOW: {
                    auto* d = reinterpret_cast<NMLVDISPINFOW*>(l);
                    if (d->item.iItem >= 0 && d->item.iItem < static_cast<int>(rows_.size())) {
                        const ScanNode* n = rows_[static_cast<size_t>(d->item.iItem)];
                        if (d->item.mask & LVIF_TEXT)
                            wcsncpy_s(d->item.pszText, static_cast<size_t>(d->item.cchTextMax),
                                      CellText(rows_[static_cast<size_t>(d->item.iItem)], d->item.iSubItem).c_str(), _TRUNCATE);
                        if ((d->item.mask & LVIF_STATE) && (d->item.stateMask & LVIS_STATEIMAGEMASK))  // no native check box image: DrawListTick paints it
                            d->item.state &= ~static_cast<UINT>(LVIS_STATEIMAGEMASK);
                        (void)n;
                    }
                    break;
                }
                case NM_CUSTOMDRAW: {
                    auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(l);
                    if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                    if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) return CDRF_NOTIFYPOSTPAINT;
                    if (cd->nmcd.dwDrawStage == CDDS_ITEMPOSTPAINT) {
                        int i = static_cast<int>(cd->nmcd.dwItemSpec);
                        if (i >= 0 && i < static_cast<int>(rows_.size())) DrawListTick(list_, cd->nmcd.hdc, i, checked_.count(rows_[static_cast<size_t>(i)]) != 0);
                    }
                    return CDRF_DODEFAULT;
                }
                case LVN_ODFINDITEMW: return -1;
                case LVN_COLUMNCLICK: {
                    int c = reinterpret_cast<NMLISTVIEW*>(l)->iSubItem;
                    if (c == sortCol_) sortAsc_ = !sortAsc_;
                    else {
                        sortCol_ = c;
                        sortAsc_ = (c == C_NAME || c == C_TYPE || c == C_LOC);
                    }
                    SyncSortUi();
                    Rebuild();
                    break;
                }
                case NM_CLICK: {  // a virtual list does not report checkbox clicks: toggle the tick ourselves
                    POINT pt;
                    GetCursorPos(&pt);
                    ScreenToClient(list_, &pt);
                    int hit = HitListTick(list_, pt);
                    if (hit >= 0 && hit < static_cast<int>(rows_.size())) {
                        const ScanNode* n = rows_[static_cast<size_t>(hit)];
                        if (!checked_.erase(n)) checked_.insert(n);
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
                    if (k->wVKey == VK_F5) Act(ID_REFRESH);
                    else if (k->wVKey == VK_DELETE && IsWindowEnabled(bRecycle_)) Act(GetKeyState(VK_SHIFT) < 0 ? ID_PERM : ID_RECYCLE);
                    else if (k->wVKey == VK_RETURN) Act(ID_OPEN);
                    else if (k->wVKey == VK_SPACE) {  // tick / untick the highlighted rows
                        for (int i = ListView_GetNextItem(list_, -1, LVNI_SELECTED); i >= 0; i = ListView_GetNextItem(list_, i, LVNI_SELECTED))
                            if (i < static_cast<int>(rows_.size()) && !checked_.erase(rows_[static_cast<size_t>(i)])) checked_.insert(rows_[static_cast<size_t>(i)]);
                        InvalidateRect(list_, nullptr, FALSE);
                        UpdateButtons();
                    } else if ((k->wVKey == 'A' && GetKeyState(VK_CONTROL) < 0) || k->wVKey == VK_ESCAPE) {  // tick every row / clear all ticks
                        checked_.clear();
                        if (k->wVKey == 'A')
                            for (ScanNode* n : rows_) checked_.insert(n);
                        InvalidateRect(list_, nullptr, FALSE);
                        UpdateButtons();
                    }
                    break;
                }
            }
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(w), code = HIWORD(w);
            if (old_ && code == CBN_SELCHANGE) {  // the Old Files choices are remembered (saved with the other settings)
                LRESULT s = SendMessageW(id == ID_SIZE ? cbSize_ : cbStale_, CB_GETCURSEL, 0, 0);
                if (id == ID_STALE && s >= 0) mw_->settings().oldAge = static_cast<uint32_t>(s);
                if (id == ID_SIZE && s >= 0 && s < 4) mw_->settings().oldSize = static_cast<uint32_t>(s);  // "Custom" is not remembered
            }
            if (id == ID_SIZE && code == CBN_SELCHANGE) {
                EnableWindow(edCustom_, SendMessageW(cbSize_, CB_GETCURSEL, 0, 0) == 4);
                Layout(lastW_, lastH_);  // shows or hides the custom-size box
                Rebuild();
            } else if ((id == ID_TYPE || id == ID_STALE) && code == CBN_SELCHANGE) {
                Rebuild();
            } else if (id == ID_CUSTOM && code == EN_KILLFOCUS) {
                Rebuild();
            } else if (code == BN_CLICKED) {
                Act(id);
            }
            return 0;
        }
    }
    return View::OnMessage(m, w, l);
}

}  // namespace ss
