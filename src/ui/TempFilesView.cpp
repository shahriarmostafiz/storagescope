#include "ui/TempFilesView.h"

#include <commctrl.h>

#include <algorithm>
#include <uxtheme.h>

#include "ui/MainWindow.h"
#include "ui/UiCommon.h"
#include "utils/FormatUtils.h"

namespace ss {
namespace {
enum { ID_LIST = 1, ID_AGE, ID_SCAN, ID_CLEAN, ID_TOTAL, ID_RESULT, ID_NOTE };
const uint32_t kAges[] = {24, 168, 720, 0};
}  // namespace

TempFilesView::~TempFilesView() = default;

void TempFilesView::OnCreate() {
    bScan_ = Make(L"BUTTON", L"Scan Temporary Files", WS_TABSTOP | BS_OWNERDRAW, ID_SCAN);
    lblAge_ = Make(L"STATIC", L"Delete files:", SS_LEFT | SS_CENTERIMAGE, 0);
    cbAge_ = Make(L"SSCombo", L"Delete files older than", WS_TABSTOP, ID_AGE);
    for (const wchar_t* s : {L"Older than 24 hours", L"Older than 7 days", L"Older than 30 days", L"All eligible files"})
        SendMessageW(cbAge_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
    int sel = 0;
    for (int i = 0; i < 4; ++i)
        if (kAges[i] == mw_->settings().tempAgeHours) sel = i;
    SendMessageW(cbAge_, CB_SETCURSEL, static_cast<WPARAM>(sel), 0);
    bClean_ = Make(L"BUTTON", L"Clean Selected", WS_TABSTOP | BS_OWNERDRAW | WS_DISABLED, ID_CLEAN);
    total_ = Make(L"STATIC", L"Recoverable space: not scanned yet", SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX, ID_TOTAL);

    list_ = Make(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, ID_LIST);
    SetWindowTheme(list_, L"Explorer", nullptr);
    ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_CHECKBOXES | LVS_EX_LABELTIP);
    struct D {
        const wchar_t* t;
        int w, fmt;
    } defs[] = {{L"Category", 230, LVCFMT_LEFT}, {L"Location", 430, LVCFMT_LEFT}, {L"Files", 90, LVCFMT_RIGHT},
                {L"Size", 110, LVCFMT_RIGHT}, {L"Risk", 90, LVCFMT_LEFT}};
    for (int i = 0; i < 5; ++i) {
        LVCOLUMNW c{LVCF_TEXT | LVCF_WIDTH | LVCF_FMT};
        c.pszText = const_cast<wchar_t*>(defs[i].t);
        c.cx = mw_->S(defs[i].w);
        c.fmt = defs[i].fmt;
        SendMessageW(list_, LVM_INSERTCOLUMNW, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(&c));
    }
    result_ = Make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, ID_RESULT);
    note_ = Make(L"SSInfo",
                 L"Only user-level temp folders are listed. Cleaned files are deleted permanently (not sent to the Recycle Bin). "
                 L"Files that are in use are skipped. Windows system folders, WinSxS, drivers, browser profiles and the registry are never touched.",
                 WS_TABSTOP, ID_NOTE);
}

