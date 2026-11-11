#pragma once
#include <windows.h>

#include "scanner/FileTypes.h"

namespace ss {

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
    WM_APP_BIN_DONE,         // a Recycle Bin restore / delete / empty finished on its worker thread
};

// All colours come from here so a dark theme can be added later by swapping this table.
struct Theme {
    COLORREF bg, panel, text, muted, accent, accentSoft, danger, success, border, barBg;
    COLORREF hover, pressed, disabled, barFill, barFillSel, cardTop, cardBot, cardBorder, navTop, navBot;
    bool dark;
};
const Theme& Colors();
void SetDarkTheme(bool dark);  // swaps the table and recreates the shared brushes
bool IsDarkTheme();
HBRUSH BgBrush();
// Flat owner-drawn button (WM_DRAWITEM). "Scan..." = accent, "Delete"/"Permanent" = red text.
void DrawFlatButton(const DRAWITEMSTRUCT* d);
// Paints the check box (ticked or empty) of row `item` of a virtual list view (call from CDDS_ITEMPOSTPAINT).
void DrawListTick(HWND list, HDC dc, int item, bool on);
// Row whose check box is under `ptClient` (list client coordinates), or -1.
int HitListTick(HWND list, POINT ptClient);
// NM_CUSTOMDRAW handler for list-view headers so column titles stay readable in dark mode (lParam = NMCUSTOMDRAW*).
LRESULT HeaderCustomDraw(LPARAM nmcd);
HBRUSH PanelBrush();

}  // namespace ss
