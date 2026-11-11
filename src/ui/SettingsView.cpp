#include "ui/SettingsView.h"

#include <commctrl.h>
#include <shobjidl.h>

#include <algorithm>

#include "app/History.h"
#include "filesystem/PathUtils.h"
#include "operations/ExplorerService.h"
#include "ui/MainWindow.h"
#include "ui/UiCommon.h"
#include "utils/Logger.h"

namespace ss {
namespace {
enum { ID_HIDDEN = 1, ID_SYSTEM, ID_R24, ID_R7, ID_R30, ID_ASK, ID_SECOND, ID_WORKERS, ID_LOGS, ID_X1, ID_X2, ID_SAFETY, ID_THEME, ID_IGNLIST, ID_ADD, ID_REMOVE, ID_HIST, ID_CLEARHIST, ID_REMEMBER, ID_AUTOUPD, ID_CLEARSCAN };
const unsigned kWorkers[] = {0, 1, 2, 4, 8};
}  // namespace

void SettingsView::OnCreate() {
    hdrTheme_ = Make(L"STATIC", L"Appearance", SS_LEFT, 0);
    cbTheme_ = Make(L"SSCombo", L"Theme", WS_TABSTOP, ID_THEME);
    for (const wchar_t* s : {L"Theme: Follow Windows", L"Theme: Light", L"Theme: Dark"})
        SendMessageW(cbTheme_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
    hdrScan_ = Make(L"STATIC", L"Scan", SS_LEFT, 0);
    skipReparse_ = Make(L"BUTTON", L"Skip reparse points - always on", WS_TABSTOP | BS_AUTOCHECKBOX, ID_X1);
    skipDenied_ = Make(L"BUTTON", L"Skip inaccessible folders - always on", WS_TABSTOP | BS_AUTOCHECKBOX, ID_X2);
    EnableWindow(skipReparse_, FALSE);
    EnableWindow(skipDenied_, FALSE);
    SendMessageW(skipReparse_, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(skipDenied_, BM_SETCHECK, BST_CHECKED, 0);
    showHidden_ = Make(L"BUTTON", L"Show hidden files", WS_TABSTOP | BS_AUTOCHECKBOX, ID_HIDDEN);

    hdrTemp_ = Make(L"STATIC", L"Temporary file cleanup - default age", SS_LEFT, 0);
    cbAge_ = Make(L"SSCombo", L"Temporary file age", WS_TABSTOP, ID_R24);
    for (const wchar_t* s : {L"Files older than 24 hours", L"Files older than 7 days", L"Files older than 30 days"})
        SendMessageW(cbAge_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));

    hdrDel_ = Make(L"STATIC", L"Deletion", SS_LEFT, 0);
    askRecycle_ = Make(L"BUTTON", L"Ask before moving to Recycle Bin", WS_TABSTOP | BS_AUTOCHECKBOX, ID_ASK);
    second_ = Make(L"BUTTON", L"Require second confirmation for permanent deletion", WS_TABSTOP | BS_AUTOCHECKBOX, ID_SECOND);

    hdrPerf_ = Make(L"STATIC", L"Performance", SS_LEFT, 0);
    lblWorkers_ = Make(L"STATIC", L"Scanner workers:", SS_LEFT | SS_CENTERIMAGE, 0);
    cbWorkers_ = Make(L"SSCombo", L"Scanner workers", WS_TABSTOP, ID_WORKERS);
    for (const wchar_t* s : {L"Auto", L"1", L"2", L"4", L"8"}) SendMessageW(cbWorkers_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));

    hdrMem_ = Make(L"STATIC", L"Scan memory and updates", SS_LEFT, 0);
    remember_ = Make(L"BUTTON", L"Remember the last scan (stores file and folder names on this PC)", WS_TABSTOP | BS_AUTOCHECKBOX, ID_REMEMBER);
    autoUpd_ = Make(L"BUTTON", L"Keep results up to date automatically", WS_TABSTOP | BS_AUTOCHECKBOX, ID_AUTOUPD);
    memInfo_ = Make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, 0);
    bClearScan_ = Make(L"BUTTON", L"Clear saved scan", WS_TABSTOP | BS_OWNERDRAW, ID_CLEARSCAN);
    hdrPriv_ = Make(L"STATIC", L"Privacy", SS_LEFT, 0);
    history_ = Make(L"BUTTON", L"Remember free space per day (no file names)", WS_TABSTOP | BS_AUTOCHECKBOX, ID_HIST);
    bClearHist_ = Make(L"BUTTON", L"Clear history", WS_TABSTOP | BS_OWNERDRAW, ID_CLEARHIST);
    hdrIgnore_ = Make(L"STATIC", L"Ignored folders and drives", SS_LEFT, 0);
    noteIgnore_ = Make(L"STATIC",
                       L"Scans skip these completely: nothing inside them is counted or shown. Changes apply to the results on screen "
                       L"at once; entries you remove come back after the next scan or Refresh.",
                       SS_LEFT | SS_NOPREFIX, 0);
    ignoreList_ = Make(L"LISTBOX", L"", WS_TABSTOP | WS_VSCROLL | WS_HSCROLL | WS_BORDER | LBS_NOTIFY | LBS_EXTENDEDSEL | LBS_NOINTEGRALHEIGHT, ID_IGNLIST);
    bAdd_ = Make(L"BUTTON", L"Add Folder or Drive...", WS_TABSTOP | BS_OWNERDRAW, ID_ADD);
    bRemove_ = Make(L"BUTTON", L"Remove Selected", WS_TABSTOP | BS_OWNERDRAW | WS_DISABLED, ID_REMOVE);

