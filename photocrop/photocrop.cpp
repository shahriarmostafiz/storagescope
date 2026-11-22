// PhotoCrop: cut the applicant photo out of scanned forms. Window code only; the logic lives in core.h.
// Flow: auto-select all -> review the images that need a check -> Done & Export.
#include "core.h"
#include <shlobj.h>
#include <commctrl.h>
#include <uxtheme.h>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ---------------------------------------------------------------- UI
enum { ID_SRC = 1, ID_DST, ID_DETECT, ID_AUTOCROP, ID_APPLY, ID_PREV, ID_NEXT, ID_CONFIRM, ID_NEXTCHK, ID_SAVEONE, ID_SAVEALL, ID_LIST, ID_CANCEL, ID_FALL, ID_FDONE, ID_FREVIEW, ID_FMISSING, ID_SHAPE };  // ID_FALL..ID_FMISSING: the filter tabs above the list
enum { M_NONE, M_NEW, M_MOVE, M_SIZE, M_CORNER };  // selection drag modes (M_CORNER: one corner of a free shape; g_mask holds which)
enum { WM_JOBDONE = WM_APP + 1 };

HWND g_wnd;
HFONT g_font, g_fontB, g_fontBig;
int g_dpi = 96;
int S(int v) { return MulDiv(v, g_dpi, 96); }
int g_hdrExtra = 0;               // extra header height while the Save group wraps to a second row
int HDR() { return S(160) + g_hdrExtra; }
int PANEL() { return S(260); }
int LISTW() { return S(300); }
int LEGEND() { return S(66); }      // filter tabs + legend above the list
int STATUSH() { return S(34); }
int g_capX[3], g_capY[3];          // position of the three group captions (step badges)
fs::path g_src, g_dst;
std::vector<Item> g_items;
int g_idx = -1;
std::unique_ptr<Bitmap> g_img;   // current image, fully in memory (the file is not kept open)
std::unique_ptr<Bitmap> g_disp;  // g_img scaled once to the size it is shown at; redrawing it is ~20x faster
int g_dispW = 0, g_dispH = 0;
std::wstring g_note;
Sel g_sel;              // box shown for the current image
bool g_unconf = false;  // shown box is only a starting guess
Sel g_last;             // last box the user confirmed; offered on images that have none
int g_mode = M_NONE, g_mask = 0;
bool g_changed = false;
double g_ax, g_ay;
Sel g_orig;
bool g_busy = false;
int g_kind = 0;  // 1 detect, 2 save
Job g_job;
std::thread g_ctrl;

HWND Ctl(int id) { return GetDlgItem(g_wnd, id); }
void SaveState();

bool PickFolder(HWND h, fs::path& out, const wchar_t* title) {
    BROWSEINFOW bi{};
    bi.hwndOwner = h;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_NEWDIALOGSTYLE | BIF_RETURNONLYFSDIRS;
    PIDLIST_ABSOLUTE p = SHBrowseForFolderW(&bi);
    if (!p) return false;
    wchar_t b[MAX_PATH];
    bool ok = SHGetPathFromIDListW(p, b);
    CoTaskMemFree(p);
    if (ok) out = b;
    return ok;
}

// pending = confirmed/auto boxes not exported yet or changed since the last export; guessPending = unconfirmed guesses not exported;
// done / review / missing = how many images each filter tab shows
struct Counts { int n = 0, box = 0, check = 0, saved = 0, pending = 0, guessPending = 0, done = 0, review = 0, missing = 0; };
Counts Count() {
    Counts c;
    c.n = (int)g_items.size();
    for (auto& i : g_items) {
        c.box += i.sel.has; c.check += NeedsCheck(i); c.saved += i.saved;
        c.pending += i.sel.has && !i.saved && (i.src == S_AUTO || i.src == S_USER);
        c.guessPending += i.sel.has && !i.saved && i.src == S_GUESS;
        FileStatus s = StatusOf(i);
        c.done += s == ST_DONE; c.review += s == ST_REVIEW; c.missing += s == ST_MISSING;
    }
    return c;
}

// ---- the file list: the filter tab decides which images are shown, rows can be multi-selected
int g_filter = 0;           // 0 All, 1 Done, 2 Needs a better crop, 3 Not completed
std::vector<int> g_view;    // image index of every list row
std::vector<int> g_rowOf;   // list row of every image, -1 while the filter hides it

void RebuildView() {
    g_view.clear();
    g_rowOf.assign(g_items.size(), -1);
    for (size_t i = 0; i < g_items.size(); i++)
        if (InFilter(g_items[i], g_filter)) { g_rowOf[i] = (int)g_view.size(); g_view.push_back((int)i); }
}

int RowOf(int item) { return item >= 0 && item < (int)g_rowOf.size() ? g_rowOf[item] : -1; }

// Select exactly this image in the list (this also drops a multi-selection).
void SelectRow(int item) {
    HWND lb = Ctl(ID_LIST);
    SendMessageW(lb, LB_SETSEL, FALSE, -1);
    int row = RowOf(item);
    if (row >= 0) { SendMessageW(lb, LB_SETSEL, TRUE, row); SendMessageW(lb, LB_SETCARETINDEX, row, FALSE); }
}

// The images the operator picked: all selected rows, or just the current image when nothing is selected.
std::vector<int> SelectedItems() {
    std::vector<int> r;
    HWND lb = Ctl(ID_LIST);
    int n = (int)SendMessageW(lb, LB_GETSELCOUNT, 0, 0);
    if (n > 0) {
        std::vector<int> rows(n);
        SendMessageW(lb, LB_GETSELITEMS, n, (LPARAM)rows.data());
        for (int row : rows) if (row >= 0 && row < (int)g_view.size()) r.push_back(g_view[row]);
    } else if (g_idx >= 0) r.push_back(g_idx);
    return r;
}

// The list holds no text (LBS_NODATA): it only knows how many rows there are, and DrawListItem paints each
// row from g_items. Call this after anything that can change which images the filter shows.
void FillList() {
    HWND lb = Ctl(ID_LIST);
    RebuildView();
    SendMessageW(lb, WM_SETREDRAW, FALSE, 0);
    int top = (int)SendMessageW(lb, LB_GETTOPINDEX, 0, 0);
    SendMessageW(lb, LB_SETCOUNT, g_view.size(), 0);
    SendMessageW(lb, LB_SETTOPINDEX, top, 0);
    SelectRow(g_idx);
    SendMessageW(lb, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lb, nullptr, TRUE);
}

void UpdateListItem(int i) {  // one image changed: repaint its row, or rebuild when the filter may now hide it
    if (g_filter != 0) { FillList(); return; }
    RECT r;
    if (RowOf(i) >= 0 && SendMessageW(Ctl(ID_LIST), LB_GETITEMRECT, RowOf(i), (LPARAM)&r) != LB_ERR) InvalidateRect(Ctl(ID_LIST), &r, FALSE);
}

void SetText(int id, const std::wstring& t) {  // only when it changed, so buttons do not flicker
    wchar_t cur[128];
    GetWindowTextW(Ctl(id), cur, 128);
    if (t != cur) SetWindowTextW(Ctl(id), t.c_str());
}

void UpdateUi() {
    Counts c = Count();
    bool has = c.n > 0, b = g_busy;
    int row = RowOf(g_idx), picked = (int)SendMessageW(Ctl(ID_LIST), LB_GETSELCOUNT, 0, 0);
    auto en = [&](int id, bool v) { EnableWindow(Ctl(id), v && !b); };
    en(ID_SRC, true); en(ID_DST, true);
    en(ID_PREV, row > 0 || (row < 0 && !g_view.empty())); en(ID_NEXT, row >= 0 ? row + 1 < (int)g_view.size() : !g_view.empty());
    en(ID_DETECT, has);
    en(ID_AUTOCROP, has);
    en(ID_APPLY, has && g_sel.has);
    en(ID_CONFIRM, g_img && g_sel.has);
    en(ID_NEXTCHK, c.check > 0);
    en(ID_SHAPE, g_img && g_sel.has);
    en(ID_SAVEONE, picked >= 2 || (g_img && g_sel.has));
    en(ID_SAVEALL, c.box > 0);
    SetText(ID_APPLY, picked >= 2 ? L"Copy box to " + std::to_wstring(picked) + L" selected" : L"Copy box to unchecked");
    SetText(ID_SAVEONE, picked >= 2 ? L"Update " + std::to_wstring(picked) + L" selected" : L"Update this one");
    SetText(ID_FALL, L"All " + std::to_wstring(c.n));
    SetText(ID_FDONE, L"✓ " + std::to_wstring(c.done));
    SetText(ID_FREVIEW, L"▲ " + std::to_wstring(c.review));
    SetText(ID_FMISSING, L"✕ " + std::to_wstring(c.missing));
    for (int id = ID_FALL; id <= ID_FMISSING; id++) en(id, has);
    EnableWindow(Ctl(ID_LIST), !b);
    ShowWindow(Ctl(ID_CANCEL), b ? SW_SHOW : SW_HIDE);
    InvalidateRect(g_wnd, nullptr, FALSE);
}

// syncList = false when the change came from a click in the list itself (so a multi-selection stays)
void LoadCurrent(bool syncList = true) {
    g_img.reset();
    g_disp.reset();
    g_changed = false;
    if (g_idx >= 0 && g_idx < (int)g_items.size()) {
        {   // load, copy into memory and close the file, so the scan can still be moved or deleted in Explorer
            Bitmap fromFile(g_items[g_idx].path.c_str());
            if (fromFile.GetLastStatus() == Ok) {
                int W = fromFile.GetWidth(), H = fromFile.GetHeight();
                auto copy = std::make_unique<Bitmap>(W, H, PixelFormat32bppPARGB);  // drawn, not cloned: a clone can keep the file open
                Graphics gc(copy.get());
                gc.SetCompositingMode(CompositingModeSourceCopy);
                gc.SetInterpolationMode(InterpolationModeNearestNeighbor);
                gc.DrawImage(&fromFile, Rect(0, 0, W, H), 0, 0, W, H, UnitPixel);
                if (gc.GetLastStatus() == Ok) g_img = std::move(copy);
            }
        }
        const Item& it = g_items[g_idx];
        if (it.sel.has) { g_sel = it.sel; g_unconf = it.src == S_GUESS; }
        else { g_sel = g_last; g_unconf = true; }  // offer the last confirmed box as a starting point
        if (syncList) SelectRow(g_idx);
    } else g_sel = Sel();
    g_note.clear();
    UpdateUi();
}