void TempFilesView::Layout(int w, int h) {
    int m = mw_->S(12), y = mw_->S(10), bh = mw_->S(32), gap = mw_->S(10);
    // Toolbar: the scan button, "Delete files: [age]" as one group, and Clean; wraps onto a second row in a narrow window.
    int x = m;
    struct It {
        HWND h;
        int w, dy, hh;
    };
    auto group = [&](std::initializer_list<It> items) {
        int total = 0;
        for (auto& it : items) total += mw_->S(it.w) + gap;
        if (x + total - gap > w - m && x > m) {
            x = m;
            y += bh + mw_->S(8);
        }
        for (auto& it : items) {
            MoveWindow(it.h, x, y + it.dy, mw_->S(it.w), it.hh, TRUE);
            x += mw_->S(it.w) + gap;
        }
        x += mw_->S(10);
    };
    group({{bScan_, 220, 0, bh}});
    group({{lblAge_, 100, 0, bh}, {cbAge_, 230, -mw_->S(1), mw_->S(200)}});
    group({{bClean_, 170, 0, bh}});
    MoveWindow(total_, m, y + bh + mw_->S(8), w - 2 * m, mw_->S(26), TRUE);
    int top = y + bh + mw_->S(40);
    int ic = mw_->S(kInfoIconSize), noteH = ic + mw_->S(12);  // the safety note is the tooltip of one "i" icon at the bottom
    int listH = std::max(mw_->S(150), h - top - noteH - mw_->S(76));  // the list takes the free height; the result sits under it
    MoveWindow(list_, 0, top, w, listH, TRUE);
    MoveWindow(result_, m, top + listH + mw_->S(8), w - 2 * m, mw_->S(56), TRUE);
    MoveWindow(note_, m, h - noteH, ic, ic, TRUE);
    int avail = std::max(mw_->S(500), w - mw_->S(24));  // columns share the width
    const int pct[5] = {24, 40, 10, 14, 12};
    for (int i = 0; i < 4; ++i) ListView_SetColumnWidth(list_, i, avail * pct[i] / 100);
    ListView_SetColumnWidth(list_, 4, LVSCW_AUTOSIZE_USEHEADER);  // the last column takes the rest: no empty header stub
}

uint32_t TempFilesView::AgeHours() const {
    int i = static_cast<int>(SendMessageW(cbAge_, CB_GETCURSEL, 0, 0));
    return kAges[(i < 0 || i > 3) ? 0 : i];
}

bool TempFilesView::Checked(int i) const { return ListView_GetCheckState(list_, i) != 0; }

void TempFilesView::Discover() {
    TempCleanup tc(mw_->protection(), mw_->settings().ignored);
    cats_.clear();
    for (auto& c : tc.DiscoverCategories()) cats_.push_back({c, {}, false});
    FillList();
}

void TempFilesView::FillList() {
    filling_ = true;
    ListView_DeleteAllItems(list_);
    uint64_t total = 0;
    for (size_t i = 0; i < cats_.size(); ++i) {
        const Row& r = cats_[i];
        LVITEMW it{LVIF_TEXT};
        it.iItem = static_cast<int>(i);
        it.pszText = const_cast<wchar_t*>(r.cat.name.c_str());
        SendMessageW(list_, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&it));
        ListView_SetItemText(list_, static_cast<int>(i), 1, const_cast<wchar_t*>(r.cat.path.c_str()));
        std::wstring files = r.scanned ? FormatCount(r.res.files) : L"-";
        std::wstring size = r.scanned ? FormatBytes(r.res.bytes) : L"-";
        ListView_SetItemText(list_, static_cast<int>(i), 2, files.data());
        ListView_SetItemText(list_, static_cast<int>(i), 3, size.data());
        ListView_SetItemText(list_, static_cast<int>(i), 4, const_cast<wchar_t*>(L"Low"));
        if (r.scanned) ListView_SetCheckState(list_, static_cast<int>(i), TRUE);
        if (r.scanned) total += r.res.bytes;
    }
    filling_ = false;
    bool any = false;
    for (auto& r : cats_) any = any || r.scanned;
    SetWindowTextW(total_, any ? (L"Recoverable space: " + FormatBytes(total)).c_str()
                               : (cats_.empty() ? L"No known temporary folders were found." : L"Recoverable space: not scanned yet"));
    UpdateButtons();
}

