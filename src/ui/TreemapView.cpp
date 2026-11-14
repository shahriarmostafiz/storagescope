#include "ui/TreemapView.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cwctype>

#include "operations/ExplorerService.h"
#include "ui/MainWindow.h"
#include "ui/UiCommon.h"
#include "utils/FormatUtils.h"

namespace ss {
namespace {
enum { ID_UP = 1, ID_OPEN, ID_BASKET, ID_PATH, ID_INFO, ID_INFOICON, ID_EMPTY, M_OPEN = 100, M_LOCATION, M_BASKET };
constexpr size_t kMaxTiles = 120;

COLORREF Blend(COLORREF a, COLORREF b, double t) {
    auto mix = [&](int x, int y) { return static_cast<int>(x + (y - x) * t); };
    return RGB(mix(GetRValue(a), GetRValue(b)), mix(GetGValue(a), GetGValue(b)), mix(GetBValue(a), GetBValue(b)));
}

COLORREF FolderColor() { return Colors().dark ? RGB(96, 146, 226) : RGB(46, 98, 180); }

// Every drive gets its own colour (by drive letter, so it stays the same between scans).
COLORREF DriveColor(const std::wstring& name) {
    static const COLORREF light[] = {RGB(46, 98, 180), RGB(0, 128, 118), RGB(200, 108, 10), RGB(128, 76, 190),
                                     RGB(196, 52, 112), RGB(52, 140, 52), RGB(176, 64, 44),  RGB(24, 130, 190)};
    static const COLORREF dark[] = {RGB(96, 146, 226), RGB(48, 190, 176), RGB(240, 152, 56), RGB(176, 130, 236),
                                    RGB(238, 106, 158), RGB(104, 200, 104), RGB(236, 118, 96), RGB(80, 180, 236)};
    int i = name.empty() ? 0 : (std::towupper(name[0]) - L'A') & 7;
    return (Colors().dark ? dark : light)[i];
}

COLORREF TileColor(const ScanNode* n) {
    if (n && n->type == NodeType::Directory && n->parent && n->parent->type == NodeType::Virtual) return DriveColor(n->name);
    if (!n) return Colors().dark ? RGB(70, 72, 78) : RGB(112, 116, 124);  // the "Other" remainder
    if (n->type == NodeType::File) return CategoryColor(Categorize(n->name));
    return FolderColor();
}

// Black or white text, whichever contrasts more with the box colour.
COLORREF TextOn(COLORREF c) {
    auto lin = [](int v) { return std::pow(v / 255.0, 2.2); };
    double L = 0.2126 * lin(GetRValue(c)) + 0.7152 * lin(GetGValue(c)) + 0.0722 * lin(GetBValue(c));
    return 1.05 / (L + 0.05) > (L + 0.05) / 0.05 ? RGB(255, 255, 255) : RGB(0, 0, 0);
}

// Segoe icon font glyph for a card (Fluent on Windows 11, MDL2 on Windows 10).
const wchar_t* Glyph(const ScanNode* n) {
    if (!n) return L"\uE712";
    if (n->type == NodeType::Directory) return L"\uE8B7";
    switch (Categorize(n->name)) {
        case FileCategory::Video: return L"\uE714";
        case FileCategory::Picture: return L"\uE91B";
        case FileCategory::Audio: return L"\uE8D6";
        case FileCategory::Archive: return L"\uE7B8";
        case FileCategory::DiskImage: return L"\uEDA2";
        case FileCategory::Installer: return L"\uE896";
        case FileCategory::Document: return L"\uE8A5";
        default: return L"\uE7C3";
    }
}

HFONT MakeIconFont(HDC dc, int px) {
    for (const wchar_t* face : {L"Segoe Fluent Icons", L"Segoe MDL2 Assets"}) {
        HFONT f = CreateFontW(-px, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
        HGDIOBJ o = SelectObject(dc, f);
        wchar_t got[64] = L"";
        GetTextFaceW(dc, 64, got);
        SelectObject(dc, o);
        if (_wcsicmp(got, face) == 0) return f;
        DeleteObject(f);
    }
    return nullptr;  // no icon font: the card shows its coloured square without a glyph
}
}  // namespace

TreemapView::~TreemapView() {
    if (iconFont_) DeleteObject(iconFont_);
}

void TreemapView::OnCreate() {
    up_ = Make(L"BUTTON", L"Up", WS_TABSTOP | BS_OWNERDRAW, ID_UP);
    bOpen_ = Make(L"BUTTON", L"Open in Analyzer", WS_TABSTOP | BS_OWNERDRAW, ID_OPEN);
    bBasket_ = Make(L"BUTTON", L"Add to Basket", WS_TABSTOP | BS_OWNERDRAW, ID_BASKET);
    path_ = Make(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE | SS_PATHELLIPSIS | SS_NOPREFIX, ID_PATH);
    info_ = Make(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE | SS_PATHELLIPSIS | SS_NOPREFIX, ID_INFO);
    infoIcon_ = Make(L"SSInfo", L"", WS_TABSTOP, ID_INFOICON);
    empty_ = Make(L"STATIC", L"", SS_CENTER | SS_NOPREFIX, ID_EMPTY);
}

void TreemapView::Layout(int w, int h) {
    int m = mw_->S(12), bh = mw_->S(32), band = mw_->S(46);
    MoveWindow(up_, m, mw_->S(7), mw_->S(64), bh, TRUE);
    MoveWindow(bOpen_, m + mw_->S(72), mw_->S(7), mw_->S(180), bh, TRUE);
    MoveWindow(bBasket_, m + mw_->S(260), mw_->S(7), mw_->S(160), bh, TRUE);
    MoveWindow(path_, m + mw_->S(432), mw_->S(7), std::max(mw_->S(100), w - 2 * m - mw_->S(432)), bh, TRUE);
    int ic = mw_->S(kInfoIconSize);
    MoveWindow(infoIcon_, m, h - mw_->S(33), ic, ic, TRUE);
    MoveWindow(info_, m + ic + mw_->S(8), h - mw_->S(36), w - 2 * m - ic - mw_->S(8), mw_->S(28), TRUE);
    area_ = {m, band + mw_->S(6), w - m, h - mw_->S(76)};
    int ew = std::min(w - 2 * m, mw_->S(520));
    MoveWindow(empty_, (w - ew) / 2, band + mw_->S(80), ew, mw_->S(48), TRUE);
    Rebuild();
}

void TreemapView::DropReferences() {
    if (cur_ && !cur_->removed) keepPath_ = BuildPath(cur_);
    cur_ = nullptr;
    tiles_.clear();
    hover_ = sel_ = -1;
    dirty_ = true;
}

void TreemapView::Rebuild() {
    dirty_ = false;
    tiles_.clear();
    hover_ = sel_ = -1;
    ScanResult* sc = mw_->scan();
    if (!sc) cur_ = nullptr;
    else if (!cur_ || cur_->removed) {
        cur_ = keepPath_.empty() ? nullptr : FindNodeByPath(sc->root.get(), keepPath_);
        if (!cur_) cur_ = sc->root.get();
    }
    keepPath_.clear();

    hiddenApp_ = 0;
    uint64_t shownApp = 0;  // app data that has its own view-only card here (the rest is only mentioned in the hint)
    bool any = false;
    if (cur_ && cur_->dir && area_.right > area_.left && area_.bottom > area_.top) {
        const Settings& s = mw_->settings();
        std::vector<Tile> all;
        for (auto& c : cur_->dir->children) {
            if (c.removed || c.size == 0 || (c.type != NodeType::File && c.type != NodeType::Directory)) continue;
            if (!s.showHidden && (c.attrs & FILE_ATTRIBUTE_HIDDEN)) continue;
            if (c.attrs & FILE_ATTRIBUTE_SYSTEM) continue;  // system files are never shown
            if (!s.showAppData && IsAppDataRoot(&c)) {  // AppData / ProgramData: a locked, view-only card (Settings > Scan unlocks it)
                all.push_back({{}, &c, c.size, 0, true});
                shownApp += c.size;
                continue;
            }
            all.push_back({{}, &c, c.size, 0});
        }
        if (!s.showAppData) {
            uint64_t total = AppDataBytes(cur_);
            hiddenApp_ = total > shownApp ? total - shownApp : 0;
        } else {
            hiddenApp_ = 0;
        }
        std::sort(all.begin(), all.end(), [](const Tile& a, const Tile& b) { return a.size > b.size; });
        if (all.size() > kMaxTiles) {  // the long tail becomes one "Other" box
            Tile other;
            other.count = all.size() - kMaxTiles;
            for (size_t i = kMaxTiles; i < all.size(); ++i) other.size += all[i].size;
            all.resize(kMaxTiles);
            all.push_back(other);
            std::sort(all.begin(), all.end(), [](const Tile& a, const Tile& b) { return a.size > b.size; });
        }
        if (!all.empty()) {
            // Cards flow left to right; a folder holding a fifth or more of the parent gets a double-width card.
            int gap = mw_->S(10), cardH = mw_->S(88), areaW = area_.right - area_.left;
            int cols = std::max(1, (areaW + gap) / (mw_->S(230) + gap));
            int cardW = (areaW - (cols - 1) * gap) / cols;
            double parent = cur_->size ? static_cast<double>(cur_->size) : 1.0;
            int cx = 0, row = 0;
            bottom_ = area_.top;
            for (auto& t : all) {
                // double width only for a big folder item, never for the drives on the "This PC" level (equal cards there)
                bool big = cur_->type != NodeType::Virtual && cols >= 3 && static_cast<double>(t.size) / parent >= 0.2;
                int span = big ? 2 : 1;
                if (cx + span > cols) {
                    cx = 0;
                    ++row;
                }
                int x = area_.left + cx * (cardW + gap), y = area_.top + row * (cardH + gap);
                t.r = {x, y, x + span * cardW + (span - 1) * gap, y + cardH};
                bottom_ = std::max(bottom_, static_cast<int>(t.r.bottom));
                cx += span;
                if (cx >= cols) {
                    cx = 0;
                    ++row;
                }
            }
            tiles_ = std::move(all);
            any = true;
        }
    }
    bool none = !any;
    ShowWindow(empty_, none ? SW_SHOW : SW_HIDE);
    if (none) SetWindowTextW(empty_, !sc ? mw_->EmptyStateText().c_str() : L"This folder is empty or has nothing to show.");
    SetWindowTextW(path_, cur_ ? (BuildPath(cur_).empty() ? L"This PC" : BuildPath(cur_)).c_str() : L"");
    if (!any) bottom_ = area_.top;
    UpdateButtons();
    UpdateInfo();
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateScroll();
}

void TreemapView::UpdateScroll() {
    if (inScroll_) return;  // SetScrollInfo can resize the client area and re-enter Layout
    inScroll_ = true;
    int page = area_.bottom - area_.top, total = bottom_ - area_.top;
    scroll_ = std::clamp(scroll_, 0, std::max(0, total - page));
    SCROLLINFO si{sizeof si, SIF_RANGE | SIF_PAGE | SIF_POS};
    si.nMax = std::max(total, 1) - 1;
    si.nPage = static_cast<UINT>(std::max(page, 1));
    si.nPos = scroll_;
    SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
    inScroll_ = false;
}

void TreemapView::ScrollTo(int y) {
    int most = std::max(0, static_cast<int>(bottom_ - area_.top - (area_.bottom - area_.top)));
    y = std::clamp(y, 0, most);
    if (y == scroll_) return;
    scroll_ = y;
    UpdateScroll();
    InvalidateRect(hwnd_, &area_, FALSE);
    POINT cp;  // another card may now be under the (still) mouse: refresh the hover highlight and the info line
    if (GetCursorPos(&cp) && ScreenToClient(hwnd_, &cp)) {
        int h = HitTest(cp);
        if (h != hover_) {
            hover_ = h;
            UpdateInfo();
        }
    }
}

void TreemapView::UpdateButtons() {
    EnableWindow(up_, cur_ && cur_->parent);
    EnableWindow(bOpen_, cur_ != nullptr);
    bool canBasket = sel_ >= 0 && tiles_[static_cast<size_t>(sel_)].n && !mw_->busy();
    EnableWindow(bBasket_, canBasket);
    ShowWindow(bBasket_, canBasket ? SW_SHOW : SW_HIDE);  // only offered once a folder is selected
}

int TreemapView::HitTest(POINT p) const {
    if (!PtInRect(&area_, p)) return -1;
    POINT q{p.x, p.y + scroll_};
    for (size_t i = 0; i < tiles_.size(); ++i)
        if (PtInRect(&tiles_[i].r, q)) return static_cast<int>(i);
    return -1;
}

void TreemapView::UpdateInfo() {
    int i = hover_ >= 0 ? hover_ : sel_;
    std::wstring t;
    if (i >= 0 && i < static_cast<int>(tiles_.size())) {
        const Tile& tl = tiles_[static_cast<size_t>(i)];
        uint64_t parent = cur_ ? cur_->size : 0;
        wchar_t pct[16];
        swprintf_s(pct, L"%.1f%%", parent ? 100.0 * static_cast<double>(tl.size) / static_cast<double>(parent) : 0.0);
        if (tl.locked) t = tl.n->name + L"   -   " + FormatBytes(tl.size) + L"   -   " + pct + L" of this folder   -   view only";
        else if (tl.n) t = tl.n->name + L"   -   " + (tl.n->type == NodeType::Directory ? L"Folder" : CategoryName(Categorize(tl.n->name))) + L"   -   " +
                      FormatBytes(tl.size) + L"   -   " + pct + L" of this folder";
        else t = L"Other: " + FormatCount(tl.count) + L" smaller items   -   " + FormatBytes(tl.size) + L"   -   " + pct + L" of this folder";
    } else if (hiddenApp_) {
        t = FormatBytes(hiddenApp_) + L" of app data hidden";
    }
    SetWindowTextW(info_, t.c_str());
    // The how-to lives in the "i" tooltip, not in a long line of text.
    bool anyApp = hiddenApp_ != 0;
    for (const Tile& tl : tiles_) anyApp = anyApp || tl.locked;
    std::wstring tip = L"Each card shows its size and share of this folder. Click a card to select it, double-click a folder to look inside.";
    if (anyApp) tip += L"\n\nApp data (AppData and ProgramData) is hidden by default and shown view only. Turn on \"Show app data\" in Settings > Scan to manage it.";
    SetWindowTextW(infoIcon_, tip.c_str());
}

void TreemapView::Enter(ScanNode* n) {
    if (!n || n->type != NodeType::Directory) return;
    cur_ = n;
    scroll_ = 0;
    Rebuild();
}

void TreemapView::ContextMenu(POINT screen) {
    POINT c = screen;
    ScreenToClient(hwnd_, &c);
    int i = HitTest(c);
    if (i < 0 || !tiles_[static_cast<size_t>(i)].n || tiles_[static_cast<size_t>(i)].locked) return;  // app data: view only
    sel_ = i;
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateButtons();
    ScanNode* n = tiles_[static_cast<size_t>(i)].n;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, M_OPEN, L"Open in Storage Analyzer");
    AppendMenuW(m, MF_STRING, M_LOCATION, L"Open File Location");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (mw_->busy() ? MF_GRAYED : 0), M_BASKET, L"Add to Basket");
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, hwnd_, nullptr);
    DestroyMenu(m);
    if (cmd == M_OPEN) mw_->BrowseTo(n->type == NodeType::Directory ? n : n->parent);
    else if (cmd == M_LOCATION) explorer::Reveal(BuildPath(n), n->type != NodeType::File);
    else if (cmd == M_BASKET) mw_->AddToBasket({n});
}

