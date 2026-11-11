#include "ui/RecycleBinView.h"

#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <uxtheme.h>

#include "ui/MainWindow.h"
#include "ui/UiCommon.h"
#include "utils/FormatUtils.h"

namespace ss {
namespace {
enum { ID_LIST = 1, ID_REFRESH, ID_ALL, ID_NONE, ID_RESTORE, ID_DELETE, ID_EMPTY, ID_INFO, ID_NOTE };
}  // namespace

void RecycleBinView::OnCreate() {
    bRefresh_ = Make(L"BUTTON", L"Refresh", WS_TABSTOP | BS_OWNERDRAW, ID_REFRESH);
    bAll_ = Make(L"BUTTON", L"Select All", WS_TABSTOP | BS_OWNERDRAW, ID_ALL);
    bNone_ = Make(L"BUTTON", L"Clear Selection", WS_TABSTOP | BS_OWNERDRAW, ID_NONE);
    bRestore_ = Make(L"BUTTON", L"Restore Selected", WS_TABSTOP | BS_OWNERDRAW | WS_DISABLED, ID_RESTORE);
    bDelete_ = Make(L"BUTTON", L"Delete Permanently...", WS_TABSTOP | BS_OWNERDRAW | WS_DISABLED, ID_DELETE);
    bEmpty_ = Make(L"BUTTON", L"Empty Recycle Bin", WS_TABSTOP | BS_OWNERDRAW | WS_DISABLED, ID_EMPTY);
    info_ = Make(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX | SS_ENDELLIPSIS, ID_INFO);
    list_ = Make(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS, ID_LIST);
    SetWindowTheme(list_, L"Explorer", nullptr);
    ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_CHECKBOXES | LVS_EX_LABELTIP);
    struct D {
        const wchar_t* t;
        int w, fmt;
    } defs[] = {{L"Name", 280, LVCFMT_LEFT}, {L"Original location", 400, LVCFMT_LEFT}, {L"Size", 100, LVCFMT_RIGHT}, {L"Deleted", 170, LVCFMT_LEFT}};
    for (int i = 0; i < 4; ++i) {
        LVCOLUMNW c{LVCF_TEXT | LVCF_WIDTH | LVCF_FMT};
        c.pszText = const_cast<wchar_t*>(defs[i].t);
        c.cx = mw_->S(defs[i].w);
        c.fmt = defs[i].fmt;
        SendMessageW(list_, LVM_INSERTCOLUMNW, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(&c));
    }
    note_ = Make(L"STATIC",
                 L"This is the Windows Recycle Bin of all drives. Tick the items you want (or highlight them), then restore them or delete "
                 L"them permanently. Permanently deleted items cannot be recovered.",
                 SS_LEFT | SS_NOPREFIX, ID_NOTE);
}

void RecycleBinView::Layout(int w, int h) {
    int m = mw_->S(12), y = mw_->S(10), bh = mw_->S(32), x = m;
    struct B {
        HWND h;
        int w;
    } bs[] = {{bRefresh_, 90}, {bAll_, 110}, {bNone_, 140}, {bRestore_, 160}, {bDelete_, 200}, {bEmpty_, 170}};
    for (auto& b : bs) {
        if (x + mw_->S(b.w) > w - m && x > m) {  // narrow window: continue on a second row
            x = m;
            y += bh + mw_->S(8);
        }
        MoveWindow(b.h, x, y, mw_->S(b.w), bh, TRUE);
        x += mw_->S(b.w) + mw_->S(8);
    }
    MoveWindow(info_, m, y + bh + mw_->S(8), w - 2 * m, mw_->S(26), TRUE);
    int top = y + bh + mw_->S(40), noteH = mw_->S(50);
    MoveWindow(list_, 0, top, w, std::max(mw_->S(100), h - top - noteH - mw_->S(12)), TRUE);
    MoveWindow(note_, m, h - noteH - mw_->S(6), w - 2 * m, noteH, TRUE);
}

void RecycleBinView::Refresh() {
    filling_ = true;
    bin_.Reload();
    ListView_DeleteAllItems(list_);
    uint64_t total = 0;
    const auto& items = bin_.items();
    for (size_t i = 0; i < items.size(); ++i) {
        const BinItem& b = items[i];
        std::wstring size = b.isDir ? L"" : FormatBytes(b.size), when = FormatFileTime(b.deleted);
        std::wstring name = b.name;
        LVITEMW it{LVIF_TEXT};
        it.iItem = static_cast<int>(i);
        it.pszText = name.data();
        SendMessageW(list_, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&it));
        ListView_SetItemText(list_, static_cast<int>(i), 1, const_cast<wchar_t*>(b.from.c_str()));
        ListView_SetItemText(list_, static_cast<int>(i), 2, size.data());
        ListView_SetItemText(list_, static_cast<int>(i), 3, when.data());
        total += b.size;
    }
    filling_ = false;
    SetWindowTextW(info_, (items.empty() ? std::wstring(L"The Recycle Bin is empty.")
                                         : FormatCount(items.size()) + (items.size() == 1 ? L" item, " : L" items, ") + FormatBytes(total) + L" (folders are listed without a size).")
                              .c_str());
    UpdateButtons();
}