void TempFilesView::UpdateButtons() {
    bool idle = !working_ && !mw_->busy();
    EnableWindow(bScan_, idle && !cats_.empty());
    EnableWindow(cbAge_, !working_);
    bool any = false, scanned = false;
    uint64_t total = 0;
    {
        std::lock_guard lk(m_);  // the worker thread writes cats_[i].res under this lock
        for (size_t i = 0; i < cats_.size(); ++i) {
            scanned = scanned || cats_[i].scanned;
            if (cats_[i].scanned && Checked(static_cast<int>(i))) {
                any = any || cats_[i].res.files > 0;
                total += cats_[i].res.bytes;
            }
        }
    }
    EnableWindow(bClean_, idle && any);
    if (scanned && !filling_) SetWindowTextW(total_, (L"Recoverable space: " + FormatBytes(total)).c_str());
}

void TempFilesView::StartScan(bool afterClean) {
    if (working_) return;
    working_ = true;
    if (!afterClean) cleanedNote_.clear();
    UpdateButtons();
    SetWindowTextW(result_, (cleanedNote_.empty() ? L"Scanning temporary files..." : cleanedNote_ + L"\r\nRefreshing the list...").c_str());
    uint32_t hours = AgeHours();
    std::vector<TempCategory> cats;
    for (auto& r : cats_) cats.push_back(r.cat);
    th_ = std::jthread([this, hours, cats, ignored = mw_->settings().ignored](std::stop_token st) {
        TempCleanup tc(mw_->protection(), ignored);
        std::vector<TempScanResult> out;
        for (auto& c : cats) out.push_back(tc.Scan(c, hours, st));
        {
            std::lock_guard lk(m_);
            for (size_t i = 0; i < out.size() && i < cats_.size(); ++i) cats_[i].res = out[i];  // UI does not read until DONE
        }
        PostMessageW(hwnd_, WM_APP_TEMP_SCAN_DONE, 0, 0);
    });
}

void TempFilesView::StartClean() {
    if (working_) return;
    std::vector<TempCategory> todo;
    std::wstring names;
    for (size_t i = 0; i < cats_.size(); ++i)
        if (cats_[i].scanned && Checked(static_cast<int>(i))) {
            todo.push_back(cats_[i].cat);
            names += L"\n  " + cats_[i].cat.name;
        }
    if (todo.empty()) return;
    static const wchar_t* ageText[] = {L"older than 24 hours", L"older than 7 days", L"older than 30 days", L"all eligible files"};
    int ai = static_cast<int>(SendMessageW(cbAge_, CB_GETCURSEL, 0, 0));
    if (!mw_->Confirm(L"Clean the selected temporary files?",
                      L"Categories:" + names + L"\n\nFiles: " + ageText[(ai < 0 || ai > 3) ? 0 : ai] +
                          L"\n\nFiles are deleted permanently. Files that are currently in use are skipped.",
                      L"Clean", false))
        return;
    working_ = true;
    UpdateButtons();
    SetWindowTextW(result_, L"Cleaning...");
    uint32_t hours = AgeHours();
    th_ = std::jthread([this, hours, todo, ignored = mw_->settings().ignored](std::stop_token st) {
        TempCleanup tc(mw_->protection(), ignored);
        TempCleanResult sum;
        for (auto& c : todo) {
            TempCleanResult r = tc.Clean(c, hours, st);  // re-validates the folder before deleting anything
            sum.cleanedFiles += r.cleanedFiles;
            sum.cleanedBytes += r.cleanedBytes;
            sum.skippedFiles += r.skippedFiles;
            sum.skippedBytes += r.skippedBytes;
            sum.inUseFiles += r.inUseFiles;
            sum.rejected = sum.rejected || r.rejected;
        }
        {
            std::lock_guard lk(m_);
            lastClean_ = sum;
        }
        PostMessageW(hwnd_, WM_APP_TEMP_CLEAN_DONE, 0, 0);
    });
}

