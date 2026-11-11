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

enum { ID_SIZE = 1, ID_CUSTOM, ID_TYPE, ID_LIST, ID_INFO, ID_EMPTY, ID_OPEN, ID_RECYCLE, ID_PERM, ID_NOTE, ID_SORT, ID_DIR, ID_REFRESH, ID_STALE, ID_BASKET };
const int kSortCols[] = {1 /*size*/, 0 /*name*/, 2 /*type*/, 3 /*location*/, 4 /*modified*/};  // combo order -> Col
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
    for (const wchar_t* s : {L"100 MB", L"500 MB", L"1 GB", L"5 GB", L"Custom"})
        SendMessageW(cbSize_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
    SendMessageW(cbSize_, CB_SETCURSEL, 0, 0);
    edCustom_ = Make(L"EDIT", L"250", WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, ID_CUSTOM, WS_EX_CLIENTEDGE);
    lblMb_ = Make(L"STATIC", L"MB", SS_LEFT | SS_CENTERIMAGE, 0);
    EnableWindow(edCustom_, FALSE);
    lblType_ = Make(L"STATIC", L"Type:", SS_LEFT | SS_CENTERIMAGE, 0);
    cbType_ = Make(L"SSCombo", L"File type", WS_TABSTOP, ID_TYPE);
    SendMessageW(cbType_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"All types"));
    for (int i = 0; i < kCategoryCount; ++i)
        SendMessageW(cbType_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(CategoryName(static_cast<FileCategory>(i))));
    SendMessageW(cbType_, CB_SETCURSEL, 0, 0);
    lblStale_ = Make(L"STATIC", L"Last modified:", SS_LEFT | SS_CENTERIMAGE, 0);
    cbStale_ = Make(L"SSCombo", L"Last modified", WS_TABSTOP, ID_STALE);  // old and big = the safest cleanup candidates
    for (const wchar_t* s : {L"Any time", L"Older than 6 months", L"Older than 1 year", L"Older than 2 years"})
        SendMessageW(cbStale_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
    SendMessageW(cbStale_, CB_SETCURSEL, 0, 0);
    bBasket_ = Make(L"BUTTON", L"Add to Basket", WS_TABSTOP | BS_OWNERDRAW, ID_BASKET);

    sortCb_ = Make(L"SSCombo", L"Sort by", WS_TABSTOP, ID_SORT);
    for (const wchar_t* s : {L"Sort: Size", L"Sort: Name", L"Sort: Type", L"Sort: Location", L"Sort: Date modified"})
        SendMessageW(sortCb_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
    dirBtn_ = Make(L"BUTTON", L"Descending", WS_TABSTOP | BS_OWNERDRAW, ID_DIR);
    SyncSortUi();
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
    info_ = Make(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX, ID_INFO);
    empty_ = Make(L"STATIC", L"", SS_CENTER | SS_NOPREFIX, ID_EMPTY);
    bOpen_ = Make(L"BUTTON", L"Open File Location", WS_TABSTOP | BS_OWNERDRAW, ID_OPEN);
    bRecycle_ = Make(L"BUTTON", L"Recycle Selected", WS_TABSTOP | BS_OWNERDRAW, ID_RECYCLE);
    bPerm_ = Make(L"BUTTON", L"Permanent Delete Selected", WS_TABSTOP | BS_OWNERDRAW, ID_PERM);
    bRefresh_ = Make(L"BUTTON", L"Refresh", WS_TABSTOP | BS_OWNERDRAW, ID_REFRESH);  // drops vanished files now, then rescans (also F5)
    note_ = Make(L"STATIC", L"File size and extension do not tell you whether a file is safe to delete. Check before removing anything.",
                 SS_LEFT | SS_NOPREFIX, ID_NOTE);
}

void LargeFilesView::Layout(int w, int h) {
    int m = mw_->S(12), y = mw_->S(10), ch = mw_->S(28);
    int x = m;
    auto put = [&](HWND hh, int ww) {
        MoveWindow(hh, x, y, mw_->S(ww), ch, TRUE);
        x += mw_->S(ww) + mw_->S(8);
    };
    put(lblSize_, 90);
    put(cbSize_, 110);
    put(edCustom_, 70);
    put(lblMb_, 30);
    x += mw_->S(16);
    put(lblType_, 40);
    put(cbType_, 140);
    // second row: stale filter on the left, sort on the right, result summary in between
    y += mw_->S(44);
    x = m;
    put(lblStale_, 110);
    put(cbStale_, 230);
    int dw = mw_->S(104), cbw = mw_->S(190);
    MoveWindow(dirBtn_, w - m - dw, y - mw_->S(2), dw, ch + mw_->S(4), TRUE);
    MoveWindow(sortCb_, w - m - dw - m - cbw, y, cbw, mw_->S(240), TRUE);
    MoveWindow(info_, x + mw_->S(8), y, std::max(10, w - m - dw - m - cbw - m - x - mw_->S(8)), ch, TRUE);
    int top = mw_->S(96), bottom = h - mw_->S(88);
    MoveWindow(list_, 0, top, w, std::max(0, bottom - top), TRUE);
    MoveWindow(note_, m, bottom + mw_->S(6), w - 2 * m, mw_->S(22), TRUE);
    int by = h - mw_->S(48), bx = m;
    struct B {
        HWND h;
        int w;
    } bs[] = {{bOpen_, 170}, {bBasket_, 150}, {bRecycle_, 150}, {bPerm_, 210}, {bRefresh_, 110}};
    for (auto& b : bs) {
        MoveWindow(b.h, bx, by, mw_->S(b.w), mw_->S(32), TRUE);
        bx += mw_->S(b.w) + mw_->S(8);
    }
    int ew = std::min(w - 2 * m, mw_->S(520));
    MoveWindow(empty_, (w - ew) / 2, top + mw_->S(60), ew, mw_->S(48), TRUE);
}

void LargeFilesView::SyncSortUi() {
    for (int i = 0; i < 5; ++i)
        if (kSortCols[i] == sortCol_) SendMessageW(sortCb_, CB_SETCURSEL, static_cast<WPARAM>(i), 0);
    SetWindowTextW(dirBtn_, sortAsc_ ? L"Ascending" : L"Descending");
}

void LargeFilesView::DropReferences() {
    rows_.clear();
    checked_.clear();
    ListView_SetItemCountEx(list_, 0, 0);
}

uint64_t LargeFilesView::Threshold() const {
    static const uint64_t mb = 1024ull * 1024ull;
    switch (SendMessageW(cbSize_, CB_GETCURSEL, 0, 0)) {
        case 0: return 100 * mb;
        case 1: return 500 * mb;
        case 2: return 1024 * mb;
        case 3: return 5 * 1024 * mb;
        default: {
            wchar_t b[16];
            GetWindowTextW(edCustom_, b, 16);
            return std::max<uint64_t>(1, _wcstoui64(b, nullptr, 10)) * mb;
        }
    }
}

void LargeFilesView::Rebuild() {
    rows_.clear();
    ScanResult* sc = mw_->scan();
    int cat = static_cast<int>(SendMessageW(cbType_, CB_GETCURSEL, 0, 0));  // 0 = all, else FileCategory + 1
    uint64_t total = 0;
    uint64_t staleCutoff = ~0ull;  // files modified at or before this FILETIME qualify
    {
        static const int kDays[] = {0, 183, 365, 730};
        int si = static_cast<int>(SendMessageW(cbStale_, CB_GETCURSEL, 0, 0));
        if (si > 0 && si < 4) {
            FILETIME now;
            GetSystemTimeAsFileTime(&now);
            uint64_t t = (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
            staleCutoff = t - static_cast<uint64_t>(kDays[si]) * 864000000000ull;
        }
    }
    if (sc) {
        uint64_t minSize = Threshold();
        std::vector<ScanNode*> stack{sc->root.get()};  // walks the existing scan index; no disk access
        while (!stack.empty()) {
            ScanNode* n = stack.back();
            stack.pop_back();
            if (!n->dir) continue;
            for (auto& c : n->dir->children) {
                if (c.removed) continue;
                if (c.attrs & FILE_ATTRIBUTE_SYSTEM) continue;  // system files are never shown
                if (!mw_->settings().showHidden && (c.attrs & FILE_ATTRIBUTE_HIDDEN)) continue;
                if (c.type == NodeType::Directory) stack.push_back(&c);
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
            case C_MOD: c = a->modified < b->modified ? -1 : a->modified > b->modified ? 1 : 0; break;
            default: c = a->size < b->size ? -1 : a->size > b->size ? 1 : 0;
        }
        return sortAsc_ ? c < 0 : c > 0;
    });
    ListView_SetItemCountEx(list_, static_cast<int>(rows_.size()), 0);
    InvalidateRect(list_, nullptr, FALSE);
    bool none = rows_.empty();
    ShowWindow(list_, none ? SW_HIDE : SW_SHOW);
    ShowWindow(empty_, none ? SW_SHOW : SW_HIDE);
    if (none) SetWindowTextW(empty_, !sc ? mw_->EmptyStateText().c_str() : L"No files matching this filter were found.");
    SetWindowTextW(info_, sc ? (FormatCount(rows_.size()) + L" files, " + FormatBytes(total) + L" in total").c_str() : L"");
    UpdateButtons();
}

std::wstring LargeFilesView::CellText(ScanNode* n, int col) const {
    switch (col) {
        case C_NAME: return n->name;
        case C_SIZE: return FormatBytes(n->size);
        case C_TYPE: return CategoryName(Categorize(n->name));
        case C_LOC: return path::Parent(BuildPath(n));
        case C_MOD: return FormatFileTime(n->modified);
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
    auto sel = Selected();
    bool ok = !sel.empty() && !mw_->busy();
    if (ok && sel.size() <= 300)
        for (auto* n : sel)
            if (mw_->protection().Classify(BuildPath(n)).level >= ProtectionLevel::NoDelete) ok = false;
    EnableWindow(bOpen_, sel.size() == 1);
    EnableWindow(bRecycle_, ok);
    EnableWindow(bPerm_, ok);
    EnableWindow(bRefresh_, mw_->scan() && !mw_->busy());
    EnableWindow(bBasket_, ok);
}

void LargeFilesView::Act(int id) {
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
            if (id == ID_SIZE && code == CBN_SELCHANGE) {
                EnableWindow(edCustom_, SendMessageW(cbSize_, CB_GETCURSEL, 0, 0) == 4);
                Rebuild();
            } else if (id == ID_SORT && code == CBN_SELCHANGE) {
                int i = static_cast<int>(SendMessageW(sortCb_, CB_GETCURSEL, 0, 0));
                if (i >= 0 && i < 5) {
                    sortCol_ = kSortCols[i];
                    sortAsc_ = (sortCol_ == C_NAME || sortCol_ == C_TYPE || sortCol_ == C_LOC);  // size/date: biggest/newest first
                    SyncSortUi();
                    Rebuild();
                }
            } else if (id == ID_DIR && code == BN_CLICKED) {
                sortAsc_ = !sortAsc_;
                SyncSortUi();
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