std::vector<size_t> RecycleBinView::Chosen() const {
    std::vector<size_t> out;
    int n = ListView_GetItemCount(list_);
    for (int i = 0; i < n; ++i)
        if (ListView_GetCheckState(list_, i)) out.push_back(static_cast<size_t>(i));
    if (out.empty())
        for (int i = ListView_GetNextItem(list_, -1, LVNI_SELECTED); i >= 0; i = ListView_GetNextItem(list_, i, LVNI_SELECTED))
            out.push_back(static_cast<size_t>(i));
    return out;
}

void RecycleBinView::UpdateButtons() {
    if (filling_) return;
    bool has = !bin_.items().empty(), chosen = !Chosen().empty();
    EnableWindow(bAll_, has);
    EnableWindow(bNone_, has);
    EnableWindow(bRestore_, chosen);
    EnableWindow(bDelete_, chosen);
    EnableWindow(bEmpty_, has);
}

void RecycleBinView::CheckAll(bool on) {
    filling_ = true;
    int n = ListView_GetItemCount(list_);
    for (int i = 0; i < n; ++i) ListView_SetCheckState(list_, i, on);
    filling_ = false;
    UpdateButtons();
}

void RecycleBinView::Restore() {
    auto idx = Chosen();
    if (idx.empty()) return;
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    size_t n = bin_.Restore(idx, mw_->hwnd());
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    Refresh();
    mw_->Toast(n ? L"Restored " + FormatCount(n) + (n == 1 ? L" item" : L" items") : L"Nothing was restored");
    mw_->DataChanged();  // drive space and (with automatic updates) the file lists change
}

void RecycleBinView::Purge() {
    auto idx = Chosen();
    if (idx.empty()) return;
    uint64_t bytes = 0;
    for (size_t i : idx) bytes += bin_.items()[i].size;
    std::wstring what = idx.size() == 1 ? L"\"" + bin_.items()[idx[0]].name + L"\"" : FormatCount(idx.size()) + L" items";
    if (!mw_->Confirm(L"Permanently delete " + what + L" from the Recycle Bin?",
                      what + L"\n" + FormatBytes(bytes) + L"\n\nThis cannot be undone.", L"Delete Permanently\nThe items cannot be recovered", true))
        return;
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    size_t n = bin_.Purge(idx);
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    Refresh();
    mw_->Toast(n ? L"Deleted " + FormatCount(n) + (n == 1 ? L" item" : L" items") + L" permanently" : L"Nothing was deleted");
    mw_->DataChanged();
}

void RecycleBinView::EmptyBin() {
    if (bin_.items().empty()) return;
    uint64_t bytes = 0;
    for (auto& b : bin_.items()) bytes += b.size;
    if (!mw_->Confirm(L"Empty the Recycle Bin?",
                      FormatBytes(bytes) + L" in " + FormatCount(bin_.items().size()) + L" items will be deleted permanently from all drives. This cannot be undone.",
                      L"Empty Recycle Bin", true))
        return;
    HRESULT hr = SHEmptyRecycleBinW(mw_->hwnd(), nullptr, SHERB_NOCONFIRMATION);
    Refresh();
    if (SUCCEEDED(hr)) mw_->Toast(L"Recycle Bin emptied");
    else mw_->ShowMessage(L"The Recycle Bin could not be emptied.", L"Some items may be in use. Try again in a moment.", true);
    mw_->DataChanged();
}

LRESULT RecycleBinView::OnMessage(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_NOTIFY: {
            auto* nm = reinterpret_cast<NMHDR*>(l);
            if (nm->code == NM_CUSTOMDRAW && nm->hwndFrom == ListView_GetHeader(list_)) return HeaderCustomDraw(l);
            if (nm->hwndFrom == list_) {
                if (nm->code == LVN_ITEMCHANGED) UpdateButtons();
                else if (nm->code == LVN_KEYDOWN) {
                    auto* k = reinterpret_cast<NMLVKEYDOWN*>(l);
                    if (k->wVKey == 'A' && GetKeyState(VK_CONTROL) < 0) CheckAll(true);
                    else if (k->wVKey == VK_ESCAPE) CheckAll(false);
                    else if (k->wVKey == VK_DELETE) Purge();
                    else if (k->wVKey == VK_F5) Refresh();
                }
            }
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(w)) {
                case ID_REFRESH: Refresh(); break;
                case ID_ALL: CheckAll(true); break;
                case ID_NONE: CheckAll(false); break;
                case ID_RESTORE: Restore(); break;
                case ID_DELETE: Purge(); break;
                case ID_EMPTY: EmptyBin(); break;
            }
            return 0;
    }
    return View::OnMessage(m, w, l);
}

}  // namespace ss