void Go(int i) {
    if (i < 0 || i >= (int)g_items.size()) return;
    if (g_rowOf.size() != g_items.size()) RebuildView();
    g_idx = i;
    if (RowOf(i) < 0 && g_filter != 0) { g_filter = 0; FillList(); }  // the filter hides this image: switch to All so it is shown
    LoadCurrent();
}

// Previous / next image in the order the list shows (so a filter tab steps only through its own images).
void StepView(int dir) {
    if (g_view.empty()) return;
    int row = RowOf(g_idx), nr = row < 0 ? (dir > 0 ? 0 : (int)g_view.size() - 1) : row + dir;
    if (nr >= 0 && nr < (int)g_view.size()) Go(g_view[nr]);
}

// The next image after `from` (in list order, wrapping round) that still needs a check; -1 when none.
int FindCheck(int from) {
    int n = (int)g_view.size();
    if (!n) return -1;
    int start = RowOf(from);  // -1: start at the top
    for (int k = 1; k <= n; k++) {
        int row = ((start + k) % n + n) % n;
        if (NeedsCheck(g_items[g_view[row]])) return g_view[row];
    }
    return -1;
}

void Commit() {  // the current box becomes this image's confirmed box
    if (g_idx < 0 || !g_sel.has) return;
    Item& it = g_items[g_idx];
    it.sel = g_sel; it.src = S_USER; it.saved = false; it.failed = false;
    g_last = g_sel; g_unconf = false;
    UpdateListItem(g_idx);
    SaveState();
}

// "Free shape": switch the box between a plain rectangle and four free corners (like a document scanner),
// so a photo that sits crooked on the scan is straightened when it is exported.
void ToggleShape() {
    if (!g_img || !g_sel.has) return;
    if (g_sel.quad) ToRect(g_sel); else ToQuad(g_sel);
    Commit();
    g_note = g_sel.quad ? L"Free shape on: drag each corner onto a corner of the photo. The preview shows it straightened." : L"Back to a plain rectangle (the tilt is dropped).";
    UpdateUi();
}

void ConfirmNext() {
    if (!g_sel.has) { g_note = L"Draw a box around the photo first."; UpdateUi(); return; }
    int n = FindCheck(g_idx);  // look before committing: afterwards this image may leave the filtered list
    if (n == g_idx) n = -1;
    Commit();
    if (n >= 0) Go(n);
    else if (g_idx + 1 < (int)g_items.size()) { Go(g_idx + 1); g_note = L"Every image has a box now. Click Done & Export when you are happy."; UpdateUi(); }
    else { g_note = L"Every image has a box now. Click Done & Export."; UpdateUi(); }
}

// Everything the app remembers lives in %APPDATA%\PhotoCrop as UTF-8 text, so any folder name works.
fs::path DataDir() {
    wchar_t b[MAX_PATH];
    GetEnvironmentVariableW(L"APPDATA", b, MAX_PATH);
    fs::path dir = fs::path(b) / L"PhotoCrop";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

void SaveSettings() {
    WriteTextAtomic(DataDir() / L"settings.txt", L"src=" + g_src.wstring() + wchar_t(10) + L"dst=" + g_dst.wstring() + wchar_t(10));
}

// key is "src" or "dst". Falls back to the PhotoCrop.ini of earlier versions.
std::wstring ReadSetting(const wchar_t* key) {
    std::wstring text = ReadText(DataDir() / L"settings.txt"), want = std::wstring(key) + L"=";
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find(wchar_t(10), pos);
        std::wstring line = text.substr(pos, e == std::wstring::npos ? std::wstring::npos : e - pos);
        pos = e == std::wstring::npos ? text.size() : e + 1;
        if (line.rfind(want, 0) == 0) return line.substr(want.size());
    }
    wchar_t v[2048];
    GetPrivateProfileStringW(L"PhotoCrop", key, L"", v, 2048, (DataDir().parent_path() / L"PhotoCrop.ini").c_str());
    return v;
}

// Remembered boxes live in state_<hash of the source folder>.txt. The hash is FNV-1a, so it never changes
// with the compiler (std::hash may). legacy = the name earlier versions used, read once to keep old work.
fs::path StatePath(bool legacy = false) {
    std::wstring key = Lower(g_src.wstring());
    unsigned long long h = 1469598103934665603ULL;
    if (legacy) h = (unsigned long long)std::hash<std::wstring>{}(key);
    else for (wchar_t c : key) { h ^= (unsigned long long)c; h *= 1099511628211ULL; }
    wchar_t name[64];
    swprintf_s(name, L"state_%016llx.txt", h);
    return DataDir() / name;
}

void SaveState() {
    if (g_src.empty() || g_items.empty()) return;
    WriteTextAtomic(StatePath(), SerializeState(g_src, g_dst, g_items));
}

// Returns how many images got their box back.
int LoadState() {
    std::wstring text = ReadText(StatePath());
    if (text.empty()) text = ReadText(StatePath(true));
    if (text.empty()) return 0;
    std::error_code ec;
    return ParseState(text, g_dst, g_items, [&](const fs::path& name) { return fs::exists(g_dst / name, ec); });
}

void SetSource(const fs::path& p) {
    g_src = p;
    g_items.clear();
    std::error_code ec;
    for (auto& e : fs::directory_iterator(p, ec))
        if (e.is_regular_file() && IsImage(e.path())) { Item it; it.path = e.path(); g_items.push_back(it); }
    std::sort(g_items.begin(), g_items.end(), [](const Item& a, const Item& b) { return a.path < b.path; });
    g_idx = g_items.empty() ? -1 : 0;
    g_last = Sel();
    int restored = LoadState();
    for (auto& i : g_items) if (i.src == S_USER) g_last = i.sel;  // offer your latest box again
    g_filter = 0;
    RebuildView();
    int first = FindCheck(-1);
    if (restored && first >= 0) g_idx = first;  // carry on where the review stopped
    FillList();
    LoadCurrent();
    if (restored) { g_note = L"Restored " + std::to_wstring(restored) + L" boxes from your last session."; UpdateUi(); }
    if (g_items.empty()) { g_note = L"No images (jpg, png, bmp, tif, gif) found in that folder."; UpdateUi(); }
    SaveSettings();
}

// ---- paint
RECT CanvasRect() {
    RECT c; GetClientRect(g_wnd, &c);
    return {LISTW(), HDR(), c.right - PANEL(), c.bottom - STATUSH()};
}

RECT ImageRect() {
    RECT v = CanvasRect(), r{0, 0, 0, 0};
    if (!g_img) return r;
    int m = S(14), top = v.top + S(32);
    int aw = v.right - v.left - 2 * m, ah = v.bottom - top - m;
    if (aw < 10 || ah < 10) return r;
    double s = std::min((double)aw / g_img->GetWidth(), (double)ah / g_img->GetHeight());
    int w = (int)(g_img->GetWidth() * s), h = (int)(g_img->GetHeight() * s);
    r.left = v.left + m + (aw - w) / 2; r.top = top; r.right = r.left + w; r.bottom = r.top + h;
    return r;
}

Rect SelPx(RECT r) {
    int w = r.right - r.left, h = r.bottom - r.top;
    int x0 = r.left + (int)(g_sel.x0 * w + .5), x1 = r.left + (int)(g_sel.x1 * w + .5);
    int y0 = r.top + (int)(g_sel.y0 * h + .5), y1 = r.top + (int)(g_sel.y1 * h + .5);
    return Rect(x0, y0, x1 - x0, y1 - y0);
}

void MouseToFrac(RECT r, int x, int y, double& fx, double& fy) {
    fx = std::clamp((double)(x - r.left) / (r.right - r.left), 0.0, 1.0);
    fy = std::clamp((double)(y - r.top) / (r.bottom - r.top), 0.0, 1.0);
}

// Is (x,y) inside the free shape? (ray casting on the four corners, in window pixels)
bool InQuad(RECT r, int x, int y) {
    int w = r.right - r.left, h = r.bottom - r.top;
    bool in = false;
    for (int i = 0, j = 3; i < 4; j = i++) {
        double xi = r.left + g_sel.qx[i] * w, yi = r.top + g_sel.qy[i] * h, xj = r.left + g_sel.qx[j] * w, yj = r.top + g_sel.qy[j] * h;
        if (((yi > y) != (yj > y)) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) in = !in;
    }
    return in;
}

int HitTest(RECT r, int x, int y, int& mask) {  // what would a click at (x,y) do?
    mask = 0;
    if (!g_sel.has) return M_NEW;
    if (g_sel.quad) {  // a free shape: grab a corner, or the inside to move the whole shape
        int w = r.right - r.left, h = r.bottom - r.top, t = S(11);
        for (int i = 0; i < 4; i++)
            if (abs(x - (r.left + (int)(g_sel.qx[i] * w + .5))) <= t && abs(y - (r.top + (int)(g_sel.qy[i] * h + .5))) <= t) { mask = i; return M_CORNER; }
        return InQuad(r, x, y) ? M_MOVE : M_NEW;
    }
    Rect s = SelPx(r);
    int t = S(8), R = s.X + s.Width, B = s.Y + s.Height;
    if (x >= s.X - t && x <= R + t && y >= s.Y - t && y <= B + t) {
        if (abs(x - s.X) <= t) mask |= 1; else if (abs(x - R) <= t) mask |= 2;
        if (abs(y - s.Y) <= t) mask |= 4; else if (abs(y - B) <= t) mask |= 8;
        if (mask) return M_SIZE;
    }
    if (x >= s.X && x <= R && y >= s.Y && y <= B) return M_MOVE;
    return M_NEW;
}