    safety_ = Make(L"STATIC", L"System path protection is always enabled and cannot be turned off.", SS_LEFT | SS_NOPREFIX, ID_SAFETY);
    bLogs_ = Make(L"BUTTON", L"Open Logs", WS_TABSTOP | BS_OWNERDRAW, ID_LOGS);
    EnableScroll();
    Load();
}

void SettingsView::Layout(int w, int h) {
    if (!BeginLayout(w, h)) return;
    int x = mw_->S(20), y = mw_->S(14) - scrollY_, rowH = mw_->S(26), cw = std::max(100, w - 2 * x);
    auto hdr = [&](HWND hh) {
        SendMessageW(hh, WM_SETFONT, reinterpret_cast<WPARAM>(mw_->boldFont()), TRUE);
        MoveWindow(hh, x, y, cw, rowH, TRUE);
        y += rowH + mw_->S(2);
    };
    auto row = [&](HWND hh) {
        MoveWindow(hh, x + mw_->S(12), y, cw - mw_->S(12), rowH, TRUE);
        y += rowH;
    };
    hdr(hdrTheme_);
    MoveWindow(cbTheme_, x + mw_->S(12), y, mw_->S(220), mw_->S(200), TRUE);
    y += rowH + mw_->S(14);
    hdr(hdrScan_);
    row(skipReparse_);
    row(skipDenied_);
    row(showHidden_);
    y += mw_->S(12);
    hdr(hdrTemp_);
    MoveWindow(cbAge_, x + mw_->S(12), y, mw_->S(260), mw_->S(200), TRUE);
    y += rowH + mw_->S(4);
    y += mw_->S(12);
    hdr(hdrDel_);
    row(askRecycle_);
    row(second_);
    y += mw_->S(12);
    hdr(hdrPerf_);
    MoveWindow(lblWorkers_, x + mw_->S(12), y, mw_->S(130), rowH, TRUE);
    MoveWindow(cbWorkers_, x + mw_->S(150), y, mw_->S(100), mw_->S(200), TRUE);
    y += rowH + mw_->S(12);
    hdr(hdrMem_);
    row(remember_);
    row(autoUpd_);
    row(memInfo_);
    MoveWindow(bClearScan_, x + mw_->S(12), y + mw_->S(4), mw_->S(160), mw_->S(32), TRUE);
    y += mw_->S(48);
    hdr(hdrPriv_);
    row(history_);
    MoveWindow(bClearHist_, x + mw_->S(12), y + mw_->S(4), mw_->S(160), mw_->S(32), TRUE);
    y += mw_->S(48);
    MoveWindow(safety_, x, y, cw, rowH, TRUE);
    y += rowH + mw_->S(10);
    MoveWindow(bLogs_, x, y, mw_->S(130), mw_->S(32), TRUE);

    // Right column (below the left one on narrow windows): the ignore list
    int rx = x, ry = y + mw_->S(48);
    if (w >= mw_->S(900)) {
        rx = mw_->S(470);
        ry = mw_->S(14) - scrollY_;
    }
    int rw = std::max(mw_->S(200), w - rx - x);
    SendMessageW(hdrIgnore_, WM_SETFONT, reinterpret_cast<WPARAM>(mw_->boldFont()), TRUE);
    MoveWindow(hdrIgnore_, rx, ry, rw, rowH, TRUE);
    MoveWindow(noteIgnore_, rx, ry + rowH + mw_->S(2), rw, mw_->S(68), TRUE);
    int ly = ry + rowH + mw_->S(76);
    MoveWindow(ignoreList_, rx, ly, rw, mw_->S(210), TRUE);
    MoveWindow(bAdd_, rx, ly + mw_->S(220), mw_->S(210), mw_->S(34), TRUE);
    MoveWindow(bRemove_, rx + mw_->S(220), ly + mw_->S(220), mw_->S(170), mw_->S(34), TRUE);

    EndLayout(std::max(y + mw_->S(32), ly + mw_->S(220) + mw_->S(34)));
}

