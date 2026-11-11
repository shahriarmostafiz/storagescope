#include "ui/DashboardView.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <numeric>

#include "app/History.h"
#include "filesystem/PathUtils.h"
#include "ui/MainWindow.h"
#include "ui/UiCommon.h"
#include "utils/FormatUtils.h"

namespace ss {
namespace {
enum { ID_SCAN = 1, ID_FOLDER, ID_ALL, ID_REFRESH, ID_NOTE, ID_BINTEXT, ID_BIN, ID_LISTF, ID_LISTX };

std::wstring TrendText(int64_t delta, int age) {
    std::wstring when = age == 1 ? L"yesterday" : std::to_wstring(age) + L" days ago";
    if (delta == 0) return L"Free space unchanged since " + when;
    return FormatBytes(static_cast<uint64_t>(delta < 0 ? -delta : delta)) + (delta < 0 ? L" less free than " : L" more free than ") + when;
}

}  // namespace

void DashboardView::OnCreate() {
    bScan_ = Make(L"BUTTON", L"Scan Selected Drive", WS_TABSTOP | BS_OWNERDRAW, ID_SCAN);
    bFolder_ = Make(L"BUTTON", L"Scan Folder...", WS_TABSTOP | BS_OWNERDRAW, ID_FOLDER);
    bAll_ = Make(L"BUTTON", L"Scan All Local Drives", WS_TABSTOP | BS_OWNERDRAW, ID_ALL);
    bRefresh_ = Make(L"BUTTON", L"Refresh", WS_TABSTOP | BS_OWNERDRAW, ID_REFRESH);
    binText_ = Make(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX | SS_ENDELLIPSIS, ID_BINTEXT);
    note_ = Make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, ID_NOTE);
    for (int i = 0; i < 2; ++i) {
        HWND l = Make(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, i == 0 ? ID_LISTF : ID_LISTX);
        SetWindowTheme(l, L"Explorer", nullptr);
        ListView_SetExtendedListViewStyle(l, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        struct C {
            const wchar_t* t;
            int w, fmt;
        } cols[] = {{i == 0 ? L"Folder" : L"File", 150, LVCFMT_LEFT}, {L"Size", 80, LVCFMT_RIGHT}, {L"Location", 230, LVCFMT_LEFT}};
        for (int c = 0; c < 3; ++c) {
            LVCOLUMNW col{LVCF_TEXT | LVCF_WIDTH | LVCF_FMT};
            col.pszText = const_cast<wchar_t*>(cols[c].t);
            col.cx = mw_->S(cols[c].w);
            col.fmt = cols[c].fmt;
            SendMessageW(l, LVM_INSERTCOLUMNW, static_cast<WPARAM>(c), reinterpret_cast<LPARAM>(&col));
        }
        ShowWindow(l, SW_HIDE);
        (i == 0 ? listFolders_ : listFiles_) = l;
    }
    SetWindowLongPtrW(hwnd_, GWL_STYLE, GetWindowLongPtrW(hwnd_, GWL_STYLE) | WS_TABSTOP);  // arrow keys pick a drive card
    EnableScroll();
}

void DashboardView::Layout(int w, int h) {
    if (!BeginLayout(w, h)) return;
    int m = mw_->S(16), bh = mw_->S(32), gap = mw_->S(16), y0 = mw_->S(12) - scrollY_;
    int x = mw_->S(12);
    struct B {
        HWND h;
        int w;
    } bs[] = {{bScan_, 190}, {bFolder_, 140}, {bAll_, 190}, {bRefresh_, 100}};
    for (auto& b : bs) {
        MoveWindow(b.h, x, y0, mw_->S(b.w), bh, TRUE);
        x += mw_->S(b.w) + mw_->S(12);
    }
    titleDrives_ = {m, y0 + mw_->S(44), w - m, y0 + mw_->S(80)};
    int cw = mw_->S(370), ch = mw_->S(160), top = y0 + mw_->S(88);
    int cols = std::max(1, (w - m) / (cw + gap));
    cards_.clear();
    for (size_t i = 0; i < drives_.size(); ++i) {
        int c = static_cast<int>(i) % cols, r = static_cast<int>(i) / cols;
        cards_.push_back({m + c * (cw + gap), top + r * (ch + gap), m + c * (cw + gap) + cw, top + r * (ch + gap) + ch});
    }
    int y = (cards_.empty() ? top : cards_.back().bottom) + gap;

    MoveWindow(binText_, m, y, std::max(mw_->S(200), w - 2 * m), bh, TRUE);
    y += mw_->S(46);
    MoveWindow(note_, m, y, std::max(10, w - 2 * m), mw_->S(26), TRUE);
    y += mw_->S(40);

    legend_.clear();
    if (hasStats_) {
        titleTypes_ = {m, y, w - m, y + mw_->S(34)};
        y += mw_->S(40);
        barRect_ = {m, y, w - m, y + mw_->S(26)};
        y += mw_->S(40);
        int itemW = mw_->S(270), perRow = std::max(1, (w - 2 * m) / itemW), rows = (kCategoryCount + perRow - 1) / perRow;
        for (int i = 0; i < kCategoryCount; ++i)
            legend_.push_back({m + (i % perRow) * itemW, y + (i / perRow) * mw_->S(26), m + (i % perRow) * itemW + itemW,
                               y + (i / perRow) * mw_->S(26) + mw_->S(26)});
        y += rows * mw_->S(26) + mw_->S(20);
        bool two = w >= mw_->S(900);
        int colW = two ? (w - 3 * m) / 2 : w - 2 * m, lh = mw_->S(300);
        titleFolders_ = {m, y, m + colW, y + mw_->S(34)};
        int ly = y + mw_->S(38);
        MoveWindow(listFolders_, m, ly, colW, lh, TRUE);
        if (two) {
            titleFiles_ = {2 * m + colW, y, w - m, y + mw_->S(34)};
            MoveWindow(listFiles_, 2 * m + colW, ly, colW, lh, TRUE);
            y = ly + lh + mw_->S(16);
        } else {
            y = ly + lh + mw_->S(16);
            titleFiles_ = {m, y, w - m, y + mw_->S(34)};
            MoveWindow(listFiles_, m, y + mw_->S(38), colW, lh, TRUE);
            y += mw_->S(38) + lh + mw_->S(16);
        }
    }
    ShowWindow(listFolders_, hasStats_ ? SW_SHOW : SW_HIDE);
    ShowWindow(listFiles_, hasStats_ ? SW_SHOW : SW_HIDE);
    EndLayout(y);
}

void DashboardView::Reload() {
    dirty_ = true;
    if (IsWindowVisible(hwnd_)) Refresh();  // hidden pages are refreshed when shown
}

void DashboardView::DropReferences() {
    stats_ = ScanStats{};
    folderNodes_.clear();
    fileNodes_.clear();
    ListView_DeleteAllItems(listFolders_);
    ListView_DeleteAllItems(listFiles_);
    hasStats_ = false;
    dirty_ = true;
}

void DashboardView::QueryRecycleBin() {
    SHQUERYRBINFO info{sizeof info};
    binBytes_ = binItems_ = 0;
    if (SUCCEEDED(SHQueryRecycleBinW(nullptr, &info))) {
        binBytes_ = static_cast<uint64_t>(info.i64Size);
        binItems_ = static_cast<uint64_t>(info.i64NumItems);
    }
    std::wstring t = L"Recycle Bin: " + FormatBytes(binBytes_) + L" in " + FormatCount(binItems_) +
                     (binItems_ == 1 ? L" item." : L" items.") + L" Moving items there does not free space until it is emptied.";
    SetWindowTextW(binText_, t.c_str());
}

void DashboardView::RefreshStats() {
    ScanResult* sc = mw_->scan();
    stats_ = ScanStats{};
    folderNodes_.clear();
    fileNodes_.clear();
    ListView_DeleteAllItems(listFolders_);
    ListView_DeleteAllItems(listFiles_);
    const SnapshotSummary* sv = sc ? nullptr : mw_->SavedSummary();  // the remembered scan, while its full tree is still loading
    hasStats_ = sc || sv;
    if (!hasStats_) return;
    auto add = [&](HWND list, int i, std::wstring name, uint64_t bytes, std::wstring loc) {
        std::wstring size = FormatBytes(bytes);
        LVITEMW it{LVIF_TEXT};
        it.iItem = i;
        it.pszText = name.data();
        SendMessageW(list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&it));
        ListView_SetItemText(list, i, 1, size.data());
        ListView_SetItemText(list, i, 2, loc.data());
    };
    if (sc) {
        const Settings& s = mw_->settings();
        stats_ = ComputeStats(sc->root.get(), s.showHidden, false, 10);  // one pass over the in-memory index
        auto fill = [&](HWND list, const std::vector<TopItem>& items, std::vector<ScanNode*>& nodes) {
            for (size_t i = 0; i < items.size(); ++i) {
                ScanNode* n = items[i].node;
                nodes.push_back(n);
                add(list, static_cast<int>(i), DisplayName(n), items[i].size, path::Parent(BuildPath(n)));
            }
        };
        fill(listFolders_, stats_.topFolders, folderNodes_);
        fill(listFiles_, stats_.topFiles, fileNodes_);
    } else {
        for (int c = 0; c < kCategoryCount; ++c) {
            stats_.bytes[c] = sv->catBytes[c];
            stats_.files[c] = sv->catFiles[c];
        }
        auto fill = [&](HWND list, const std::vector<SavedTop>& items, std::vector<ScanNode*>& nodes) {
            for (size_t i = 0; i < items.size(); ++i) {
                nodes.push_back(nullptr);  // no tree yet: double-click does nothing until it is loaded
                add(list, static_cast<int>(i), items[i].name, items[i].size, items[i].location);
            }
        };
        fill(listFolders_, sv->topFolders, folderNodes_);
        fill(listFiles_, sv->topFiles, fileNodes_);
    }
    // legend order: largest category first
    legendCat_.resize(kCategoryCount);
    std::iota(legendCat_.begin(), legendCat_.end(), 0);
    std::sort(legendCat_.begin(), legendCat_.end(), [&](int a, int b) { return stats_.bytes[a] > stats_.bytes[b]; });
}

