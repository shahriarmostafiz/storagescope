#pragma once
#include <windows.h>

#include "scanner/FileTypes.h"

namespace ss {
constexpr COLORREF kNoColor = 0xFFFFFFFF;  // "no border" for the AA helpers

// One colour per file category, shared by the Dashboard bar and the treemap (readable on light and dark backgrounds).
COLORREF CategoryColor(FileCategory c);

// Custom messages posted from worker threads to the UI thread (never touch controls from workers).
enum : UINT {
    WM_APP_SCAN_PROGRESS = WM_APP + 1,  // throttled batch update (~10/s), data read from shared state
    WM_APP_SCAN_COMPLETE,
    WM_APP_DELETE_DONE,
    WM_APP_TEMP_SCAN_DONE,
    WM_APP_TEMP_CLEAN_DONE,
    WM_APP_SNAPSHOT_LOADED,  // the saved scan finished loading on its worker thread
    WM_APP_VALIDATED,        // the start-up check of the loaded scan found its changed folders
    WM_APP_SYNC_DONE,        // folders were re-listed on a worker thread; apply them to the tree
    WM_APP_INFO_THEME,       // sent to every SSInfo icon when the theme changes (tooltip colours)
    WM_APP_BIN_DONE,         // a Recycle Bin restore / delete / empty finished on its worker thread
};

// All colours come from here so a dark theme can be added later by swapping this table.
struct Theme {
    COLORREF bg, panel, text, muted, accent, accentSoft, danger, success, border, barBg;
    COLORREF hover, pressed, disabled, barFill, barFillSel, cardTop, cardBot, cardBorder, navTop, navBot;
    bool dark;
};
const Theme& Colors();
void SetDarkTheme(bool dark);  // rebuilds the table (for the current accent) and recreates the shared brushes
// Accent colour presets (Settings > Appearance). SetAccent only stores the choice; call SetDarkTheme afterwards to apply it.
void SetAccent(int idx);
int AccentIndex();
int AccentCount();
const wchar_t* AccentName(int i);
COLORREF AccentColor(int i, bool dark);
constexpr int kInfoIconSize = 22;  // dp; Make(L"SSInfo", tip, WS_TABSTOP, id): a round "i" that shows `tip` as a tooltip
constexpr int kAccentSwatchBase = 5000;  // control ids of the accent swatches (owner-drawn buttons) in Settings
bool IsDarkTheme();
HBRUSH BgBrush();
// Flat owner-drawn button (WM_DRAWITEM). "Scan..." = accent, "Delete"/"Permanent" = red text.
void DrawFlatButton(const DRAWITEMSTRUCT* d);
// Paints the check box (ticked or empty) of row `item` of a virtual list view (call from CDDS_ITEMPOSTPAINT).
void DrawListTick(HWND list, HDC dc, int item, bool on);
// Same box for lists that keep the native check box (Recycle Bin, Temporary Files): painted over it at the left edge of the row.
void DrawListTickNative(HWND list, HDC dc, int item, bool on);
// Row whose check box is under `ptClient` (list client coordinates), or -1.
int HitListTick(HWND list, POINT ptClient);
// NM_CUSTOMDRAW handler for list-view headers so column titles stay readable in dark mode (lParam = NMCUSTOMDRAW*).
LRESULT HeaderCustomDraw(LPARAM nmcd);
HBRUSH PanelBrush();
// Buttons laid out left to right, wrapping to a new row when the next one would not fit (narrow windows).
struct FlowItem {
    HWND h;
    int w;  // pixels
};
// Starts at x = margin, y = top. place=false only counts rows. show=false hides the buttons (they are still moved).
// `end` (optional) receives where the next button would go. Returns the number of rows used.
// Sizes the columns in proportion to `base` so they always fill `avail` pixels (no horizontal scrollbar).
void FitColumns(HWND list, const int* base, int n, int avail);
int FlowButtons(const FlowItem* items, int count, int margin, int width, int top, int btnH, int gap, bool place, bool show, POINT* end = nullptr);
// Anti-aliased shapes (Gfx.cpp). border = kNoColor for a fill only.
void FillRoundRectAA(HDC dc, const RECT& rc, int radius, COLORREF fill, COLORREF border = kNoColor, float borderW = 1.f);
void StrokeRoundRectAA(HDC dc, const RECT& rc, int radius, COLORREF color, float width);
void DrawCheckMarkAA(HDC dc, const RECT& box, COLORREF color);
void DrawChevronAA(HDC dc, int cx, int cy, int half, COLORREF color, float width);

}  // namespace ss