LRESULT TempFilesView::OnMessage(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_APP_TEMP_SCAN_DONE: {
            if (th_.joinable()) th_.join();
            for (auto& r : cats_) r.scanned = true;
            working_ = false;
            FillList();
            uint64_t files = 0;
            for (auto& r : cats_) files += r.res.files;
            uint64_t bytes = 0;
            for (auto& r : cats_) bytes += r.res.bytes;
            std::wstring found = Plural(files, L"eligible file", L"eligible files") + L" found.";
            SetWindowTextW(result_, (cleanedNote_.empty() ? found : cleanedNote_ + L"\r\n" + found).c_str());
            if (pendingConfirm_) {  // the list behind this box is already refreshed
                pendingConfirm_ = false;
                mw_->ShowMessage(L"Temporary files cleaned",
                                 confirmText_ + L"\n\nThe list has been refreshed: " + Plural(files, L"eligible file", L"eligible files") + L" (" +
                                     FormatBytes(bytes) + (files == 1 ? L") remains." : L") remain."));
            }
            return 0;
        }
        case WM_APP_TEMP_CLEAN_DONE: {
            if (th_.joinable()) th_.join();
            working_ = false;
            TempCleanResult r;
            {
                std::lock_guard lk(m_);
                r = lastClean_;
            }
            std::wstring t = L"Cleaned:\r\n" + FormatBytes(r.cleanedBytes) + L" (" + Plural(r.cleanedFiles, L"file", L"files") + L")\r\nSkipped:\r\n" +
                             FormatBytes(r.skippedBytes) + L" (" + Plural(r.skippedFiles, L"file", L"files");
            if (r.inUseFiles) t += L", " + FormatCount(r.inUseFiles) + L" currently in use";
            t += L")";
            if (r.rejected) t += L"\r\nOne folder failed the safety validation and was not touched.";
            cleanedNote_ = t;
            confirmText_ = L"Freed: " + FormatBytes(r.cleanedBytes) + L" (" + Plural(r.cleanedFiles, L"file", L"files") + L")\nSkipped: " +
                           FormatBytes(r.skippedBytes) + L" (" + Plural(r.skippedFiles, L"file", L"files") +
                           (r.inUseFiles ? L", " + FormatCount(r.inUseFiles) + L" currently in use" : L"") + L")";
            if (r.rejected) confirmText_ += L"\nOne folder failed the safety validation and was not touched.";
            pendingConfirm_ = true;
            mw_->SetStatus(L"Temporary files cleaned: " + FormatBytes(r.cleanedBytes));
            mw_->DataChanged();       // drive statistics
            StartScan(true);          // refresh the list so it shows what is left, not the old numbers
            return 0;
        }
        case WM_NOTIFY: {
            auto* nm = reinterpret_cast<NMHDR*>(l);
            if (nm->code == NM_CUSTOMDRAW && nm->hwndFrom == ListView_GetHeader(list_)) return HeaderCustomDraw(l);
            if (nm->code == NM_CUSTOMDRAW && nm->hwndFrom == list_) {
                auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(l);
                if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) return CDRF_NOTIFYPOSTPAINT;
                if (cd->nmcd.dwDrawStage == CDDS_ITEMPOSTPAINT) {
                    int i = static_cast<int>(cd->nmcd.dwItemSpec);
                    DrawListTickNative(list_, cd->nmcd.hdc, i, ListView_GetCheckState(list_, i) != 0);
                }
                return CDRF_DODEFAULT;
            }
            if (nm->hwndFrom == list_ && nm->code == LVN_ITEMCHANGED && !filling_) {
                auto* v = reinterpret_cast<NMLISTVIEW*>(l);
                if ((v->uChanged & LVIF_STATE) && ((v->uNewState ^ v->uOldState) & LVIS_STATEIMAGEMASK)) UpdateButtons();
            }
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(w)) {
                case ID_SCAN: StartScan(); break;
                case ID_CLEAN: StartClean(); break;
                case ID_AGE:
                    if (HIWORD(w) == CBN_SELCHANGE) {
                        for (auto& r : cats_) r.scanned = false;  // results no longer match the filter
                        FillList();
                        SetWindowTextW(result_, L"Filter changed. Scan again to see what can be cleaned.");
                    }
                    break;
            }
            return 0;
    }
    return View::OnMessage(m, w, l);
}

}  // namespace ss