void DashboardView::Refresh() {
    drives_ = DriveManager::Enumerate();  // live values: also reflects space freed by deletions
    int today = History::Today();
    if (mw_->settings().keepHistory) History::Record(drives_, today);
    trends_.assign(drives_.size(), Trend{});
    if (mw_->settings().keepHistory)
        for (size_t i = 0; i < drives_.size(); ++i) {
            const DriveInfo& d = drives_[i];
            if (d.IsFixed() && d.ready) trends_[i].valid = History::Delta(d.letter, today, d.free, 7, trends_[i].delta, trends_[i].age);
        }
    if (sel_ < 0 || sel_ >= static_cast<int>(drives_.size())) {
        sel_ = -1;
        for (size_t i = 0; i < drives_.size(); ++i)
            if (drives_[i].IsFixed()) {
                sel_ = static_cast<int>(i);
                break;
            }
    }
    QueryRecycleBin();
    if (dirty_) {
        RefreshStats();
        dirty_ = false;
    }
    SetWindowTextW(note_, mw_->LastScanSummary().empty() ? L"No scan has been performed. Choose a drive and press \"Scan Selected Drive\"."
                                                         : mw_->LastScanSummary().c_str());
    RECT rc;
    GetClientRect(hwnd_, &rc);
    Layout(rc.right, rc.bottom);
    UpdateButtons();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void DashboardView::UpdateButtons() {
    bool idle = !mw_->busy();
    EnableWindow(bScan_, idle && sel_ >= 0 && drives_[static_cast<size_t>(sel_)].ready);
    EnableWindow(bFolder_, idle);
    EnableWindow(bAll_, idle);
}

void DashboardView::ScanSelected() {
    if (sel_ < 0) return;
    const DriveInfo& d = drives_[static_cast<size_t>(sel_)];
    if (!d.ready) {
        mw_->ShowMessage(L"Drive not ready.", L"The drive " + d.root + L" is not available.", true);
        return;
    }
    mw_->StartScan({d.root});
}

void DashboardView::ScanAll() {
    std::vector<std::wstring> roots;
    for (auto& d : drives_)
        if (d.IsFixed() && d.ready) roots.push_back(d.root);  // network / removable / optical are never included implicitly
    if (roots.empty()) {
        mw_->ShowMessage(L"No local drives were found.", L"", true);
        return;
    }
    mw_->StartScan(roots);
}

void DashboardView::ScanFolder() {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"Choose a folder to scan");
    std::wstring path;
    if (SUCCEEDED(dlg->Show(mw_->hwnd()))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(dlg->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                path = p;
                CoTaskMemFree(p);
            }
            it->Release();
        }
    }
    dlg->Release();
    if (!path.empty()) mw_->StartScan({path});
}

