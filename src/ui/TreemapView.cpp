#include "ui/TreemapView.h"

#include <windowsx.h>

#include <algorithm>

#include "operations/ExplorerService.h"
#include "ui/MainWindow.h"
#include "ui/UiCommon.h"
#include "utils/FormatUtils.h"

namespace ss {
namespace {
enum { ID_UP = 1, ID_OPEN, ID_BASKET, ID_PATH, ID_INFO, ID_EMPTY, M_OPEN = 100, M_LOCATION, M_BASKET };
constexpr size_t kMaxTiles = 120;

struct RectD {
    double x, y, w, h;
};

// Squarified treemap (Bruls et al.): `areas` sorted largest first and summing to r.w * r.h.
void Squarify(const std::vector<double>& areas, RectD r, std::vector<RectD>& out) {
    out.assign(areas.size(), {});
    size_t i = 0;
    while (i < areas.size() && r.w > 0.5 && r.h > 0.5) {
        double side = std::min(r.w, r.h), sum = 0, worst = 1e18;
        size_t j = i;
        while (j < areas.size()) {
            double ns = sum + areas[j];
            double ratio = std::max(side * side * areas[i] / (ns * ns), ns * ns / (side * side * std::max(areas[j], 1e-9)));
            if (j > i && ratio > worst) break;  // adding this item made the worst box less square: close the row
            sum = ns;
            worst = ratio;
            ++j;
        }
        double thick = sum / side;
        if (r.w >= r.h) {  // vertical strip on the left
            double y = r.y;
            for (size_t k = i; k < j; ++k) {
                double hh = areas[k] / thick;
                out[k] = {r.x, y, thick, hh};
                y += hh;
            }
            r.x += thick;
            r.w -= thick;
        } else {  // horizontal strip on top
            double x = r.x;
            for (size_t k = i; k < j; ++k) {
                double ww = areas[k] / thick;
                out[k] = {x, r.y, ww, thick};
                x += ww;
            }
            r.y += thick;
            r.h -= thick;
        }
        i = j;
    }
}

COLORREF Blend(COLORREF a, COLORREF b, double t) {
    auto mix = [&](int x, int y) { return static_cast<int>(x + (y - x) * t); };
    return RGB(mix(GetRValue(a), GetRValue(b)), mix(GetGValue(a), GetGValue(b)), mix(GetBValue(a), GetBValue(b)));
}

COLORREF TileColor(const ScanNode* n) {
    const Theme& t = Colors();
    if (!n) return RGB(120, 124, 132);
    if (n->type == NodeType::File) return CategoryColor(Categorize(n->name));
    size_t h = std::hash<std::wstring>{}(n->name);  // folders: accent in a few shades so neighbours differ
    return Blend(t.accent, t.dark ? RGB(0, 0, 0) : RGB(255, 255, 255), 0.05 + 0.1 * static_cast<double>(h % 5) / 1.0 * 0.6);
}

bool Light(COLORREF c) { return (299 * GetRValue(c) + 587 * GetGValue(c) + 114 * GetBValue(c)) / 1000 > 150; }
}  // namespace

void TreemapView::OnCreate() {
    up_ = Make(L"BUTTON", L"Up", WS_TABSTOP | BS_OWNERDRAW, ID_UP);
    bOpen_ = Make(L"BUTTON", L"Open in Analyzer", WS_TABSTOP | BS_OWNERDRAW, ID_OPEN);
    bBasket_ = Make(L"BUTTON", L"Add to Basket", WS_TABSTOP | BS_OWNERDRAW, ID_BASKET);
    path_ = Make(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE | SS_PATHELLIPSIS | SS_NOPREFIX, ID_PATH);
    info_ = Make(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE | SS_PATHELLIPSIS | SS_NOPREFIX, ID_INFO);
    empty_ = Make(L"STATIC", L"", SS_CENTER | SS_NOPREFIX, ID_EMPTY);
}

void TreemapView::Layout(int w, int h) {
    int m = mw_->S(12), bh = mw_->S(32), band = mw_->S(46);
    MoveWindow(up_, m, mw_->S(7), mw_->S(64), bh, TRUE);
    MoveWindow(bOpen_, m + mw_->S(72), mw_->S(7), mw_->S(180), bh, TRUE);
    MoveWindow(bBasket_, m + mw_->S(260), mw_->S(7), mw_->S(160), bh, TRUE);
    MoveWindow(path_, m + mw_->S(432), mw_->S(7), std::max(mw_->S(100), w - 2 * m - mw_->S(432)), bh, TRUE);
    MoveWindow(info_, m, h - mw_->S(36), w - 2 * m, mw_->S(28), TRUE);
    area_ = {m, band + mw_->S(6), w - m, h - mw_->S(44)};
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

    bool any = false;
    if (cur_ && cur_->dir && area_.right > area_.left && area_.bottom > area_.top) {
        const Settings& s = mw_->settings();
        std::vector<Tile> all;
        for (auto& c : cur_->dir->children) {
            if (c.removed || c.size == 0 || (c.type != NodeType::File && c.type != NodeType::Directory)) continue;
            if (!s.showHidden && (c.attrs & FILE_ATTRIBUTE_HIDDEN)) continue;
            if (c.attrs & FILE_ATTRIBUTE_SYSTEM) continue;  // system files are never shown
            all.push_back({{}, &c, c.size, 0});
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
            double total = 0;
            for (auto& t : all) total += static_cast<double>(t.size);
            RectD ar{static_cast<double>(area_.left), static_cast<double>(area_.top), static_cast<double>(area_.right - area_.left),
                     static_cast<double>(area_.bottom - area_.top)};
            std::vector<double> areas;
            for (auto& t : all) areas.push_back(ar.w * ar.h * static_cast<double>(t.size) / total);
            std::vector<RectD> rects;
            Squarify(areas, ar, rects);
            for (size_t i = 0; i < all.size(); ++i) {
                all[i].r = {static_cast<LONG>(rects[i].x + 0.5), static_cast<LONG>(rects[i].y + 0.5),
                            static_cast<LONG>(rects[i].x + rects[i].w + 0.5), static_cast<LONG>(rects[i].y + rects[i].h + 0.5)};
            }
            tiles_ = std::move(all);
            any = true;
        }
    }
    bool none = !any;
    ShowWindow(empty_, none ? SW_SHOW : SW_HIDE);
    if (none) SetWindowTextW(empty_, !sc ? mw_->EmptyStateText().c_str() : L"This folder is empty or has nothing to show.");
    SetWindowTextW(path_, cur_ ? (BuildPath(cur_).empty() ? L"This PC" : BuildPath(cur_)).c_str() : L"");
    UpdateButtons();
    UpdateInfo();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void TreemapView::UpdateButtons() {
    EnableWindow(up_, cur_ && cur_->parent);
    EnableWindow(bOpen_, cur_ != nullptr);
    EnableWindow(bBasket_, sel_ >= 0 && tiles_[static_cast<size_t>(sel_)].n && !mw_->busy());
}

int TreemapView::HitTest(POINT p) const {
    for (size_t i = 0; i < tiles_.size(); ++i)
        if (PtInRect(&tiles_[i].r, p)) return static_cast<int>(i);
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
        if (tl.n) t = tl.n->name + L"   -   " + (tl.n->type == NodeType::Directory ? L"Folder" : CategoryName(Categorize(tl.n->name))) + L"   -   " +
                      FormatBytes(tl.size) + L"   -   " + pct + L" of this folder";
        else t = L"Other: " + FormatCount(tl.count) + L" smaller items   -   " + FormatBytes(tl.size) + L"   -   " + pct + L" of this folder";
    } else if (!tiles_.empty()) {
        t = L"Hover a box for details. Click to select, double-click to open a folder.";
    }
    SetWindowTextW(info_, t.c_str());
}

void TreemapView::Enter(ScanNode* n) {
    if (!n || n->type != NodeType::Directory) return;
    cur_ = n;
    Rebuild();
}

void TreemapView::ContextMenu(POINT screen) {
    POINT c = screen;
    ScreenToClient(hwnd_, &c);
    int i = HitTest(c);
    if (i < 0 || !tiles_[static_cast<size_t>(i)].n) return;
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

    for (size_t i = 0; i < tiles_.size(); ++i) {
        const Tile& t = tiles_[i];
        RECT r = t.r;
        InflateRect(&r, -1, -1);  // 1px gap between boxes
        if (r.right <= r.left || r.bottom <= r.top) continue;
        COLORREF col = TileColor(t.n);
        HBRUSH b = CreateSolidBrush(col);
        FillRect(mem, &r, b);
        DeleteObject(b);
        bool hot = static_cast<int>(i) == hover_ || static_cast<int>(i) == sel_;
        if (hot) {
            HPEN pen = CreatePen(PS_SOLID, mw_->S(3), c.dark ? RGB(255, 255, 255) : RGB(0, 0, 0));
            HGDIOBJ op = SelectObject(mem, pen), obr = SelectObject(mem, GetStockObject(NULL_BRUSH));
            Rectangle(mem, r.left + 1, r.top + 1, r.right, r.bottom);
            SelectObject(mem, op);
            SelectObject(mem, obr);
            DeleteObject(pen);
        }
        int w = r.right - r.left, h = r.bottom - r.top;
        if (w > mw_->S(64) && h > mw_->S(30)) {  // label only where it fits
            SetTextColor(mem, Light(col) ? RGB(20, 20, 20) : RGB(255, 255, 255));
            std::wstring label = t.n ? t.n->name : L"Other (" + FormatCount(t.count) + L")";
            RECT tr{r.left + mw_->S(6), r.top + mw_->S(4), r.right - mw_->S(6), r.bottom - mw_->S(4)};
            SelectObject(mem, t.n && t.n->type == NodeType::Directory ? mw_->boldFont() : mw_->font());
            DrawTextW(mem, label.c_str(), -1, &tr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            if (h > mw_->S(54)) {
                tr.top += mw_->S(22);
                SelectObject(mem, mw_->font());
                std::wstring sz = FormatBytes(t.size);
                DrawTextW(mem, sz.c_str(), -1, &tr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
        }
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
            UpdateButtons();
            UpdateInfo();
            InvalidateRect(hwnd_, &area_, FALSE);
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            int i = HitTest({GET_X_LPARAM(l), GET_Y_LPARAM(l)});
            if (i >= 0) Enter(tiles_[static_cast<size_t>(i)].n);
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
                Rebuild();
                return 0;
            }
            break;
        case WM_COMMAND:
            switch (LOWORD(w)) {
                case ID_UP:
                    if (cur_ && cur_->parent) {
                        cur_ = cur_->parent;
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
