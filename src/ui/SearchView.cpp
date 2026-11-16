#include "ui/SearchView.h"

#include <commctrl.h>
#include <shobjidl.h>
#include <uxtheme.h>

#include <algorithm>

#include "filesystem/PathUtils.h"
#include "operations/ExplorerService.h"
#include "ui/MainWindow.h"
#include "ui/UiCommon.h"
#include "utils/FormatUtils.h"

namespace ss {
namespace {

enum {
    ID_WHERE = 1, ID_KIND, ID_QUERY, ID_FILTERS, ID_SCOPELIST, ID_ADDSCOPE, ID_REMSCOPE, ID_TYPE, ID_INCLUDE, ID_EXCLUDE, ID_MIN, ID_MAX, ID_DATE,
    ID_CASE, ID_WORD, ID_REGEX, ID_SKIPLIST, ID_ADDSKIP, ID_REMSKIP, ID_LIST, ID_INFO, ID_EMPTY, ID_NOTE, ID_OPEN, ID_BASKET, ID_RECYCLE, ID_PERM
};
enum Col { C_NAME, C_SIZE, C_TYPE, C_LOC, C_MOD, C_COUNT };
constexpr UINT_PTR kTimer = 1;
constexpr uint64_t kDay = 864000000000ull;  // FILETIME ticks per day
struct DateOpt {
    const wchar_t* label;
    int days;
    bool newer;  // true: modified within the last `days`; false: older than `days`
};
constexpr DateOpt kDates[] = {{L"Any time", 0, true},           {L"Last 7 days", 7, true},          {L"Last 30 days", 30, true},
                              {L"Last year", 365, true},        {L"Older than 3 months", 91, false}, {L"Older than 6 months", 183, false},
                              {L"Older than 1 year", 365, false}, {L"Older than 2 years", 730, false}};
constexpr ULONGLONG kBudgetMs = 3000;       // a search that runs longer (a heavy regex) is stopped

uint64_t MbToBytes(const std::wstring& s, uint64_t none) {
    if (s.empty()) return none;
    return _wcstoui64(s.c_str(), nullptr, 10) * 1024ull * 1024ull;
}

std::wstring Trim(std::wstring s) {
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == L' ' || s[i] == L'\t')) ++i;
    return s.substr(i);
}

std::vector<std::wstring> PickFolders(HWND owner, const wchar_t* title) {
    std::vector<std::wstring> out;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return out;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_ALLOWMULTISELECT | FOS_PATHMUSTEXIST);
    dlg->SetTitle(title);
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItemArray* arr = nullptr;
        if (SUCCEEDED(dlg->GetResults(&arr))) {
            DWORD n = 0;
            arr->GetCount(&n);
            for (DWORD i = 0; i < n; ++i) {
                IShellItem* it = nullptr;
                if (FAILED(arr->GetItemAt(i, &it))) continue;
                PWSTR p = nullptr;
                if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                    std::wstring c = path::Canonicalize(p);
                    CoTaskMemFree(p);
                    if (!c.empty()) out.push_back(c);
                }
                it->Release();
            }
            arr->Release();
        }
    }
    dlg->Release();
    return out;
}

}  // namespace

std::wstring SearchView::Text(HWND h) const {
    wchar_t b[600];
    GetWindowTextW(h, b, 600);
    return Trim(b);
}

bool SearchView::Checked(HWND h) const { return SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED; }