void DashboardView::OpenTop(HWND list) {
    int i = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    auto& nodes = list == listFolders_ ? folderNodes_ : fileNodes_;
    if (i < 0 || i >= static_cast<int>(nodes.size())) return;
    ScanNode* n = nodes[static_cast<size_t>(i)];
    if (n && list == listFiles_) n = n->parent;  // files: show the folder that holds them
    if (n) mw_->BrowseTo(n);
}

void DashboardView::Paint() {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd_, &ps);
    RECT rc;
    GetClientRect(hwnd_, &rc);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ ob = SelectObject(mem, bmp);
    FillRect(mem, &rc, BgBrush());
    SetBkMode(mem, TRANSPARENT);
    const Theme& c = Colors();

    auto title = [&](const RECT& r, const wchar_t* t) {
        SelectObject(mem, mw_->titleFont());
        SetTextColor(mem, c.text);
        RECT tr = r;
        DrawTextW(mem, t, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    };
    title(titleDrives_, L"Drives");

    int pad = mw_->S(14);
    for (size_t i = 0; i < drives_.size() && i < cards_.size(); ++i) {
        const DriveInfo& d = drives_[i];
        RECT r = cards_[i];
        bool sel = static_cast<int>(i) == sel_;
        // glass card: soft vertical gradient, bright top edge, thin border
        int rad = mw_->S(16);
        HRGN rg = CreateRoundRectRgn(r.left, r.top, r.right + 1, r.bottom + 1, rad, rad);
        SelectClipRgn(mem, rg);
        auto vtx = [](LONG x, LONG y, COLORREF col) {
            return TRIVERTEX{x, y, static_cast<COLOR16>(GetRValue(col) << 8), static_cast<COLOR16>(GetGValue(col) << 8),
                             static_cast<COLOR16>(GetBValue(col) << 8), 0};
        };
        TRIVERTEX tv[2] = {vtx(r.left, r.top, c.cardTop), vtx(r.right, r.bottom, c.cardBot)};
        GRADIENT_RECT gr{0, 1};
        GradientFill(mem, tv, 2, &gr, 1, GRADIENT_FILL_RECT_V);
        SelectClipRgn(mem, nullptr);
        DeleteObject(rg);
        HPEN pen = CreatePen(PS_SOLID, sel ? mw_->S(2) : 1, sel ? c.accent : c.cardBorder);
        HGDIOBJ op = SelectObject(mem, pen), obr = SelectObject(mem, GetStockObject(NULL_BRUSH));
        RoundRect(mem, r.left, r.top, r.right, r.bottom, rad, rad);
        SelectObject(mem, op);
        SelectObject(mem, obr);
        DeleteObject(pen);
        HPEN hi = CreatePen(PS_SOLID, 1, c.dark ? RGB(88, 94, 108) : RGB(255, 255, 255));  // top highlight
        HGDIOBJ oh = SelectObject(mem, hi);
        MoveToEx(mem, r.left + rad / 2, r.top + 1, nullptr);
        LineTo(mem, r.right - rad / 2, r.top + 1);
        SelectObject(mem, oh);
        DeleteObject(hi);

        std::wstring head = std::wstring(1, d.letter) + L":";
        if (!d.label.empty()) head += L"  " + d.label;
        SelectObject(mem, mw_->boldFont());
        SetTextColor(mem, c.text);
        RECT l1{r.left + pad, r.top + pad, r.right - pad, r.top + pad + mw_->S(22)};
        DrawTextW(mem, head.c_str(), -1, &l1, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

        SelectObject(mem, mw_->font());
        SetTextColor(mem, c.muted);
        RECT l2{r.left + pad, l1.bottom, r.right - pad, l1.bottom + mw_->S(20)};
        std::wstring typeLine = d.TypeText() + (mw_->IsIgnoredPath(d.root) ? L"  -  ignored (Settings)" : L"");
        DrawTextW(mem, typeLine.c_str(), -1, &l2, DT_LEFT | DT_SINGLELINE);

        if (d.ready && d.total) {
            RECT bar{r.left + pad, l2.bottom + mw_->S(8), r.right - pad, l2.bottom + mw_->S(8) + mw_->S(10)};
            HBRUSH bb = CreateSolidBrush(c.barBg);
            FillRect(mem, &bar, bb);
            DeleteObject(bb);
            double frac = static_cast<double>(d.Used()) / static_cast<double>(d.total);
            RECT fill = bar;
            fill.right = bar.left + static_cast<LONG>((bar.right - bar.left) * frac);
            HBRUSH fb = CreateSolidBrush(frac > 0.9 ? c.danger : c.accent);
            FillRect(mem, &fill, fb);
            DeleteObject(fb);
            std::wstring t = L"Used: " + FormatBytes(d.Used()) + L"   Free: " + FormatBytes(d.free) + L"   Total: " + FormatBytes(d.total);
            SetTextColor(mem, c.text);
            RECT l3{r.left + pad, bar.bottom + mw_->S(8), r.right - pad, bar.bottom + mw_->S(8) + mw_->S(24)};
            DrawTextW(mem, t.c_str(), -1, &l3, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            if (i < trends_.size() && trends_[i].valid) {  // free space compared with ~a week ago
                SetTextColor(mem, trends_[i].delta < 0 ? c.danger : (trends_[i].delta > 0 ? c.success : c.muted));
                RECT l4{r.left + pad, l3.bottom, r.right - pad, r.bottom - mw_->S(4)};
                std::wstring tt = TrendText(trends_[i].delta, trends_[i].age);
                DrawTextW(mem, tt.c_str(), -1, &l4, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
        } else {
            SetTextColor(mem, d.type == DRIVE_REMOTE ? c.muted : c.danger);
            RECT l3{r.left + pad, l2.bottom + mw_->S(10), r.right - pad, r.bottom};
            DrawTextW(mem, d.type == DRIVE_REMOTE ? L"Network drive: size not queried" : L"Not ready / unavailable", -1, &l3,
                      DT_LEFT | DT_SINGLELINE);
        }
    }

    if (hasStats_) {
        title(titleTypes_, L"Usage by file type");
        title(titleFolders_, L"Biggest folders (by the files directly inside)");
        title(titleFiles_, L"Biggest files");
        uint64_t total = stats_.TotalBytes();
        if (total > 0) {
            int rad = mw_->S(10);
            HRGN rg = CreateRoundRectRgn(barRect_.left, barRect_.top, barRect_.right + 1, barRect_.bottom + 1, rad, rad);
            SelectClipRgn(mem, rg);
            HBRUSH bg = CreateSolidBrush(c.barBg);
            FillRect(mem, &barRect_, bg);
            DeleteObject(bg);
            double x = barRect_.left, width = barRect_.right - barRect_.left;
            for (int cat : legendCat_) {
                if (!stats_.bytes[cat]) continue;
                double seg = width * static_cast<double>(stats_.bytes[cat]) / static_cast<double>(total);
                RECT sr{static_cast<LONG>(x), barRect_.top, static_cast<LONG>(x + seg) , barRect_.bottom};
                HBRUSH sb = CreateSolidBrush(CategoryColor(static_cast<FileCategory>(cat)));
                FillRect(mem, &sr, sb);
                DeleteObject(sb);
                x += seg;
            }
            SelectClipRgn(mem, nullptr);
            DeleteObject(rg);
        }
        SelectObject(mem, mw_->font());
        for (size_t i = 0; i < legend_.size() && i < legendCat_.size(); ++i) {
            int cat = legendCat_[i];
            RECT r = legend_[i];
            RECT sw{r.left, r.top + mw_->S(6), r.left + mw_->S(14), r.top + mw_->S(20)};
            HBRUSH sb = CreateSolidBrush(CategoryColor(static_cast<FileCategory>(cat)));
            FillRect(mem, &sw, sb);
            DeleteObject(sb);
            wchar_t pct[16];
            swprintf_s(pct, L"%.0f%%", total ? 100.0 * static_cast<double>(stats_.bytes[cat]) / static_cast<double>(total) : 0.0);
            std::wstring t = std::wstring(CategoryName(static_cast<FileCategory>(cat))) + L"  " + FormatBytes(stats_.bytes[cat]) + L"  (" + pct + L")";
            SetTextColor(mem, c.text);
            RECT tr{r.left + mw_->S(22), r.top, r.right, r.bottom};
            DrawTextW(mem, t.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, ob);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd_, &ps);
}

LRESULT DashboardView::OnMessage(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            Paint();
            return 0;
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_KEYDOWN:
            if (!drives_.empty() && (w == VK_LEFT || w == VK_RIGHT || w == VK_UP || w == VK_DOWN)) {
                int d = (w == VK_LEFT || w == VK_UP) ? -1 : 1;
                sel_ = std::clamp(sel_ + d, 0, static_cast<int>(drives_.size()) - 1);
                InvalidateRect(hwnd_, nullptr, FALSE);
                UpdateButtons();
                return 0;
            }
            break;
        case WM_LBUTTONDOWN: {
            POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            for (size_t i = 0; i < cards_.size(); ++i)
                if (PtInRect(&cards_[i], p)) {
                    sel_ = static_cast<int>(i);
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    UpdateButtons();
                }
            SetFocus(hwnd_);
            return 0;
        }
        case WM_NOTIFY: {
            auto* nm = reinterpret_cast<NMHDR*>(l);
            if (nm->hwndFrom == listFolders_ || nm->hwndFrom == listFiles_) {
                if (nm->code == NM_DBLCLK) OpenTop(nm->hwndFrom);
                else if (nm->code == LVN_KEYDOWN && reinterpret_cast<NMLVKEYDOWN*>(l)->wVKey == VK_RETURN) OpenTop(nm->hwndFrom);
            }
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(w)) {
                case ID_SCAN: ScanSelected(); break;
                case ID_FOLDER: ScanFolder(); break;
                case ID_ALL: ScanAll(); break;
                case ID_REFRESH:
                    dirty_ = true;
                    Refresh();
                    break;
            }
            return 0;
    }
    return View::OnMessage(m, w, l);
}

}  // namespace ss