void TreemapView::Paint() {
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
    HPEN line = CreatePen(PS_SOLID, 1, c.border);
    HGDIOBJ ol = SelectObject(mem, line);
    MoveToEx(mem, 0, mw_->S(46) - 1, nullptr);
    LineTo(mem, rc.right, mw_->S(46) - 1);
    SelectObject(mem, ol);
    DeleteObject(line);

    uint64_t parentSize = cur_ ? cur_->size : 0;
    int px = mw_->S(20);
    if (!iconFont_ || iconPx_ != px) {
        if (iconFont_) DeleteObject(iconFont_);
        iconFont_ = MakeIconFont(mem, px);
        iconPx_ = px;
    }
    int saved = SaveDC(mem);
    IntersectClipRect(mem, area_.left, area_.top, area_.right, area_.bottom);
    for (size_t i = 0; i < tiles_.size(); ++i) {
        const Tile& t = tiles_[i];
        RECT r = t.r;
        OffsetRect(&r, 0, -scroll_);
        if (r.bottom < area_.top || r.top > area_.bottom) continue;
        bool hot = static_cast<int>(i) == hover_ && !t.locked, sel = static_cast<int>(i) == sel_;  // a locked card gives no "clickable" highlight
        COLORREF col = t.locked ? (c.dark ? RGB(110, 114, 122) : RGB(96, 100, 108)) : TileColor(t.n);  // app data: neutral grey
        COLORREF soft = Blend(col, c.panel, c.dark ? 0.80 : 0.88);  // card background: a light wash of the category colour
        COLORREF edge = sel ? c.text : hot ? col : Blend(col, c.panel, 0.55);
        RECT card = r;
        InflateRect(&card, -1, -1);
        FillRoundRectAA(mem, card, mw_->S(10), soft, edge, static_cast<float>(sel ? mw_->S(3) : hot ? mw_->S(2) : 1));
        int pad = mw_->S(12), isz = mw_->S(40);
        RECT ic{r.left + pad, r.top + pad, r.left + pad + isz, r.top + pad + isz};
        FillRoundRectAA(mem, ic, mw_->S(10), col);
        if (iconFont_) {
            HGDIOBJ of = SelectObject(mem, iconFont_);
            SetTextColor(mem, TextOn(col));
            DrawTextW(mem, t.locked ? L"" : Glyph(t.n), -1, &ic, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);  // E72E = lock
            SelectObject(mem, of);
        }
        RECT tr{ic.right + mw_->S(10), r.top + mw_->S(10), r.right - pad, r.top + mw_->S(32)};
        std::wstring label = t.locked ? t.n->name + L" (view only)" : t.n ? t.n->name : L"Other (" + FormatCount(t.count) + L" items)";
        SetTextColor(mem, c.text);
        SelectObject(mem, mw_->boldFont());
        DrawTextW(mem, label.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        double share = parentSize ? static_cast<double>(t.size) / static_cast<double>(parentSize) : 0.0;
        wchar_t pct[16];
        swprintf_s(pct, L"%.1f%%", 100.0 * share);
        std::wstring sz = FormatBytes(t.size) + L"  \u00B7  " + pct;
        tr.top += mw_->S(24);
        tr.bottom += mw_->S(24);
        SetTextColor(mem, c.muted);
        SelectObject(mem, mw_->font());
        DrawTextW(mem, sz.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        RECT bar{r.left + pad, r.bottom - mw_->S(17), r.right - pad, r.bottom - mw_->S(11)};  // share of the folder
        FillRoundRectAA(mem, bar, mw_->S(3), Blend(col, c.panel, 0.70));
        bar.right = bar.left + std::max<int>(mw_->S(6), static_cast<int>((bar.right - bar.left) * std::min(1.0, share)));
        FillRoundRectAA(mem, bar, mw_->S(3), col);
    }
    RestoreDC(mem, saved);
    // legend: what the colours mean (not at the "This PC" level, where each drive has its own colour)
    if (!(cur_ && cur_->type == NodeType::Virtual)) {
        SelectObject(mem, mw_->font());
        SetTextColor(mem, c.text);
        int x = area_.left, y = area_.bottom + mw_->S(10), sw = mw_->S(12);
        auto item = [&](COLORREF col, const wchar_t* name) {
            SIZE sz{};
            GetTextExtentPoint32W(mem, name, static_cast<int>(wcslen(name)), &sz);
            if (x + sw + mw_->S(6) + sz.cx > area_.right) return;
            RECT b{x, y + (sz.cy - sw) / 2, x + sw, y + (sz.cy - sw) / 2 + sw};
            FillRoundRectAA(mem, b, mw_->S(3), col);
            RECT tr{x + sw + mw_->S(6), y, x + sw + mw_->S(6) + sz.cx + 2, y + sz.cy};
            DrawTextW(mem, name, -1, &tr, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
            x += sw + mw_->S(6) + sz.cx + mw_->S(16);
        };
        item(FolderColor(), L"Folder");
        for (int k = 0; k < kCategoryCount; ++k) item(CategoryColor(static_cast<FileCategory>(k)), CategoryName(static_cast<FileCategory>(k)));
    }
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, ob);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd_, &ps);
}

LRESULT TreemapView::OnMessage(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: Paint(); return 0;
        case WM_MOUSEWHEEL: ScrollTo(scroll_ - GET_WHEEL_DELTA_WPARAM(w) * mw_->S(60) / WHEEL_DELTA); return 0;
        case WM_VSCROLL: {
            SCROLLINFO si{sizeof si, SIF_ALL};
            GetScrollInfo(hwnd_, SB_VERT, &si);
            int y = si.nPos;
            switch (LOWORD(w)) {
                case SB_LINEUP: y -= mw_->S(40); break;
                case SB_LINEDOWN: y += mw_->S(40); break;
                case SB_PAGEUP: y -= static_cast<int>(si.nPage); break;
                case SB_PAGEDOWN: y += static_cast<int>(si.nPage); break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: y = si.nTrackPos; break;
                case SB_TOP: y = 0; break;
                case SB_BOTTOM: y = bottom_; break;
            }
            ScrollTo(y);
            return 0;
        }
        case WM_MOUSEMOVE: {
            int i = HitTest({GET_X_LPARAM(l), GET_Y_LPARAM(l)});
            if (i != hover_) {
                hover_ = i;
                UpdateInfo();
                InvalidateRect(hwnd_, &area_, FALSE);
            }
            TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, hwnd_, 0};
            TrackMouseEvent(&t);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (hover_ != -1) {
                hover_ = -1;
                UpdateInfo();
                InvalidateRect(hwnd_, &area_, FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd_);
            sel_ = HitTest({GET_X_LPARAM(l), GET_Y_LPARAM(l)});
            if (sel_ >= 0 && tiles_[static_cast<size_t>(sel_)].locked) sel_ = -1;  // app data cannot be selected
            UpdateButtons();
            UpdateInfo();
            InvalidateRect(hwnd_, &area_, FALSE);
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            int i = HitTest({GET_X_LPARAM(l), GET_Y_LPARAM(l)});
            if (i >= 0 && !tiles_[static_cast<size_t>(i)].locked) Enter(tiles_[static_cast<size_t>(i)].n);
            return 0;
        }
        case WM_RBUTTONUP: {
            POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            ClientToScreen(hwnd_, &p);
            ContextMenu(p);
            return 0;
        }
        case WM_KEYDOWN:
            if (w == VK_BACK && cur_ && cur_->parent) {
                cur_ = cur_->parent;
                scroll_ = 0;
                Rebuild();
                return 0;
            }
            break;
        case WM_COMMAND:
            switch (LOWORD(w)) {
                case ID_UP:
                    if (cur_ && cur_->parent) {
                        cur_ = cur_->parent;
                        scroll_ = 0;
                        Rebuild();
                    }
                    break;
                case ID_OPEN:
                    if (cur_) mw_->BrowseTo(cur_);
                    break;
                case ID_BASKET:
                    if (sel_ >= 0 && tiles_[static_cast<size_t>(sel_)].n) mw_->AddToBasket({tiles_[static_cast<size_t>(sel_)].n});
                    break;
            }
            return 0;
    }
    return View::OnMessage(m, w, l);
}

}  // namespace ss