void SearchView::OnCreate() {
    lblWhere_ = Make(L"STATIC", L"Search in:", SS_LEFT | SS_CENTERIMAGE, 0);
    cbWhere_ = Make(L"SSCombo", L"Search in", WS_TABSTOP, ID_WHERE);
    for (const wchar_t* s : {L"Whole system", L"Selected folders"}) SendMessageW(cbWhere_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
    lblKind_ = Make(L"STATIC", L"Show:", SS_LEFT | SS_CENTERIMAGE, 0);
    cbKind_ = Make(L"SSCombo", L"Show", WS_TABSTOP, ID_KIND);
    for (const wchar_t* s : {L"Files and folders", L"Files only", L"Folders only"}) SendMessageW(cbKind_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
    edQuery_ = Make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, ID_QUERY, WS_EX_CLIENTEDGE);
    SendMessageW(edQuery_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Search files and folders: words, \"exact phrase\", *.pdf, report?.docx"));
    bFilters_ = Make(L"BUTTON", L"Filters", WS_TABSTOP | BS_OWNERDRAW, ID_FILTERS);

    lblScope_ = Make(L"STATIC", L"Search only in:", SS_LEFT | SS_CENTERIMAGE, 0);
    lbScope_ = Make(L"LISTBOX", L"", WS_TABSTOP | WS_VSCROLL | WS_HSCROLL | WS_BORDER | LBS_NOTIFY | LBS_EXTENDEDSEL | LBS_NOINTEGRALHEIGHT, ID_SCOPELIST);
    bAddScope_ = Make(L"BUTTON", L"Add Folder...", WS_TABSTOP | BS_OWNERDRAW, ID_ADDSCOPE);
    bRemScope_ = Make(L"BUTTON", L"Remove Selected", WS_TABSTOP | BS_OWNERDRAW | WS_DISABLED, ID_REMSCOPE);

    lblType_ = Make(L"STATIC", L"Type:", SS_LEFT | SS_CENTERIMAGE, 0);
    cbType_ = Make(L"SSCombo", L"File type", WS_TABSTOP, ID_TYPE);
    SendMessageW(cbType_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"All types"));
    for (int i = 0; i < kCategoryCount; ++i) SendMessageW(cbType_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(CategoryName(static_cast<FileCategory>(i))));
    edInclude_ = Make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, ID_INCLUDE, WS_EX_CLIENTEDGE);
    SendMessageW(edInclude_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Only extensions: pdf, docx, *.log"));
    edExclude_ = Make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, ID_EXCLUDE, WS_EX_CLIENTEDGE);
    SendMessageW(edExclude_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Leave out: tmp, *.bak"));
    edMin_ = Make(L"EDIT", L"", WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, ID_MIN, WS_EX_CLIENTEDGE);
    SendMessageW(edMin_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Min size (MB)"));
    edMax_ = Make(L"EDIT", L"", WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, ID_MAX, WS_EX_CLIENTEDGE);
    SendMessageW(edMax_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Max size (MB)"));
    lblDate_ = Make(L"STATIC", L"Modified:", SS_LEFT | SS_CENTERIMAGE, 0);
    cbDate_ = Make(L"SSCombo", L"Modified", WS_TABSTOP, ID_DATE);
    for (const DateOpt& d : kDates) SendMessageW(cbDate_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(d.label));
    chkCase_ = Make(L"SSCheck", L"Match case", WS_TABSTOP, ID_CASE);
    chkWord_ = Make(L"SSCheck", L"Whole word", WS_TABSTOP, ID_WORD);
    chkRegex_ = Make(L"SSCheck", L"Regex", WS_TABSTOP, ID_REGEX);
    lblSkip_ = Make(L"STATIC", L"Skip folders:", SS_LEFT | SS_CENTERIMAGE, 0);
    lbSkip_ = Make(L"LISTBOX", L"", WS_TABSTOP | WS_VSCROLL | WS_HSCROLL | WS_BORDER | LBS_NOTIFY | LBS_EXTENDEDSEL | LBS_NOINTEGRALHEIGHT, ID_SKIPLIST);
    bAddSkip_ = Make(L"BUTTON", L"Add Folder...", WS_TABSTOP | BS_OWNERDRAW, ID_ADDSKIP);
    bRemSkip_ = Make(L"BUTTON", L"Remove Selected", WS_TABSTOP | BS_OWNERDRAW | WS_DISABLED, ID_REMSKIP);

    list_ = Make(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS, ID_LIST);
    SetWindowTheme(list_, L"Explorer", nullptr);
    ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP | LVS_EX_CHECKBOXES);
    ListView_SetCallbackMask(list_, LVIS_STATEIMAGEMASK);
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
    HWND hdr = ListView_GetHeader(list_);
    HDITEMW it{HDI_FORMAT};
    if (hdr && Header_GetItem(hdr, 0, &it)) {
        it.fmt |= HDF_SORTUP;
        Header_SetItem(hdr, 0, &it);
    }
    info_ = Make(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX, ID_INFO);
    empty_ = Make(L"STATIC", L"", SS_CENTER | SS_NOPREFIX, ID_EMPTY);
    bOpen_ = Make(L"BUTTON", L"Open File Location", WS_TABSTOP | BS_OWNERDRAW, ID_OPEN);
    bBasket_ = Make(L"BUTTON", L"Add to Basket", WS_TABSTOP | BS_OWNERDRAW, ID_BASKET);
    bRecycle_ = Make(L"BUTTON", L"Move to Recycle Bin", WS_TABSTOP | BS_OWNERDRAW, ID_RECYCLE);
    bPerm_ = Make(L"BUTTON", L"Delete Permanently", WS_TABSTOP | BS_OWNERDRAW, ID_PERM);
    note_ = Make(L"SSInfo",
                 L"Searches the current scan, not the disk, so results are instant but only as fresh as the scan. Words must all match. Use \"quotes\" for an "
                 L"exact phrase, * and ? as wildcards (*.pdf), and a backslash to match part of the path. Windows and installed programs (Program Files) are never "
                 L"listed, files marked as system files are skipped, and app data stays hidden unless you turn on Show app data in Settings.",
                 WS_TABSTOP, ID_NOTE);

    // restore the remembered choices
    const Settings& s = mw_->settings();
    SendMessageW(cbWhere_, CB_SETCURSEL, std::min<uint32_t>(s.searchWhere, 1), 0);
    SendMessageW(cbKind_, CB_SETCURSEL, std::min<uint32_t>(s.searchKind, 2), 0);
    SendMessageW(cbType_, CB_SETCURSEL, std::min<uint32_t>(s.searchType, kCategoryCount), 0);
    SendMessageW(cbDate_, CB_SETCURSEL, std::min<uint32_t>(s.searchDate, 7), 0);
    SetWindowTextW(edInclude_, s.searchInclude.c_str());
    SetWindowTextW(edExclude_, s.searchExclude.c_str());
    SendMessageW(chkCase_, BM_SETCHECK, (s.searchFlags & 2) ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(chkWord_, BM_SETCHECK, (s.searchFlags & 4) ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(chkRegex_, BM_SETCHECK, (s.searchFlags & 8) ? BST_CHECKED : BST_UNCHECKED, 0);
    advanced_ = (s.searchFlags & 1) != 0;
    scope_ = s.searchScope;
    skip_ = s.searchSkip;
    FillList(scope_, lbScope_);
    FillList(skip_, lbSkip_);
    ShowPanels();
    loading_ = false;
}

void SearchView::SaveChoices() {
    if (loading_) return;
    Settings& s = mw_->settings();
    s.searchWhere = static_cast<uint32_t>(std::max<LRESULT>(0, SendMessageW(cbWhere_, CB_GETCURSEL, 0, 0)));
    s.searchKind = static_cast<uint32_t>(std::max<LRESULT>(0, SendMessageW(cbKind_, CB_GETCURSEL, 0, 0)));
    s.searchType = static_cast<uint32_t>(std::max<LRESULT>(0, SendMessageW(cbType_, CB_GETCURSEL, 0, 0)));
    s.searchDate = static_cast<uint32_t>(std::max<LRESULT>(0, SendMessageW(cbDate_, CB_GETCURSEL, 0, 0)));
    s.searchInclude = Text(edInclude_);
    s.searchExclude = Text(edExclude_);
    s.searchFlags = (advanced_ ? 1u : 0u) | (Checked(chkCase_) ? 2u : 0u) | (Checked(chkWord_) ? 4u : 0u) | (Checked(chkRegex_) ? 8u : 0u);
    s.searchScope = scope_;
    s.searchSkip = skip_;
}

void SearchView::ShowPanels() {
    bool folders = SendMessageW(cbWhere_, CB_GETCURSEL, 0, 0) == 1;
    for (HWND h : {lblScope_, lbScope_, bAddScope_, bRemScope_}) ShowWindow(h, folders ? SW_SHOW : SW_HIDE);
    for (HWND h : {lblType_, cbType_, edInclude_, edExclude_, edMin_, edMax_, lblDate_, cbDate_, chkCase_, chkWord_, chkRegex_, lblSkip_, lbSkip_, bAddSkip_, bRemSkip_})
        ShowWindow(h, advanced_ ? SW_SHOW : SW_HIDE);
    SetWindowTextW(bFilters_, advanced_ ? L"Hide filters" : L"Filters");
    InvalidateRect(bFilters_, nullptr, TRUE);
}

void SearchView::Layout(int w, int h) {
    lastW_ = w;
    lastH_ = h;
    int m = mw_->S(12), gap = mw_->S(8), ch = mw_->S(34), bh = mw_->S(32);
    bool folders = SendMessageW(cbWhere_, CB_GETCURSEL, 0, 0) == 1;
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
        x += mw_->S(10);
    };
    // a labelled list of folders with Add / Remove buttons on its right
    auto folderRow = [&](HWND lbl, HWND lb, HWND add, HWND rem) {
        int lw = mw_->S(104), bw = mw_->S(150), lh = mw_->S(72);
        MoveWindow(lbl, m, y, lw - gap, ch, TRUE);
        MoveWindow(lb, m + lw, y, std::max(mw_->S(120), w - 2 * m - lw - bw - gap), lh, TRUE);
        MoveWindow(add, w - m - bw, y, bw, bh, TRUE);
        MoveWindow(rem, w - m - bw, y + bh + mw_->S(6), bw, bh, TRUE);
        y += lh + gap;
    };

    group({{lblWhere_, 80}, {cbWhere_, 190}});
    group({{lblKind_, 50}, {cbKind_, 170}});
    y += ch + gap;
    x = m;
    int fw = mw_->S(120);
    MoveWindow(edQuery_, m, y, std::max(mw_->S(150), w - 2 * m - fw - gap), ch, TRUE);
    MoveWindow(bFilters_, w - m - fw, y, fw, ch, TRUE);
    y += ch + gap;
    if (folders) folderRow(lblScope_, lbScope_, bAddScope_, bRemScope_);
    if (advanced_) {
        x = m;
        group({{lblType_, 48}, {cbType_, 150}});
        group({{edInclude_, 230}});
        group({{edExclude_, 200}});
        y += ch + gap;
        x = m;
        group({{edMin_, 120}, {edMax_, 120}});
        group({{lblDate_, 76}, {cbDate_, 190}});
        group({{chkCase_, 120}, {chkWord_, 120}, {chkRegex_, 90}});
        y += ch + gap;
        folderRow(lblSkip_, lbSkip_, bAddSkip_, bRemSkip_);
    }
    MoveWindow(info_, m, y, w - 2 * m, mw_->S(24), TRUE);
    int top = y + mw_->S(28);
    const FlowItem btns[] = {{bOpen_, mw_->S(170)}, {bBasket_, mw_->S(150)}, {bRecycle_, mw_->S(190)}, {bPerm_, mw_->S(180)}};
    int rows = FlowButtons(btns, 4, m, w, 0, bh, gap, false, false);
    int barH = barShown_ ? rows * (bh + gap) + gap : 0;
    int bottom = h - mw_->S(34) - barH;
    MoveWindow(list_, 0, top, w, std::max(0, bottom - top), TRUE);
    static const int kCols[C_COUNT] = {300, 90, 100, 300, 160};
    FitColumns(list_, kCols, C_COUNT, w);
    MoveWindow(note_, m, bottom + mw_->S(6), mw_->S(kInfoIconSize), mw_->S(kInfoIconSize), TRUE);
    FlowButtons(btns, 4, m, w, h - barH + gap, bh, gap, true, barShown_);
    int ew = std::min(w - 2 * m, mw_->S(560));
    MoveWindow(empty_, (w - ew) / 2, top + mw_->S(60), ew, mw_->S(60), TRUE);
}

void SearchView::FillList(std::vector<std::wstring>& list, HWND lb) {
    SendMessageW(lb, LB_RESETCONTENT, 0, 0);
    size_t longest = 0;
    for (const auto& p : list) {
        SendMessageW(lb, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(p.c_str()));
        longest = std::max(longest, p.size());
    }
    SendMessageW(lb, LB_SETHORIZONTALEXTENT, static_cast<WPARAM>(longest * mw_->S(9) + mw_->S(24)), 0);
    UpdateRemove();
}

void SearchView::UpdateRemove() {
    EnableWindow(bRemScope_, SendMessageW(lbScope_, LB_GETSELCOUNT, 0, 0) > 0);
    EnableWindow(bRemSkip_, SendMessageW(lbSkip_, LB_GETSELCOUNT, 0, 0) > 0);
}

void SearchView::AddFolders(std::vector<std::wstring>& list, HWND lb, const wchar_t* title) {
    for (auto& c : PickFolders(mw_->hwnd(), title)) {
        bool dup = false;
        for (const auto& e : list) dup = dup || path::EqualsNoCase(e, c);
        if (!dup) list.push_back(c);
    }
    FillList(list, lb);
}

void SearchView::RemoveSelected(std::vector<std::wstring>& list, HWND lb) {
    int cnt = static_cast<int>(SendMessageW(lb, LB_GETSELCOUNT, 0, 0));
    if (cnt <= 0) return;
    std::vector<int> idx(static_cast<size_t>(cnt));
    SendMessageW(lb, LB_GETSELITEMS, static_cast<WPARAM>(cnt), reinterpret_cast<LPARAM>(idx.data()));
    std::sort(idx.rbegin(), idx.rend());
    for (int i : idx)
        if (i >= 0 && i < static_cast<int>(list.size())) list.erase(list.begin() + i);
    FillList(list, lb);
}

void SearchView::SetQuery(const std::wstring& q) {
    loading_ = true;
    SetWindowTextW(edQuery_, q.c_str());
    loading_ = false;
    Run();
    SetFocus(edQuery_);
}

void SearchView::OnShow() {
    if (dirty_ || rows_.empty()) Run();
}

void SearchView::Reload() {
    dirty_ = true;
    if (!IsWindowVisible(hwnd_)) return;
    auto keep = std::move(checked_);  // ticks survive a refresh; they are only compared by address, never used
    Run();
    for (ScanNode* n : rows_)
        if (keep.count(n)) checked_.insert(n);
    InvalidateRect(list_, nullptr, FALSE);
    UpdateButtons();
}

void SearchView::DropReferences() {
    index_.Clear();
    rows_.clear();
    checked_.clear();
    dirty_ = true;
    ListView_SetItemCountEx(list_, 0, 0);
}

void SearchView::Run() {
    KillTimer(hwnd_, kTimer);
    rows_.clear();
    checked_.clear();
    ScanResult* sc = mw_->scan();
    std::wstring info, emptyMsg;
    uint64_t fileBytes = 0;
    if (!sc) {
        emptyMsg = mw_->EmptyStateText();
    } else {
        if (dirty_ || index_.Empty()) {
            index_.Build(sc->root.get());
            dirty_ = false;
        }
        const Settings& st = mw_->settings();
        SearchSpec sp;
        sp.text = Text(edQuery_);
        sp.matchCase = Checked(chkCase_);
        sp.wholeWord = Checked(chkWord_);
        sp.regex = Checked(chkRegex_);
        sp.kind = static_cast<SearchKind>(std::clamp<LRESULT>(SendMessageW(cbKind_, CB_GETCURSEL, 0, 0), 0, 2));
        int ts = static_cast<int>(SendMessageW(cbType_, CB_GETCURSEL, 0, 0));
        sp.category = ts > 0 ? ts - 1 : -1;
        sp.include = Text(edInclude_);
        sp.exclude = Text(edExclude_);
        sp.minSize = MbToBytes(Text(edMin_), 0);
        sp.maxSize = MbToBytes(Text(edMax_), ~0ull);
        int ds = static_cast<int>(SendMessageW(cbDate_, CB_GETCURSEL, 0, 0));
        if (ds > 0) {
            FILETIME now;
            GetSystemTimeAsFileTime(&now);
            uint64_t t = (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
            uint64_t cut = t - static_cast<uint64_t>(kDates[ds].days) * kDay;
            if (kDates[ds].newer) sp.minModified = cut;  // within the last N days
            else sp.maxModified = cut;                   // older than N days
        }
        sp.showHidden = st.showHidden;
        bool showApp = st.showAppData;
        sp.hideDir = [this, showApp](const ScanNode& c) {
            if (!showApp && IsAppDataRoot(&c)) return true;
            if (c.scanRoot || (c.parent && c.parent->scanRoot)) {  // Windows / Program Files: only the top level needs asking
                std::wstring p = BuildPath(&c);
                return !path::IsDriveRoot(p) && mw_->protection().Classify(p).level >= ProtectionLevel::NoDelete;
            }
            return false;
        };
        ULONGLONG t0 = GetTickCount64();
        sp.cancel = [t0] { return GetTickCount64() - t0 > kBudgetMs; };

        int missing = 0;
        bool folders = SendMessageW(cbWhere_, CB_GETCURSEL, 0, 0) == 1;
        if (folders)
            for (const auto& p : scope_) {
                if (const ScanNode* n = FindNodeByPath(sc->root.get(), p)) sp.scopes.push_back(n);
                else ++missing;
            }
        for (const auto& p : skip_)
            if (const ScanNode* n = FindNodeByPath(sc->root.get(), p)) sp.excluded.push_back(n);

        bool criteria = !sp.text.empty() || sp.category >= 0 || !sp.include.empty() || sp.minSize > 0 || sp.maxSize != ~0ull || ds > 0;
        if (folders && scope_.empty()) emptyMsg = L"Add one or more folders above to search in.";
        else if (folders && sp.scopes.empty()) emptyMsg = L"The chosen folders are not part of the current scan. Scan them first (Dashboard, Scan Folder).";
        else if (!criteria) emptyMsg = L"Type something to search for, or open Filters to search by type, size or date.";
        else {
            SearchResult r = index_.Run(sp);
            if (!r.error.empty()) emptyMsg = r.error;
            else {
                rows_ = std::move(r.hits);
                size_t files = 0;
                for (ScanNode* n : rows_)
                    if (n->type == NodeType::File) {
                        ++files;
                        fileBytes += n->size;
                    }
                info = Plural(rows_.size(), L"result", L"results") + L" (" + Plural(files, L"file", L"files") + L", " + FormatBytes(fileBytes) + L")";
                if (r.truncated) info += L"   -   showing the first 50,000, refine the search to narrow it";
                if (r.canceled) info += L"   -   stopped after 3 seconds, refine the search";
                if (missing) info += L"   -   " + Plural(static_cast<size_t>(missing), L"chosen folder is", L"chosen folders are") + L" not in the scan";
                if (rows_.empty()) emptyMsg = L"Nothing matched. Check the filters, or try fewer words.";
            }
        }
    }
    Sort();
    ListView_SetItemCountEx(list_, static_cast<int>(rows_.size()), 0);
    InvalidateRect(list_, nullptr, FALSE);
    bool none = rows_.empty();
    ShowWindow(list_, none ? SW_HIDE : SW_SHOW);
    ShowWindow(empty_, none ? SW_SHOW : SW_HIDE);
    SetWindowTextW(empty_, emptyMsg.c_str());
    SetWindowTextW(info_, info.c_str());
    UpdateButtons();
}

void SearchView::Sort() {
    if (sortCol_ == C_LOC || sortCol_ == C_TYPE) {  // build each key once; building it per comparison froze the UI on big lists
        std::vector<std::pair<std::wstring, ScanNode*>> keyed;
        keyed.reserve(rows_.size());
        for (ScanNode* n : rows_) keyed.emplace_back(CellText(n, sortCol_), n);
        std::sort(keyed.begin(), keyed.end(), [&](const auto& a, const auto& b) {
            int c = CompareStringOrdinal(a.first.c_str(), -1, b.first.c_str(), -1, TRUE) - CSTR_EQUAL;
            return sortAsc_ ? c < 0 : c > 0;
        });
        for (size_t i = 0; i < keyed.size(); ++i) rows_[i] = keyed[i].second;
        return;
    }
    std::sort(rows_.begin(), rows_.end(), [&](ScanNode* a, ScanNode* b) {
        int c = 0;
        switch (sortCol_) {
            case C_NAME: c = CompareStringOrdinal(a->name.c_str(), -1, b->name.c_str(), -1, TRUE) - CSTR_EQUAL; break;
            case C_MOD: c = a->modified < b->modified ? -1 : a->modified > b->modified ? 1 : 0; break;
            default: c = a->size < b->size ? -1 : a->size > b->size ? 1 : 0;
        }
        return sortAsc_ ? c < 0 : c > 0;
    });
}

std::wstring SearchView::CellText(ScanNode* n, int col) const {
    switch (col) {
        case C_NAME: return n->name;
        case C_SIZE: return FormatBytes(n->size);
        case C_TYPE: return n->type == NodeType::Directory ? L"Folder" : CategoryName(Categorize(n->name));
        case C_LOC: return path::Parent(BuildPath(n));
        case C_MOD: return FormatFileTime(n->modified);
    }
    return L"";
}

std::vector<ScanNode*> SearchView::Selected() const {
    std::vector<ScanNode*> out;
    if (!checked_.empty())
        for (ScanNode* n : rows_)
            if (checked_.count(n) && !n->removed) out.push_back(n);
    if (!out.empty()) return out;
    for (int i = ListView_GetNextItem(list_, -1, LVNI_SELECTED); i >= 0; i = ListView_GetNextItem(list_, i, LVNI_SELECTED))
        if (i < static_cast<int>(rows_.size())) out.push_back(rows_[static_cast<size_t>(i)]);
    return out;
}

void SearchView::UpdateButtons() {
    auto sel = Selected();
    bool ok = !sel.empty() && !mw_->busy();
    if (ok && sel.size() <= 300)
        for (auto* n : sel)
            if (mw_->protection().Classify(BuildPath(n)).level >= ProtectionLevel::NoDelete) ok = false;
    if (barShown_ != !sel.empty()) {
        barShown_ = !sel.empty();
        Layout(lastW_, lastH_);
    }
    EnableWindow(bOpen_, sel.size() == 1);
    EnableWindow(bRecycle_, ok);
    EnableWindow(bPerm_, ok);
    EnableWindow(bBasket_, ok);
}

void SearchView::Act(int id) {
    auto sel = Selected();
    if (sel.empty()) return;
    if (id == ID_BASKET) mw_->AddToBasket(sel);
    else if (id == ID_OPEN) explorer::Reveal(BuildPath(sel[0]), false);
    else if (id == ID_RECYCLE) mw_->DeleteItems(sel, DeleteMode::Recycle);
    else if (id == ID_PERM) mw_->DeleteItems(sel, DeleteMode::Permanent);
}

LRESULT SearchView::OnMessage(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_TIMER:
            if (w == kTimer) {
                Run();
                return 0;
            }
            break;
        case WM_NOTIFY: {
            auto* nm = reinterpret_cast<NMHDR*>(l);
            if (nm->code == NM_CUSTOMDRAW && nm->hwndFrom == ListView_GetHeader(list_)) return HeaderCustomDraw(l);
            if (nm->hwndFrom != list_) break;
            switch (nm->code) {
                case LVN_GETDISPINFOW: {
                    auto* d = reinterpret_cast<NMLVDISPINFOW*>(l);
                    if (d->item.iItem >= 0 && d->item.iItem < static_cast<int>(rows_.size())) {
                        if (d->item.mask & LVIF_TEXT)
                            wcsncpy_s(d->item.pszText, static_cast<size_t>(d->item.cchTextMax), CellText(rows_[static_cast<size_t>(d->item.iItem)], d->item.iSubItem).c_str(),
                                      _TRUNCATE);
                        if ((d->item.mask & LVIF_STATE) && (d->item.stateMask & LVIS_STATEIMAGEMASK)) d->item.state &= ~static_cast<UINT>(LVIS_STATEIMAGEMASK);
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
                    HWND hdr = ListView_GetHeader(list_);
                    for (int i = 0; i < C_COUNT; ++i) {
                        HDITEMW it{HDI_FORMAT};
                        if (!Header_GetItem(hdr, i, &it)) continue;
                        it.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
                        if (i == sortCol_) it.fmt |= sortAsc_ ? HDF_SORTUP : HDF_SORTDOWN;
                        Header_SetItem(hdr, i, &it);
                    }
                    Sort();
                    ListView_RedrawItems(list_, 0, std::max(0, static_cast<int>(rows_.size()) - 1));
                    InvalidateRect(list_, nullptr, FALSE);
                    break;
                }
                case NM_CLICK: {
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
                    if (k->wVKey == VK_DELETE && IsWindowEnabled(bRecycle_)) Act(GetKeyState(VK_SHIFT) < 0 ? ID_PERM : ID_RECYCLE);
                    else if (k->wVKey == VK_RETURN) Act(ID_OPEN);
                    else if (k->wVKey == VK_SPACE) {
                        for (int i = ListView_GetNextItem(list_, -1, LVNI_SELECTED); i >= 0; i = ListView_GetNextItem(list_, i, LVNI_SELECTED))
                            if (i < static_cast<int>(rows_.size()) && !checked_.erase(rows_[static_cast<size_t>(i)])) checked_.insert(rows_[static_cast<size_t>(i)]);
                        InvalidateRect(list_, nullptr, FALSE);
                        UpdateButtons();
                    } else if ((k->wVKey == 'A' && GetKeyState(VK_CONTROL) < 0) || k->wVKey == VK_ESCAPE) {
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
            auto rerun = [&] {
                SaveChoices();
                SetTimer(hwnd_, kTimer, 200, nullptr);  // wait for a pause in typing
            };
            if (id == ID_SCOPELIST || id == ID_SKIPLIST) {  // LBN_SELCHANGE and CBN_SELCHANGE are both 1: tell them apart by control
                UpdateRemove();
            } else if (code == EN_CHANGE && (id == ID_QUERY || id == ID_INCLUDE || id == ID_EXCLUDE || id == ID_MIN || id == ID_MAX)) {
                if (!loading_) rerun();
            } else if (code == CBN_SELCHANGE) {
                if (id == ID_WHERE) {
                    ShowPanels();
                    Layout(lastW_, lastH_);
                }
                rerun();
            } else if (code == BN_CLICKED) {
                switch (id) {
                    case ID_FILTERS:
                        advanced_ = !advanced_;
                        ShowPanels();
                        Layout(lastW_, lastH_);
                        SaveChoices();
                        break;
                    case ID_CASE: case ID_WORD: case ID_REGEX: rerun(); break;
                    case ID_ADDSCOPE: AddFolders(scope_, lbScope_, L"Choose folders to search in"); rerun(); break;
                    case ID_REMSCOPE: RemoveSelected(scope_, lbScope_); rerun(); break;
                    case ID_ADDSKIP: AddFolders(skip_, lbSkip_, L"Choose folders to skip"); rerun(); break;
                    case ID_REMSKIP: RemoveSelected(skip_, lbSkip_); rerun(); break;
                    default: Act(id);
                }
            }
            return 0;
        }
    }
    return View::OnMessage(m, w, l);
}

}  // namespace ss