LPCWSTR CursorFor(int mode, int mask) {
    if (mode == M_MOVE || mode == M_CORNER) return IDC_SIZEALL;
    if (mode == M_NEW) return IDC_CROSS;
    if (mask == 1 || mask == 2) return IDC_SIZEWE;
    if (mask == 4 || mask == 8) return IDC_SIZENS;
    return (mask == 5 || mask == 10) ? IDC_SIZENWSE : IDC_SIZENESW;
}

void Txt(HDC m, HFONT f, COLORREF col, const std::wstring& t, RECT r, UINT fmt) {
    SelectObject(m, f);
    SetTextColor(m, col);
    SetBkMode(m, TRANSPARENT);
    DrawTextW(m, t.c_str(), -1, &r, fmt | DT_NOPREFIX | ((fmt & DT_WORDBREAK) ? 0 : DT_SINGLELINE));
}

void Fill(HDC m, RECT r, COLORREF col) {
    HBRUSH b = CreateSolidBrush(col);
    FillRect(m, &r, b);
    DeleteObject(b);
}

std::wstring Pct(int n, int t) { return std::to_wstring(n) + L" / " + std::to_wstring(t) + L"  (" + std::to_wstring(t ? n * 100 / t : 0) + L"%)"; }

// Overall progress of the running job: detect is pass 1 = 0-70%, pass 2 = 70-100%.
int ProgressPct() {
    int t = g_totalN, d = g_doneN, p = t ? d * 100 / t : 0;
    if (g_kind == 1) p = g_phase == 1 ? p * 70 / 100 : 70 + p * 30 / 100;
    return std::clamp(p, 0, 100);
}

std::wstring StatusText() {
    if (g_busy) {
        if (g_cancel) return L"Cancelling...";
        if (g_phase == 1) return L"Auto-detect, pass 1 of 2: reading the forms   " + Pct(g_doneN, g_totalN);
        if (g_phase == 2) return L"Auto-detect, pass 2 of 2: re-checking unclear forms   " + Pct(g_doneN, g_totalN);
        return L"Saving cropped photos   " + Pct(g_doneN, g_totalN);
    }
    if (!g_note.empty()) return g_note;
    int picked = (int)SendMessageW(Ctl(ID_LIST), LB_GETSELCOUNT, 0, 0);
    if (picked >= 2) return std::to_wstring(picked) + L" images selected: fix the current box and click \"Copy box to selected\", or click \"Update selected\" to re-export just those.";
    Counts c = Count();
    if (!c.n) return L"Start: click \"1. Source folder\" and choose the folder with your scanned forms.";
    if (!c.box && !g_sel.has) return L"Next: click \"Auto-select all\" to find every photo and review them, or \"Auto-crop all\" to find and export in one go.";
    if (!c.box) return L"Click \"Copy box to unchecked\" to use this box on every image, then review them.";
    if (g_unconf) return L"This box is only a guess. Drag it to fit the photo, then press Enter (or click Confirm).";
    if (c.check) return std::to_wstring(c.check) + L" images need a better crop or a box (▲ ✕ tabs above the list). Click \"Next to check\", fix the box if needed, press Enter.";
    if (c.saved && c.pending) return std::to_wstring(c.pending) + L" images are new or changed since the last export. Click \"Done & Export\" to update only those.";
    if (c.saved == c.n) return L"Everything is exported. Edit any image and click \"Update this one\" (or \"Done & Export\") to re-export just the changes.";
    return L"All images have a box. Look through them if you like, then click \"Done & Export\".";
}

// ---- modern look: palette, rounded shapes, owner-drawn buttons and list rows, custom progress bar
const COLORREF cBg = RGB(244, 246, 250), cCard = RGB(255, 255, 255), cLine = RGB(226, 230, 237), cText = RGB(28, 33, 45), cDim = RGB(110, 118, 135);
const COLORREF cAccent = RGB(47, 111, 237), cGreen = RGB(31, 157, 99), cAmber = RGB(240, 160, 30), cPurple = RGB(124, 92, 255), cCanvas = RGB(30, 33, 40);
const COLORREF cSelBg = RGB(231, 239, 254), cRed = RGB(214, 69, 69);

HFONT g_fontBtn;
int g_hover = 0;  // id of the button under the mouse

HFONT MkFont(int pt, int weight, const wchar_t* face) {
    return CreateFontW(-MulDiv(pt, g_dpi, 72), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face);
}
void MakeFonts() {  // called at startup and again whenever the zoom (DPI) changes
    g_font = MkFont(9, FW_NORMAL, L"Segoe UI");
    g_fontBig = MkFont(24, FW_BOLD, L"Segoe UI");
    g_fontBtn = MkFont(9, FW_SEMIBOLD, L"Segoe UI Semibold");
    g_fontB = MkFont(8, FW_BOLD, L"Segoe UI");
}

Color C(COLORREF c, int a = 255) { return Color((BYTE)a, GetRValue(c), GetGValue(c), GetBValue(c)); }

void RoundPath(GraphicsPath& p, REAL x, REAL y, REAL w, REAL h, REAL r) {
    REAL d = std::min(r * 2, std::min(w, h));
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
}

enum BtnKind { BK_SECONDARY, BK_PRIMARY, BK_SUCCESS, BK_DANGER, BK_TAB };
BtnKind KindOf(int id) {
    if (id == ID_DETECT || id == ID_SAVEALL) return BK_PRIMARY;
    if (id == ID_CONFIRM) return BK_SUCCESS;
    if (id == ID_CANCEL) return BK_DANGER;
    if ((id >= ID_FALL && id <= ID_FMISSING) || id == ID_SHAPE) return BK_TAB;
    return BK_SECONDARY;
}

void DrawButton(const DRAWITEMSTRUCT* d) {
    HDC dc = d->hDC;
    RECT r = d->rcItem;
    int id = (int)d->CtlID;
    bool dis = d->itemState & ODS_DISABLED, down = d->itemState & ODS_SELECTED, hov = g_hover == id && !dis;
    wchar_t txt[128];
    GetWindowTextW(d->hwndItem, txt, 128);
    Fill(dc, r, id == ID_CANCEL || (id >= ID_FALL && id <= ID_FMISSING) ? cCard : cBg);  // what shows in the rounded corners
    COLORREF fill, border = 0, text;
    bool hasBorder = false;
    switch (KindOf(id)) {
    case BK_PRIMARY:
        fill = dis ? RGB(184, 201, 238) : down ? RGB(30, 85, 200) : hov ? RGB(38, 98, 222) : cAccent;
        text = dis ? RGB(240, 244, 255) : RGB(255, 255, 255);
        break;
    case BK_SUCCESS:
        fill = dis ? RGB(170, 214, 192) : down ? RGB(20, 120, 76) : hov ? RGB(25, 140, 88) : cGreen;
        text = dis ? RGB(238, 249, 243) : RGB(255, 255, 255);
        break;
    case BK_DANGER:
        fill = down ? RGB(250, 217, 217) : hov ? RGB(253, 236, 236) : cCard;
        border = RGB(214, 69, 69); hasBorder = true; text = RGB(200, 55, 55);
        break;
    case BK_TAB: {  // filter tab above the list: the active one is tinted with an accent border
        bool on = id == ID_SHAPE ? g_sel.quad : id - ID_FALL == g_filter;  // Free shape is lit while the box is a free shape
        fill = on ? cSelBg : hov ? RGB(240, 243, 249) : cCard;
        border = on ? cAccent : cLine; hasBorder = true;
        text = dis ? RGB(160, 166, 181) : on ? cAccent : cText;
        break;
    }
    default:
        fill = dis ? RGB(240, 242, 246) : down ? RGB(228, 233, 242) : hov ? RGB(238, 242, 250) : cCard;
        border = dis ? RGB(228, 231, 237) : hov ? RGB(160, 180, 225) : RGB(212, 218, 228); hasBorder = true;
        text = dis ? RGB(160, 166, 181) : cText;
    }
    {
        Graphics g(dc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        GraphicsPath p;
        RoundPath(p, r.left + 0.5f, r.top + 0.5f, (REAL)(r.right - r.left - 1), (REAL)(r.bottom - r.top - 1), (REAL)S(8));
        SolidBrush b(C(fill));
        g.FillPath(&b, &p);
        if (hasBorder) { Pen pen(C(border), 1.0f); g.DrawPath(&pen, &p); }
        if ((d->itemState & ODS_FOCUS) && !(d->itemState & ODS_NOFOCUSRECT)) {  // keyboard focus ring (Tab)
            GraphicsPath fp;
            RoundPath(fp, r.left + 3.0f, r.top + 3.0f, (REAL)(r.right - r.left - 6), (REAL)(r.bottom - r.top - 6), (REAL)S(6));
            BtnKind k = KindOf(id);
            Pen ring(k == BK_PRIMARY || k == BK_SUCCESS ? C(RGB(255, 255, 255)) : C(cAccent), 2.0f);
            g.DrawPath(&ring, &fp);
        }
    }
    SelectObject(dc, g_fontBtn);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, text);
    DrawTextW(dc, txt, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

LRESULT CALLBACK BtnProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
    if (m == WM_MOUSEMOVE && g_hover != (int)id) {
        g_hover = (int)id;
        TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
        TrackMouseEvent(&t);
        InvalidateRect(h, nullptr, FALSE);
    } else if (m == WM_MOUSELEAVE) {
        if (g_hover == (int)id) g_hover = 0;
        InvalidateRect(h, nullptr, FALSE);
    } else if (m == WM_SETCURSOR && IsWindowEnabled(h)) {
        SetCursor(LoadCursor(nullptr, IDC_HAND));
        return TRUE;
    } else if (m == WM_KEYDOWN) {  // keyboard use: Enter presses the button, review keys go to the window
        if (w == VK_RETURN) { SendMessageW(h, BM_CLICK, 0, 0); return 0; }
        if (w == VK_LEFT || w == VK_RIGHT || w == VK_UP || w == VK_DOWN || w == VK_ESCAPE) return SendMessageW(GetParent(h), m, w, l);
    } else if (m == WM_NCDESTROY) RemoveWindowSubclass(h, BtnProc, id);
    return DefSubclassProc(h, m, w, l);
}

// The file list keeps Up/Down for itself but hands the review keys to the window.
LRESULT CALLBACK ListProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
    if (m == WM_KEYDOWN && (w == VK_RETURN || w == VK_ESCAPE || w == VK_LEFT || w == VK_RIGHT)) return SendMessageW(GetParent(h), m, w, l);
    if (m == WM_NCDESTROY) RemoveWindowSubclass(h, ListProc, id);
    return DefSubclassProc(h, m, w, l);
}