void SettingsView::FillIgnoreList() {
    SendMessageW(ignoreList_, LB_RESETCONTENT, 0, 0);
    for (const auto& p : mw_->settings().ignored) SendMessageW(ignoreList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(p.c_str()));
    SendMessageW(ignoreList_, LB_SETHORIZONTALEXTENT, mw_->S(1200), 0);
    UpdateRemoveButton();
}

void SettingsView::UpdateRemoveButton() {
    EnableWindow(bRemove_, SendMessageW(ignoreList_, LB_GETSELCOUNT, 0, 0) > 0);
}

void SettingsView::AddIgnored() {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_ALLOWMULTISELECT | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"Choose folders or drives to ignore");
    bool changed = false;
    if (SUCCEEDED(dlg->Show(mw_->hwnd()))) {
        IShellItemArray* arr = nullptr;
        if (SUCCEEDED(dlg->GetResults(&arr))) {
            DWORD n = 0;
            arr->GetCount(&n);
            auto& list = mw_->settings().ignored;
            for (DWORD i = 0; i < n; ++i) {
                IShellItem* it = nullptr;
                if (FAILED(arr->GetItemAt(i, &it))) continue;
                PWSTR p = nullptr;
                if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                    std::wstring c = path::Canonicalize(p);
                    CoTaskMemFree(p);
                    bool dup = false;
                    for (const auto& e : list) dup = dup || path::EqualsNoCase(e, c);
                    if (!c.empty() && !dup) {
                        list.push_back(c);
                        changed = true;
                    }
                }
                it->Release();
            }
            arr->Release();
        }
    }
    dlg->Release();
    if (changed) {
        FillIgnoreList();
        mw_->IgnoreListChanged();
    }
}