COLORREF GlyphColor(const Item& i) {
    switch (StatusOf(i)) {
    case ST_DONE: return cGreen;
    case ST_REVIEW: return cAmber;
    case ST_MISSING: return cRed;
    default: return i.src == S_USER ? cPurple : cAccent;  // waiting to be exported
    }
}
const wchar_t* GlyphOf(const Item& i) {
    switch (StatusOf(i)) {
    case ST_DONE: return L"✓";
    case ST_REVIEW: return L"▲";
    case ST_MISSING: return L"✕";
    default: return i.src == S_USER ? L"◆" : L"●";
    }
}
// Short note on the right of a row; nothing for finished images.
const wchar_t* LabelOf(const Item& i) {
    switch (StatusOf(i)) {
    case ST_DONE: return nullptr;
    case ST_REVIEW: return i.saved ? L"better crop?" : L"check";
    case ST_MISSING: return i.failed ? L"export failed" : L"not done";
    default: return L"waiting";
    }
}

// Row `d->itemID` of the (filtered) list. Selected rows are tinted; the image shown on the canvas also gets an accent bar.
void DrawListItem(const DRAWITEMSTRUCT* d) {
    if (d->itemID == (UINT)-1 || d->itemID >= g_view.size()) return;
    int idx = g_view[d->itemID];
    HDC dc = d->hDC;
    RECT r = d->rcItem;
    bool sel = d->itemState & ODS_SELECTED, dis = g_busy, cur = idx == g_idx;
    const Item& it = g_items[idx];
    Fill(dc, r, cCard);
    if (sel || cur) {
        Graphics g(dc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        GraphicsPath p;
        RoundPath(p, (REAL)(r.left + S(8)), (REAL)(r.top + S(2)), (REAL)(r.right - r.left - S(16)), (REAL)(r.bottom - r.top - S(4)), (REAL)S(7));
        SolidBrush b(C(sel ? cSelBg : RGB(244, 246, 250)));
        g.FillPath(&b, &p);
        if (cur) {
            SolidBrush a(C(cAccent));
            g.FillRectangle(&a, r.left + S(8), r.top + S(9), S(3), r.bottom - r.top - S(18));
        }
    }
    SelectObject(dc, g_font);
    SetBkMode(dc, TRANSPARENT);
    RECT gr{r.left + S(20), r.top, r.left + S(40), r.bottom}, nr{r.left + S(44), r.top, r.right - S(112), r.bottom};
    SetTextColor(dc, dis ? RGB(170, 175, 188) : GlyphColor(it));
    DrawTextW(dc, GlyphOf(it), -1, &gr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SetTextColor(dc, dis ? RGB(150, 156, 170) : cur ? cAccent : cText);
    std::wstring name = it.path.filename().wstring();
    DrawTextW(dc, name.c_str(), -1, &nr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (const wchar_t* lbl = LabelOf(it)) {
        RECT lr{r.right - S(112), r.top, r.right - S(16), r.bottom};
        SetTextColor(dc, dis ? RGB(170, 175, 188) : StatusOf(it) == ST_WAITING ? cDim : GlyphColor(it));
        DrawTextW(dc, lbl, -1, &lr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
}

// Rounded progress bar: gradient fill with a soft moving shimmer.
void DrawProgress(Graphics& g, REAL x, REAL y, REAL w, REAL h, int pct, COLORREF track) {
    GraphicsPath tp;
    RoundPath(tp, x, y, w, h, h / 2);
    SolidBrush tb(C(track));
    g.FillPath(&tb, &tp);
    REAL fw = std::max(h, w * pct / 100);
    if (pct <= 0) return;
    GraphicsPath fp;
    RoundPath(fp, x, y, fw, h, h / 2);
    LinearGradientBrush gb(PointF(x, y), PointF(x + std::max(fw, 2.0f), y), C(RGB(76, 141, 255)), C(RGB(46, 211, 196)));
    g.FillPath(&gb, &fp);
    REAL bandW = h * 5, pos = (REAL)((GetTickCount() / 5) % (DWORD)(fw + bandW)) - bandW;
    Region old;
    g.GetClip(&old);
    g.SetClip(&fp, CombineModeIntersect);
    LinearGradientBrush sb(PointF(x + pos, y), PointF(x + pos + bandW, y), C(RGB(255, 255, 255), 0), C(RGB(255, 255, 255), 90));
    Color cols[3] = {C(RGB(255, 255, 255), 0), C(RGB(255, 255, 255), 90), C(RGB(255, 255, 255), 0)};
    REAL offs[3] = {0, 0.5f, 1};
    sb.SetInterpolationColors(cols, offs, 3);
    g.FillRectangle(&sb, x + pos, y, bandW, h);
    g.SetClip(&old);
}

void Paint(HDC dc, RECT clip) {
    RECT c; GetClientRect(g_wnd, &c);
    HDC m = CreateCompatibleDC(dc);
    HBITMAP bm = CreateCompatibleBitmap(dc, c.right, c.bottom);
    HGDIOBJ old = SelectObject(m, bm);
    RECT cv = CanvasRect();
    RECT pnl{c.right - PANEL(), HDR(), c.right, c.bottom - STATUSH()};
    RECT stat{0, c.bottom - STATUSH(), c.right, c.bottom};
    int mg = S(14), by = S(12), fh = S(32), px0 = mg + S(30) + S(180) + S(10);

    // 1. backgrounds
    Fill(m, RECT{0, 0, c.right, HDR()}, cBg);
    Fill(m, RECT{0, HDR() - 1, c.right, HDR()}, cLine);
    Fill(m, RECT{0, HDR(), LISTW(), c.bottom - STATUSH()}, cCard);
    Fill(m, RECT{LISTW() - 1, HDR(), LISTW(), c.bottom - STATUSH()}, cLine);
    Fill(m, RECT{0, HDR() + LEGEND() - 1, LISTW() - 1, HDR() + LEGEND()}, cLine);
    Fill(m, cv, cCanvas);
    Fill(m, pnl, cCard);
    Fill(m, RECT{pnl.left, pnl.top, pnl.left + 1, pnl.bottom}, cLine);
    Fill(m, stat, cCard);
    Fill(m, RECT{0, stat.top, c.right, stat.top + 1}, cLine);

    RECT ir = ImageRect();
    int rowY[2] = {by, S(50)};
    int badge = S(18);
    std::wstring sizeText;
    RECT sizeRc{0, 0, 0, 0};
    RECT cardRc{0, 0, 0, 0};

    // 2. shapes and images
    {
        Graphics g(m);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetInterpolationMode(InterpolationModeHighQualityBilinear);
        g.SetClip(Rect(clip.left, clip.top, clip.right - clip.left, clip.bottom - clip.top));
        // path pills
        for (int i = 0; i < 2; i++) {
            GraphicsPath p;
            RoundPath(p, (REAL)px0, rowY[i] + 0.5f, (REAL)(c.right - mg - px0), (REAL)(fh - 1), (REAL)S(8));
            SolidBrush b(C(cCard));
            Pen pen(C(cLine), 1.0f);
            g.FillPath(&b, &p);
            g.DrawPath(&pen, &p);
            SolidBrush a(C(cAccent));
            g.FillEllipse(&a, mg, rowY[i] + (fh - badge) / 2, badge, badge);
        }
        SolidBrush acc(C(cAccent));
        for (int i = 0; i < 3; i++) g.FillEllipse(&acc, g_capX[i], g_capY[i], badge, badge);
        // status dot
        Counts cc = Count();
        COLORREF dotc = g_busy ? cAccent : !cc.n ? cDim : cc.check ? cAmber : cGreen;
        SolidBrush dot(C(dotc));
        g.FillEllipse(&dot, mg, stat.top + (STATUSH() - S(9)) / 2, S(9), S(9));
        if (g_busy) DrawProgress(g, (REAL)(c.right - S(430) - S(120)), (REAL)(stat.top + (STATUSH() - S(8)) / 2), (REAL)S(430), (REAL)S(8), ProgressPct(), cLine);

        if (g_img) {
            int w = ir.right - ir.left, h = ir.bottom - ir.top;
            if (!g_disp || g_dispW != w || g_dispH != h) {  // scale once per image and window size, not on every repaint
                g_disp = std::make_unique<Bitmap>(w, h, PixelFormat32bppPARGB);
                Graphics gd(g_disp.get());
                gd.SetInterpolationMode(InterpolationModeHighQualityBilinear);
                gd.DrawImage(g_img.get(), 0, 0, w, h);
                g_dispW = w; g_dispH = h;
            }
            g.DrawImage(g_disp.get(), ir.left, ir.top, w, h);
            if (g_sel.has && g_sel.quad) {  // free shape: outline, dimmed outside, four corner handles, straightened preview
                PointF pts[4];
                for (int i = 0; i < 4; i++) pts[i] = PointF((REAL)(ir.left + g_sel.qx[i] * w), (REAL)(ir.top + g_sel.qy[i] * h));
                GraphicsPath outside;
                outside.SetFillMode(FillModeAlternate);  // the image rectangle minus the shape
                outside.AddRectangle(Rect(ir.left, ir.top, w, h));
                outside.AddPolygon(pts, 4);
                SolidBrush dim(Color(120, 0, 0, 0));
                g.FillPath(&dim, &outside);
                Color accent = g_unconf ? C(cAmber) : C(RGB(70, 150, 255));
                Pen pen(accent, (REAL)std::max(2, S(2)));
                g.DrawPolygon(&pen, pts, 4);
                REAL hs = (REAL)S(12);
                SolidBrush white(Color(255, 255, 255));
                Pen hp(accent, 2.0f);
                for (int i = 0; i < 4; i++) {
                    g.FillEllipse(&white, pts[i].X - hs / 2, pts[i].Y - hs / 2, hs, hs);
                    g.DrawEllipse(&hp, pts[i].X - hs / 2, pts[i].Y - hs / 2, hs, hs);
                }
                int nw, nh, pw = pnl.right - pnl.left - S(32), ph = pnl.bottom - pnl.top - S(100);
                QuadSize(g_sel, g_img->GetWidth(), g_img->GetHeight(), nw, nh);
                if (pw > 10 && ph > 10) {
                    double k = std::min((double)pw / nw, (double)ph / nh);
                    int dw = std::max(4, (int)(nw * k)), dh = std::max(4, (int)(nh * k));
                    int dx = pnl.left + S(16) + (pw - dw) / 2, dy = pnl.top + S(40);
                    if (auto warped = WarpQuad(*g_img, g_sel, dw, dh)) g.DrawImage(warped.get(), dx, dy, dw, dh);
                    Pen bp(C(cLine), 1.0f);
                    g.DrawRectangle(&bp, Rect(dx, dy, dw, dh));
                    wchar_t buf[64];
                    swprintf_s(buf, L"%d x %d px", nw, nh);
                    sizeText = buf;
                    sizeRc = RECT{pnl.left + S(16), dy + dh + S(10), pnl.right - S(16), dy + dh + S(32)};
                }
            } else if (g_sel.has) {
                Rect s = SelPx(ir);
                SolidBrush dim(Color(120, 0, 0, 0));
                g.FillRectangle(&dim, Rect(ir.left, ir.top, w, s.Y - ir.top));
                g.FillRectangle(&dim, Rect(ir.left, s.Y + s.Height, w, ir.bottom - (s.Y + s.Height)));
                g.FillRectangle(&dim, Rect(ir.left, s.Y, s.X - ir.left, s.Height));
                g.FillRectangle(&dim, Rect(s.X + s.Width, s.Y, ir.right - (s.X + s.Width), s.Height));
                Color accent = g_unconf ? C(cAmber) : C(RGB(70, 150, 255));
                Pen pen(accent, (REAL)std::max(2, S(2)));
                g.DrawRectangle(&pen, s);
                int hs = S(9);
                SolidBrush white(Color(255, 255, 255));
                Pen hp(accent, 1.6f);
                int xs[3] = {s.X, s.X + s.Width / 2, s.X + s.Width}, ys[3] = {s.Y, s.Y + s.Height / 2, s.Y + s.Height};
                for (int i = 0; i < 3; i++)
                    for (int j = 0; j < 3; j++) {
                        if (i == 1 && j == 1) continue;
                        g.FillEllipse(&white, xs[i] - hs / 2, ys[j] - hs / 2, hs, hs);
                        g.DrawEllipse(&hp, xs[i] - hs / 2, ys[j] - hs / 2, hs, hs);
                    }
                double sx = g_sel.x0 * g_img->GetWidth(), sy = g_sel.y0 * g_img->GetHeight();
                double sw = (g_sel.x1 - g_sel.x0) * g_img->GetWidth(), sh = (g_sel.y1 - g_sel.y0) * g_img->GetHeight();
                int pw = pnl.right - pnl.left - S(32), ph = pnl.bottom - pnl.top - S(100);
                if (pw > 10 && ph > 10 && sw >= 1 && sh >= 1) {
                    double k = std::min(pw / sw, ph / sh);
                    int dw = (int)(sw * k), dh = (int)(sh * k);
                    int dx = pnl.left + S(16) + (pw - dw) / 2, dy = pnl.top + S(40);
                    g.DrawImage(g_img.get(), RectF((REAL)dx, (REAL)dy, (REAL)dw, (REAL)dh), (REAL)sx, (REAL)sy, (REAL)sw, (REAL)sh, UnitPixel);
                    Pen bp(C(cLine), 1.0f);
                    g.DrawRectangle(&bp, Rect(dx, dy, dw, dh));
                    wchar_t buf[64];
                    swprintf_s(buf, L"%d x %d px", (int)(sw + .5), (int)(sh + .5));
                    sizeText = buf;
                    sizeRc = RECT{pnl.left + S(16), dy + dh + S(10), pnl.right - S(16), dy + dh + S(32)};
                }
            }
        }
        if (g_busy) {  // progress card over the image
            int bw = S(520), bh = S(176);
            cardRc = RECT{(cv.left + cv.right - bw) / 2, (cv.top + cv.bottom - bh) / 2, 0, 0};
            cardRc.right = cardRc.left + bw; cardRc.bottom = cardRc.top + bh;
            for (int k = 7; k >= 1; k--) {  // soft shadow
                GraphicsPath sp;
                RoundPath(sp, (REAL)(cardRc.left - k * 2), (REAL)(cardRc.top - k * 2 + S(8)), (REAL)(bw + k * 4), (REAL)(bh + k * 4), (REAL)(S(18) + k * 2));
                SolidBrush sb(Color(12, 0, 0, 0));
                g.FillPath(&sb, &sp);
            }
            GraphicsPath cp;
            RoundPath(cp, (REAL)cardRc.left, (REAL)cardRc.top, (REAL)bw, (REAL)bh, (REAL)S(18));
            SolidBrush cb(Color(255, 37, 41, 52));
            g.FillPath(&cb, &cp);
            DrawProgress(g, (REAL)(cardRc.left + S(30)), (REAL)(cardRc.top + S(106)), (REAL)(bw - S(60)), (REAL)S(12), ProgressPct(), RGB(60, 66, 82));
        }
    }

    // 3. text
    SelectObject(m, g_font);
    Counts cc = Count();
    auto path = [&](int y, const fs::path& p, const wchar_t* none) {
        Txt(m, g_font, p.empty() ? cDim : cText, p.empty() ? none : p.wstring(), RECT{px0 + S(12), y, c.right - mg - S(12), y + fh}, DT_VCENTER | DT_PATH_ELLIPSIS);
    };
    path(rowY[0], g_src, L"No folder chosen yet");
    path(rowY[1], g_dst, L"Not chosen yet - you will be asked when you save");
    const wchar_t* nums1[2] = {L"1", L"2"};
    for (int i = 0; i < 2; i++) Txt(m, g_fontB, RGB(255, 255, 255), nums1[i], RECT{mg, rowY[i] + (fh - badge) / 2, mg + badge, rowY[i] + (fh - badge) / 2 + badge}, DT_CENTER | DT_VCENTER);
    const wchar_t* nums[3] = {L"3", L"4", L"5"};
    const wchar_t* caps[3] = {L"Find the photos", L"Review", L"Save"};
    for (int i = 0; i < 3; i++) {
        Txt(m, g_fontB, RGB(255, 255, 255), nums[i], RECT{g_capX[i], g_capY[i], g_capX[i] + badge, g_capY[i] + badge}, DT_CENTER | DT_VCENTER);
        Txt(m, g_fontB, cText, caps[i], RECT{g_capX[i] + badge + S(8), g_capY[i] - 1, g_capX[i] + S(260), g_capY[i] + badge + 1}, DT_VCENTER);
    }
    {   // legend above the list, glyphs in their list colours
        struct L { const wchar_t* g; const wchar_t* t; COLORREF col; } leg[4] = {{L"✓", L"done", cGreen}, {L"▲", L"better crop?", cAmber}, {L"✕", L"not done", cRed}, {L"●", L"waiting", cAccent}};
        int x = S(14), y = HDR() + S(40);  // under the filter tabs
        for (auto& e : leg) {
            SIZE sz;
            SelectObject(m, g_font);
            GetTextExtentPoint32W(m, e.g, 1, &sz);
            Txt(m, g_font, e.col, e.g, RECT{x, y, x + sz.cx + 2, y + S(22)}, DT_VCENTER);
            x += sz.cx + S(4);
            GetTextExtentPoint32W(m, e.t, (int)wcslen(e.t), &sz);
            Txt(m, g_font, cDim, e.t, RECT{x, y, x + sz.cx + 2, y + S(22)}, DT_VCENTER);
            x += sz.cx + S(12);
        }
    }
    Txt(m, g_font, cText, StatusText(), RECT{mg + S(20), stat.top, c.right - S(580), stat.bottom}, DT_VCENTER | DT_END_ELLIPSIS);

    if (g_items.empty()) Txt(m, g_font, RGB(150, 156, 170), L"Choose a source folder to begin", cv, DT_CENTER | DT_VCENTER);
    else if (!g_img) Txt(m, g_font, RGB(240, 150, 150), L"Cannot open this image", cv, DT_CENTER | DT_VCENTER);
    else {
        std::wstring t = std::to_wstring(g_idx + 1) + L" / " + std::to_wstring(g_items.size()) + L"    " + g_items[g_idx].path.filename().wstring() +
                         L"    " + std::to_wstring(g_img->GetWidth()) + L" x " + std::to_wstring(g_img->GetHeight()) + L" px";
        Txt(m, g_font, RGB(205, 210, 220), t, RECT{cv.left + S(16), cv.top + S(6), cv.right - S(320), cv.top + S(30)}, DT_VCENTER | DT_END_ELLIPSIS);
        const Item& it = g_items[g_idx];
        std::wstring bs = !g_sel.has ? L"No box yet: drag around the photo" : g_unconf ? (it.saved ? L"Exported from a guess: improve it, Enter, Update" : L"Guess: adjust, then press Enter") : it.src == S_AUTO ? L"Box: auto-detected" : L"Box: set by you";
        if (g_sel.has && g_sel.quad) bs += L"  ·  free shape";
        COLORREF bc = (!g_sel.has || g_unconf) ? cAmber : RGB(110, 180, 255);
        Txt(m, g_fontB, bc, bs, RECT{cv.right - S(310), cv.top + S(6), cv.right - S(16), cv.top + S(30)}, DT_VCENTER | DT_RIGHT);
    }
    Txt(m, g_fontB, cDim, L"CROP PREVIEW", RECT{pnl.left + S(16), pnl.top + S(10), pnl.right - S(16), pnl.top + S(30)}, DT_VCENTER);
    if (!sizeText.empty()) Txt(m, g_fontB, cText, sizeText, sizeRc, DT_VCENTER);
    if (!g_img || !g_sel.has)
        Txt(m, g_font, cDim, L"Select the photo area to see the cropped result here.", RECT{pnl.left + S(16), pnl.top + S(44), pnl.right - S(16), pnl.top + S(100)}, DT_WORDBREAK);
    if (g_busy) {
        Txt(m, g_fontB, RGB(150, 160, 180), g_kind == 1 ? L"AUTO-DETECTING PHOTOS" : L"SAVING CROPPED PHOTOS", RECT{cardRc.left + S(30), cardRc.top + S(20), cardRc.right - S(30), cardRc.top + S(42)}, DT_VCENTER);
        Txt(m, g_fontBig, RGB(255, 255, 255), std::to_wstring(ProgressPct()) + L"%", RECT{cardRc.left + S(30), cardRc.top + S(44), cardRc.right - S(30), cardRc.top + S(98)}, DT_VCENTER);
        Txt(m, g_font, RGB(170, 178, 195), StatusText(), RECT{cardRc.left + S(30), cardRc.top + S(128), cardRc.right - S(30), cardRc.top + S(156)}, DT_VCENTER | DT_END_ELLIPSIS);
    }
    BitBlt(dc, clip.left, clip.top, clip.right - clip.left, clip.bottom - clip.top, m, clip.left, clip.top, SRCCOPY);
    SelectObject(m, old); DeleteObject(bm); DeleteDC(m);
}

void Layout() {
    RECT c; GetClientRect(g_wnd, &c);
    int m = S(14), bh = S(34), gap = S(6), y1 = S(12), y2 = S(50), fh = S(32);
    MoveWindow(Ctl(ID_SRC), m + S(30), y1, S(180), fh, TRUE);
    MoveWindow(Ctl(ID_DST), m + S(30), y2, S(180), fh, TRUE);
    int x = m, y3 = S(118);
    auto put = [&](int id, int w) { MoveWindow(Ctl(id), x, y3, S(w), bh, TRUE); x += S(w) + gap; };
    g_capX[0] = x; g_capY[0] = S(94);
    put(ID_DETECT, 126); put(ID_AUTOCROP, 112); put(ID_APPLY, 176);
    x += S(16); g_capX[1] = x; g_capY[1] = S(94);
    put(ID_PREV, 62); put(ID_NEXT, 62); put(ID_CONFIRM, 96); put(ID_NEXTCHK, 116); put(ID_SHAPE, 104);
    x += S(16);
    bool wrap = x + S(132 + 120 + 6) + m > c.right;  // the Save group does not fit on this row: give it a second row
    g_hdrExtra = wrap ? S(66) : 0;
    if (wrap) { x = m; y3 = S(182); g_capY[2] = S(158); } else g_capY[2] = S(94);
    g_capX[2] = x;
    put(ID_SAVEONE, 132); put(ID_SAVEALL, 120);
    int tg = S(6), tw = (LISTW() - S(20) - 3 * tg) / 4;  // four filter tabs above the list
    const int tabs[4] = {ID_FALL, ID_FDONE, ID_FREVIEW, ID_FMISSING};
    for (int i = 0; i < 4; i++) MoveWindow(Ctl(tabs[i]), S(10) + i * (tw + tg), HDR() + S(8), tw, S(28), TRUE);
    MoveWindow(Ctl(ID_LIST), 0, HDR() + LEGEND(), LISTW() - 1, std::max(10, (int)c.bottom - HDR() - LEGEND() - STATUSH()), TRUE);
    MoveWindow(Ctl(ID_CANCEL), c.right - S(112), c.bottom - STATUSH() + S(4), S(98), STATUSH() - S(8), TRUE);
}

// ---- jobs
void StartJob(int kind) {
    g_kind = kind; g_busy = true; g_cancel = false; g_doneN = 0; g_totalN = 0;
    g_ctrl = std::thread([kind] {
        if (kind == 1) RunDetect(g_job); else RunSave(g_job);
        PostMessageW(g_wnd, WM_JOBDONE, 0, 0);
    });
    SetTimer(g_wnd, 1, 60, nullptr);
    UpdateUi();
}

bool g_autoExport = false;  // Auto-crop all: export straight after detection, without waiting for review

void StartDetect(bool silent = false) {
    Job& j = g_job;
    j = Job();
    int manual = 0;
    for (auto& i : g_items) { j.paths.push_back(i.path); j.todo.push_back(i.src != S_USER); manual += i.src == S_USER; }
    if (!silent && manual && MessageBoxW(g_wnd, (L"Keep the " + std::to_wstring(manual) + L" boxes you already set yourself (they are skipped)?\n\nYes = keep them, No = re-detect everything.").c_str(), L"PhotoCrop", MB_YESNO | MB_ICONQUESTION) == IDNO)
        std::fill(j.todo.begin(), j.todo.end(), 1);
    StartJob(1);
}

bool EnsureDst() {
    if (g_dst.empty() && !(PickFolder(g_wnd, g_dst, L"Choose the folder where the cropped photos will be saved") && !g_dst.empty())) return false;
    std::error_code ec;
    if (fs::equivalent(g_src, g_dst, ec)) {
        MessageBoxW(g_wnd, L"The destination must be a different folder from the source, or the original scans would be overwritten.", L"PhotoCrop", MB_ICONWARNING);
        g_dst.clear();
        return false;
    }
    SaveSettings();
    return true;
}

bool g_autoCropRun = false;     // the running export belongs to Auto-crop all: finish with a completion summary
bool g_selectedExport = false;  // the running export is "Update N selected": its summary names only those
int g_selectedSkipped = 0;      // selected images that had no box and were left out of that export

// Starts the save job for the images flagged in todo.
void StartExportTodo(const std::vector<char>& todo) {
    Job& j = g_job;
    j = Job();
    j.todo = todo;
    int n = 0;
    for (size_t k = 0; k < g_items.size(); k++) { j.paths.push_back(g_items[k].path); j.sel.push_back(g_items[k].sel); n += todo[k]; }
    if (!n) { g_autoCropRun = g_selectedExport = false; g_note = L"Nothing to export: everything is already up to date."; UpdateUi(); return; }
    j.dst = g_dst;
    StartJob(2);
}

// Exports the images that need it. all = every image that has a box; otherwise only new or changed ones
// (unconfirmed guesses only when withGuess).
void StartExport(bool withGuess, bool all) { StartExportTodo(ExportSet(g_items, withGuess, all)); }

// "Update N selected": exports exactly the selected images that have a box (guesses too, you picked them),
// so one or many files in the destination folder can be updated after a better crop.
void ExportSelected();
void SaveOne();

// "Done & Export": writes only what is new or changed since the last export.
void ExportClicked() {
    if (!EnsureDst()) { UpdateUi(); return; }
    Counts c = Count();
    if (!c.pending && !c.guessPending) {
        if (!c.saved) { g_note = L"Nothing to export yet: put a box on at least one image."; UpdateUi(); return; }
        if (MessageBoxW(g_wnd, L"Everything that has a box is already exported and unchanged.\n\nExport all of them again anyway?", L"PhotoCrop", MB_YESNO | MB_ICONQUESTION) == IDYES) StartExport(true, true);
        return;
    }
    int none = 0;
    for (auto& i : g_items) none += !i.sel.has;
    bool withGuess = false;
    if (c.guessPending) {
        std::wstring m = std::to_wstring(c.guessPending) + L" boxes are only guesses (▲) that you have not confirmed";
        if (none) m += L", and " + std::to_wstring(none) + L" images have no box at all";
        m += L".\n\nYes = export the guesses too\nNo = export only the auto-detected and your own boxes\nCancel = go back and review";
        int r = MessageBoxW(g_wnd, m.c_str(), L"PhotoCrop", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (r == IDCANCEL) return;
        withGuess = r == IDYES;
    }
    StartExport(withGuess, false);
}

// "Auto-crop all": detect and export everything in one go (guesses included), no review step.
void AutoCrop() {
    if (!EnsureDst()) { UpdateUi(); return; }
    if (MessageBoxW(g_wnd, L"Auto-crop finds the photo on every image and exports them right away, without waiting for your review. Boxes the program is unsure about are exported too, and on forms with light photo backgrounds that can be many (about 6 in 10 on a test of 600 forms).\n\nThe safer way for a first batch is \"Auto-select all\", then review.\n\nBoxes you set yourself are kept as they are. You can still fix single images afterwards and re-export just those.\n\nContinue?", L"PhotoCrop", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    g_autoExport = true;
    g_autoCropRun = true;
    StartDetect(true);
}

void SaveOne() {
    if (!g_img || !g_sel.has || !EnsureDst()) return;
    Commit();
    std::wstring name = g_items[g_idx].path.filename().wstring();
    if (!SaveCrop(g_items[g_idx].path, g_items[g_idx].sel, g_dst)) {
        g_items[g_idx].failed = true;
        UpdateListItem(g_idx);
        MessageBoxW(g_wnd, L"Could not crop/save this image.", L"PhotoCrop", MB_ICONERROR);
        return;
    }
    g_items[g_idx].saved = true;
    g_items[g_idx].failed = false;
    UpdateListItem(g_idx);
    SaveState();
    int n = FindCheck(g_idx);
    if (n >= 0) Go(n); else if (g_idx + 1 < (int)g_items.size()) Go(g_idx + 1);
    g_note = L"Exported " + name; UpdateUi();
}

void ExportSelected() {
    std::vector<int> sel = SelectedItems();
    if (sel.size() < 2) { SaveOne(); return; }
    if (!EnsureDst()) { UpdateUi(); return; }
    std::vector<char> todo(g_items.size(), 0);
    int n = 0;
    for (int i : sel) if (g_items[i].sel.has) { todo[i] = 1; n++; }
    if (!n) { g_note = L"None of the selected images has a box yet. Draw one and use Copy box to selected."; UpdateUi(); return; }
    g_selectedExport = true;
    g_selectedSkipped = (int)sel.size() - n;
    StartExportTodo(todo);
}

// "Copy box to ...": to the selected images when several are selected, otherwise to every image that still needs a check.
void ApplyBox() {
    if (!g_sel.has) return;
    std::vector<int> sel = SelectedItems(), targets;
    if (sel.size() >= 2) targets = sel;
    else for (size_t i = 0; i < g_items.size(); i++) if (NeedsCheck(g_items[i])) targets.push_back((int)i);
    for (int i : targets) { Item& it = g_items[i]; it.sel = g_sel; it.src = S_USER; it.saved = false; it.failed = false; }
    g_last = g_sel; g_unconf = false;
    FillList();
    SaveState();
    g_note = L"Box copied to " + std::to_wstring(targets.size()) + L" images; they now wait to be exported. Look through them and fix any that are off, then click Done & Export.";
    UpdateUi();
}

void JobDone() {
    if (g_ctrl.joinable()) g_ctrl.join();
    KillTimer(g_wnd, 1);
    g_busy = false;
    SetWindowTextW(g_wnd, L"PhotoCrop - crop applicant photos from scanned forms");
    Job& j = g_job;
    std::wstring msg;
    if (g_kind == 1) {
        int good = 0, guess = 0, none = 0, merged = 0;
        for (size_t i = 0; i < g_items.size(); i++) {
            if (!j.todo[i] || !j.done[i]) continue;
            merged++;
            Item& it = g_items[i];
            const Sel& o = it.sel; const Sel& nw = j.sel[i];
            bool same = o.has && nw.has && fabs(o.x0 - nw.x0) < 1e-4 && fabs(o.y0 - nw.y0) < 1e-4 && fabs(o.x1 - nw.x1) < 1e-4 && fabs(o.y1 - nw.y1) < 1e-4;
            it.sel = nw; it.src = j.src[i];
            if (!same) it.saved = false;  // a different box means the exported file is out of date
            good += j.src[i] == S_AUTO; guess += j.src[i] == S_GUESS; none += j.src[i] == S_NONE;
        }
        FillList();
        if (g_autoExport) {  // Auto-crop: no review, export straight away
            g_autoExport = false;
            if (!g_cancel) { SaveState(); StartExport(true, false); return; }
            g_autoCropRun = false;  // cancelled during detection: no export follows
        }
        if (g_cancel) msg = merged ? L"Cancelled. " + std::to_wstring(merged) + L" images were processed and kept." : L"Cancelled. Nothing was changed.";
        else {
            msg = L"Auto-detect finished.\n\n" + std::to_wstring(good) + L" found with a normal-sized box\n" + std::to_wstring(guess) + L" guessed (please check)\n" + std::to_wstring(none) + L" not found (please draw the box)\n\nNow review the images marked ▲: the app jumps to the first one.";
            int n = FindCheck(-1);
            if (n >= 0) Go(n); else LoadCurrent();
        }
        if (g_cancel) LoadCurrent();
    } else {
        int ok = 0, fail = 0, upToDate = 0, unconfirmed = 0, noBox = 0;
        std::wstring bad;
        for (size_t i = 0; i < g_items.size(); i++) {
            if (!j.todo[i]) {
                if (!g_items[i].sel.has) noBox++;
                else if (g_items[i].saved) upToDate++;
                else unconfirmed++;
                continue;
            }
            if (j.done[i]) { g_items[i].saved = true; g_items[i].failed = false; ok++; }
            else { g_items[i].failed = true; if (fail++ < 12) bad += L"\n" + g_items[i].path.filename().wstring(); }
        }
        FillList();
        Counts c = Count();
        if (g_autoCropRun && !g_cancel) {
            msg = L"Auto-crop finished: " + std::to_wstring(ok) + L" photos exported to:\n" + g_dst.wstring() +
                  L"\n\nWhat still needs you (use the tabs above the file list):\n  ▲  " + std::to_wstring(c.review) + L" need a better crop (exported from a guess)\n  ✕  " +
                  std::to_wstring(c.missing) + L" are not completed (no photo found, or the export failed)\n\nPick a tab, select one or many files (Ctrl or Shift + click), fix the box, then click Update. Only those files are rewritten.";
            if (fail) msg += L"\n\nExport failed for: " + std::to_wstring(fail) + bad;
            g_filter = c.missing ? 3 : c.review ? 2 : 0;  // open the tab that needs attention first
            FillList();
            if (!g_view.empty()) Go(g_view[0]);
        } else if (g_selectedExport && !g_cancel) {
            msg = L"Updated " + std::to_wstring(ok) + L" files in:\n" + g_dst.wstring();
            if (g_selectedSkipped) msg += L"\n\nLeft out (selected, but no photo box yet ✕): " + std::to_wstring(g_selectedSkipped);
            if (fail) msg += L"\n\nFailed: " + std::to_wstring(fail) + bad;
        } else {
            msg = L"Exported " + std::to_wstring(ok) + L" cropped photos to:\n" + g_dst.wstring();
            if (upToDate) msg += L"\n\nAlready up to date, not touched: " + std::to_wstring(upToDate);
            if (unconfirmed) msg += L"\n\nNot exported (unconfirmed guesses): " + std::to_wstring(unconfirmed);
            if (noBox) msg += L"\n\nNot completed (✕), no photo box yet: " + std::to_wstring(noBox);
            if (fail) msg += L"\n\nFailed: " + std::to_wstring(fail) + bad;
        }
        if (g_cancel) msg = L"Cancelled after exporting " + std::to_wstring(ok) + L" photos.";
        g_note = L"Exported " + std::to_wstring(ok) + L" photos to " + g_dst.wstring();
        g_autoCropRun = g_selectedExport = false;
    }
    SaveState();
    g_cancel = false;
    UpdateUi();
    MessageBoxW(g_wnd, msg.c_str(), L"PhotoCrop", MB_ICONINFORMATION);
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE: {
        g_wnd = h;
        auto mk = [&](const wchar_t* cls, const wchar_t* txt, int id, DWORD style = 0) {
            HWND c = CreateWindowW(cls, txt, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, h, (HMENU)(INT_PTR)id, nullptr, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
        };
        auto btn = [&](const wchar_t* txt, int id) {  // owner-drawn flat button with hover tracking
            mk(L"BUTTON", txt, id, BS_OWNERDRAW | WS_TABSTOP);
            SendMessageW(Ctl(id), WM_SETFONT, (WPARAM)g_fontBtn, TRUE);
            SetWindowSubclass(Ctl(id), BtnProc, id, 0);
        };
        btn(L"Source folder...", ID_SRC);
        btn(L"Destination folder...", ID_DST);
        btn(L"Auto-select all", ID_DETECT);
        btn(L"Auto-crop all", ID_AUTOCROP);
        btn(L"Copy box to unchecked", ID_APPLY);
        btn(L"‹  Prev", ID_PREV);
        btn(L"Next  ›", ID_NEXT);
        btn(L"Confirm", ID_CONFIRM);
        btn(L"Next to check", ID_NEXTCHK);
        btn(L"Free shape", ID_SHAPE);
        btn(L"Update this one", ID_SAVEONE);
        btn(L"Done & Export", ID_SAVEALL);
        btn(L"All", ID_FALL);  // filter tabs: the text (with counts) is set in UpdateUi
        btn(L"Done", ID_FDONE);
        btn(L"Review", ID_FREVIEW);
        btn(L"Missing", ID_FMISSING);
        mk(L"LISTBOX", L"", ID_LIST, WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_NODATA | LBS_EXTENDEDSEL);
        SetWindowTheme(Ctl(ID_LIST), L"Explorer", nullptr);  // slim modern scrollbar
        SetWindowSubclass(Ctl(ID_LIST), ListProc, 0, 0);
        btn(L"Cancel", ID_CANCEL);
        Layout();
        UpdateUi();
        return 0;
    }
    case WM_DPICHANGED: {  // moved to a screen with a different zoom: rebuild fonts and sizes
        g_dpi = HIWORD(w);
        HFONT old[4] = {g_font, g_fontBig, g_fontBtn, g_fontB};
        MakeFonts();
        for (int id = ID_SRC; id <= ID_SHAPE; id++)
            if (HWND c = Ctl(id)) SendMessageW(c, WM_SETFONT, (WPARAM)(id == ID_LIST ? g_font : g_fontBtn), TRUE);
        for (HFONT f : old) DeleteObject(f);
        SendMessageW(Ctl(ID_LIST), LB_SETITEMHEIGHT, 0, S(32));
        const RECT* r = (const RECT*)l;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        g_disp.reset();
        Layout();
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_GETMINMAXINFO:
        { RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);  // never demand more than the screen has
          ((MINMAXINFO*)l)->ptMinTrackSize = {std::min((int)S(1080), (int)(wa.right - wa.left)), std::min((int)S(560), (int)(wa.bottom - wa.top))}; }
        return 0;
    case WM_SIZE:
        if (Ctl(ID_LIST)) { Layout(); InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_MEASUREITEM:
        if (((MEASUREITEMSTRUCT*)l)->CtlID == ID_LIST) { ((MEASUREITEMSTRUCT*)l)->itemHeight = S(32); return TRUE; }
        break;
    case WM_DRAWITEM:
        if (w == ID_LIST) DrawListItem((DRAWITEMSTRUCT*)l); else DrawButton((DRAWITEMSTRUCT*)l);
        return TRUE;
    case WM_TIMER:
        { RECT c; GetClientRect(h, &c); RECT r{0, c.bottom - STATUSH(), c.right, c.bottom}; InvalidateRect(h, &r, FALSE); RECT cv = CanvasRect(); InvalidateRect(h, &cv, FALSE); }
        SetWindowTextW(h, (L"PhotoCrop - " + std::wstring(g_kind == 1 ? L"detecting " : L"saving ") + std::to_wstring(ProgressPct()) + L"%").c_str());
        return 0;
    case WM_JOBDONE: JobDone(); return 0;
    case WM_CLOSE:
        if (g_busy) { g_cancel = true; if (g_ctrl.joinable()) g_ctrl.join(); g_busy = false; }
        SaveState();
        DestroyWindow(h);
        return 0;
    case WM_COMMAND:
        if (g_busy && LOWORD(w) != ID_CANCEL) return 0;  // nothing but Cancel while a job runs
        switch (LOWORD(w)) {
        case ID_SRC: { fs::path p; if (PickFolder(h, p, L"Choose the folder that contains the scanned forms")) SetSource(p); break; }
        case ID_DST: { fs::path p; if (PickFolder(h, p, L"Choose the folder where the cropped photos will be saved")) {
            std::error_code ec;
            bool same = !g_dst.empty() && fs::equivalent(p, g_dst, ec);  // picking the same folder again changes nothing
            g_dst = p;
            if (!same) { for (auto& i : g_items) i.saved = false; FillList(); }  // exported marks belong to the old destination
            SaveSettings(); SaveState(); UpdateUi();
        } break; }
        case ID_LIST:
            if (HIWORD(w) == LBN_SELCHANGE && !g_busy) {  // the clicked row (the caret) is shown; Ctrl/Shift+click can select many
                int row = (int)SendMessageW(Ctl(ID_LIST), LB_GETCARETINDEX, 0, 0);
                int item = row >= 0 && row < (int)g_view.size() ? g_view[row] : -1;
                if (item >= 0 && item != g_idx) { g_idx = item; LoadCurrent(false); } else UpdateUi();
            }
            break;  // focus goes back to the window below, so Enter / Esc / Left / Right work right after a click
        case ID_FALL: case ID_FDONE: case ID_FREVIEW: case ID_FMISSING:  // filter tabs
            g_filter = LOWORD(w) - ID_FALL;
            FillList();
            if (RowOf(g_idx) < 0 && !g_view.empty()) Go(g_view[0]); else UpdateUi();
            break;
        case ID_PREV: StepView(-1); break;
        case ID_NEXT: StepView(1); break;
        case ID_DETECT: StartDetect(); break;
        case ID_AUTOCROP: AutoCrop(); break;
        case ID_APPLY: ApplyBox(); break;
        case ID_CONFIRM: ConfirmNext(); break;
        case ID_NEXTCHK: { int n = FindCheck(g_idx); if (n >= 0) Go(n); break; }
        case ID_SAVEONE: ExportSelected(); break;  // one image: SaveOne, several selected: they are all re-exported
        case ID_SAVEALL: ExportClicked(); break;
        case ID_SHAPE: ToggleShape(); break;
        case ID_CANCEL: g_cancel = true; UpdateUi(); break;
        }
        if (LOWORD(w) != ID_LIST || HIWORD(w) == LBN_SELCHANGE) SetFocus(h);  // so arrow keys / Enter work on the window (a Tab onto the list keeps its focus)
        return 0;
    case WM_KEYDOWN:
        if (g_busy) return 0;
        if (w == VK_LEFT || w == VK_UP) StepView(-1);
        else if (w == VK_RIGHT || w == VK_DOWN) StepView(1);
        else if (w == VK_RETURN) { if (IsWindowEnabled(Ctl(ID_CONFIRM))) ConfirmNext(); }
        else if (w == VK_ESCAPE) LoadCurrent();  // drop unsaved edits of this image
        else if (w == 'Q') ToggleShape();
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT && !g_busy) {
            POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
            RECT r = ImageRect();
            int mode = g_mode, mask = g_mask;
            if (!mode && g_img) {
                RECT grow = r; InflateRect(&grow, S(8), S(8));
                if (PtInRect(&grow, p)) {
                    mode = HitTest(r, p.x, p.y, mask);
                    if (mode == M_NEW && !PtInRect(&r, p)) mode = M_NONE;
                }
            }
            SetCursor(LoadCursor(nullptr, mode ? CursorFor(mode, mask) : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN: {
        SetFocus(h);
        if (g_busy) return 0;
        RECT r = ImageRect();
        POINT pt{(short)LOWORD(l), (short)HIWORD(l)};
        RECT grow = r; InflateRect(&grow, S(8), S(8));
        if (g_img && PtInRect(&grow, pt)) {
            int mask, mode = HitTest(r, pt.x, pt.y, mask);
            if (mode == M_NEW && !PtInRect(&r, pt)) return 0;
            g_mode = mode; g_mask = mask; g_orig = g_sel; g_changed = false;
            MouseToFrac(r, pt.x, pt.y, g_ax, g_ay);
            if (mode == M_NEW) g_sel = {g_ax, g_ay, g_ax, g_ay, false};
            g_note.clear();
            SetCapture(h);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_mode) {
            RECT r = ImageRect();
            double fx, fy, e = 0.005;
            MouseToFrac(r, (short)LOWORD(l), (short)HIWORD(l), fx, fy);
            g_changed = true;
            if (g_mode == M_NEW) g_sel = {std::min(g_ax, fx), std::min(g_ay, fy), std::max(g_ax, fx), std::max(g_ay, fy), true};
            else if (g_mode == M_CORNER) {  // one corner of the free shape follows the mouse
                g_sel = g_orig;
                g_sel.qx[g_mask] = fx; g_sel.qy[g_mask] = fy;
                QuadBounds(g_sel);
            } else if (g_mode == M_MOVE) {
                double dx = std::clamp(fx - g_ax, -g_orig.x0, 1 - g_orig.x1), dy = std::clamp(fy - g_ay, -g_orig.y0, 1 - g_orig.y1);
                g_sel = {g_orig.x0 + dx, g_orig.y0 + dy, g_orig.x1 + dx, g_orig.y1 + dy, true};
                if (g_orig.quad) { g_sel = g_orig; for (int i = 0; i < 4; i++) { g_sel.qx[i] += dx; g_sel.qy[i] += dy; } QuadBounds(g_sel); }
            } else {
                g_sel = g_orig;
                if (g_mask & 1) g_sel.x0 = std::min(fx, g_orig.x1 - e);
                if (g_mask & 2) g_sel.x1 = std::max(fx, g_orig.x0 + e);
                if (g_mask & 4) g_sel.y0 = std::min(fy, g_orig.y1 - e);
                if (g_mask & 8) g_sel.y1 = std::max(fy, g_orig.y0 + e);
            }
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_mode) {
            g_mode = M_NONE;
            ReleaseCapture();
            if (g_sel.x1 - g_sel.x0 < 0.005 || g_sel.y1 - g_sel.y0 < 0.005) { g_sel = g_orig; g_changed = false; }
            if (g_changed) Commit();  // an edit by the user confirms the box
            UpdateUi();
        }
        return 0;
    case WM_ERASEBKGND: {
        // the window is double-buffered in Paint; only themed children ask us to paint header colour
        RECT rc; GetClipBox((HDC)w, &rc);
        if (rc.bottom <= HDR()) Fill((HDC)w, rc, cBg);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        Paint(dc, ps.rcPaint);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show) {
    // Per-monitor DPI awareness (Windows 10 1703+), so the window stays sharp when moved between screens;
    // older Windows falls back to system DPI awareness.
    typedef BOOL(WINAPI * SetDpiCtxFn)(HANDLE);
    auto setCtx = (SetDpiCtxFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    if (!setCtx || !setCtx((HANDLE)-4)) SetProcessDPIAware();  // -4 = DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    HDC sdc = GetDC(nullptr);
    g_dpi = GetDeviceCaps(sdc, LOGPIXELSX);
    ReleaseDC(nullptr, sdc);
    MakeFonts();
    OleInitialize(nullptr);
    GdiplusStartupInput gi;
    ULONG_PTR tok;
    GdiplusStartup(&tok, &gi, nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hbrBackground = nullptr;
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));  // embedded from photocrop.ico (see photocrop.rc)
    wc.lpszClassName = L"PhotoCrop";
    RegisterClassW(&wc);
    RECT wa;  // fit the window inside the usable screen area (above the taskbar)
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int ww = std::min((int)S(1300), (int)(wa.right - wa.left)), wh = std::min((int)S(860), (int)(wa.bottom - wa.top));
    HWND h = CreateWindowW(L"PhotoCrop", L"PhotoCrop - crop applicant photos from scanned forms", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_VISIBLE,
                           wa.left + (wa.right - wa.left - ww) / 2, wa.top + (wa.bottom - wa.top - wh) / 2, ww, wh, nullptr, nullptr, hi, nullptr);
    ShowWindow(h, show);
    // optional: PhotoCrop.exe <source folder> [destination folder]; otherwise restore the last session
    std::error_code ec;
    if (__argc > 1) {
        if (fs::is_directory(__wargv[1], ec)) { if (__argc > 2 && fs::is_directory(__wargv[2], ec)) g_dst = __wargv[2]; SetSource(__wargv[1]); }
    } else {
        fs::path a = ReadSetting(L"src"), b = ReadSetting(L"dst");
        if (!b.empty() && fs::is_directory(b, ec)) g_dst = b;
        if (!a.empty() && fs::is_directory(a, ec)) SetSource(a);
        UpdateUi();
    }
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        if (m.message == WM_KEYDOWN && m.wParam == VK_TAB && IsWindowEnabled(h)) {  // Tab / Shift+Tab between buttons and the list
            HWND cur = GetFocus(), next = GetNextDlgTabItem(h, cur == h ? nullptr : cur, GetKeyState(VK_SHIFT) < 0);
            if (next) {
                SendMessageW(h, WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);  // show the focus ring
                SetFocus(next);
                continue;
            }
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    g_img.reset();  // bitmaps must be freed before GdiplusShutdown
    g_disp.reset();
    GdiplusShutdown(tok);
    OleUninitialize();
    return 0;
}