void SettingsView::RemoveIgnored() {
    int cnt = static_cast<int>(SendMessageW(ignoreList_, LB_GETSELCOUNT, 0, 0));
    if (cnt <= 0) return;
    std::vector<int> idx(static_cast<size_t>(cnt));
    SendMessageW(ignoreList_, LB_GETSELITEMS, static_cast<WPARAM>(cnt), reinterpret_cast<LPARAM>(idx.data()));
    auto& list = mw_->settings().ignored;
    std::sort(idx.rbegin(), idx.rend());  // erase from the back so earlier indexes stay valid
    for (int i : idx)
        if (i >= 0 && i < static_cast<int>(list.size())) list.erase(list.begin() + i);
    FillIgnoreList();
    mw_->IgnoreListChanged();
}

void SettingsView::Load() {
    loading_ = true;
    const Settings& s = mw_->settings();
    SendMessageW(showHidden_, BM_SETCHECK, s.showHidden ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(askRecycle_, BM_SETCHECK, s.askBeforeRecycle ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(second_, BM_SETCHECK, s.requireSecondPermanentConfirm ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(cbAge_, CB_SETCURSEL, s.tempAgeHours == 168 ? 1 : s.tempAgeHours == 720 ? 2 : 0, 0);
    int sel = 0;
    for (int i = 0; i < 5; ++i)
        if (kWorkers[i] == s.workers) sel = i;
    SendMessageW(cbWorkers_, CB_SETCURSEL, static_cast<WPARAM>(sel), 0);
    SendMessageW(cbTheme_, CB_SETCURSEL, s.theme, 0);
    SendMessageW(history_, BM_SETCHECK, s.keepHistory ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(remember_, BM_SETCHECK, s.rememberScan ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(autoUpd_, BM_SETCHECK, s.autoUpdate ? BST_CHECKED : BST_UNCHECKED, 0);
    SetWindowTextW(memInfo_, mw_->SavedScanInfo().c_str());
    FillIgnoreList();
    loading_ = false;
}

void SettingsView::Store() {
    if (loading_) return;
    Settings& s = mw_->settings();
    s.showHidden = SendMessageW(showHidden_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    s.askBeforeRecycle = SendMessageW(askRecycle_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    s.requireSecondPermanentConfirm = SendMessageW(second_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    int ai = static_cast<int>(SendMessageW(cbAge_, CB_GETCURSEL, 0, 0));
    s.tempAgeHours = ai == 1 ? 168 : ai == 2 ? 720 : 24;
    int i = static_cast<int>(SendMessageW(cbWorkers_, CB_GETCURSEL, 0, 0));
    s.workers = kWorkers[(i < 0 || i > 4) ? 0 : i];
    s.keepHistory = SendMessageW(history_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (!s.keepHistory) History::Clear();  // switching it off also removes what was stored
    s.rememberScan = SendMessageW(remember_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    s.autoUpdate = SendMessageW(autoUpd_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    int t = static_cast<int>(SendMessageW(cbTheme_, CB_GETCURSEL, 0, 0));
    s.theme = (t < 0 || t > 2) ? 0 : static_cast<uint32_t>(t);
    mw_->SettingsChanged();
    SetWindowTextW(memInfo_, mw_->SavedScanInfo().c_str());
}

LRESULT SettingsView::OnMessage(UINT m, WPARAM w, LPARAM l) {
    if (m == WM_COMMAND) {
        int id = LOWORD(w);
        if (id == ID_LOGS) explorer::OpenFolder(Logger::Get().Dir());
        else if (id == ID_CLEARHIST) {
            History::Clear();
            mw_->DataChanged();
        } else if (id == ID_CLEARSCAN) {
            mw_->ClearSavedScan();
            SetWindowTextW(memInfo_, mw_->SavedScanInfo().c_str());
        } else if (id == ID_ADD) AddIgnored();
        else if (id == ID_REMOVE) RemoveIgnored();
        else if (id == ID_IGNLIST) UpdateRemoveButton();
        else if ((id == ID_WORKERS || id == ID_THEME || id == ID_R24) ? HIWORD(w) == CBN_SELCHANGE : HIWORD(w) == BN_CLICKED) Store();
        return 0;
    }
    return View::OnMessage(m, w, l);
}

}  // namespace ss
