// Anti-aliased shapes (GDI+) for the owner-drawn controls: plain GDI RoundRect/LineTo have jagged edges.
#include <windows.h>
#include <objidl.h>
#include <algorithm>
using std::max;  // GDI+ headers call min/max unqualified (NOMINMAX is set)
using std::min;
#pragma warning(push)
#pragma warning(disable : 4458)  // GDI+ headers shadow their own members
#include <gdiplus.h>
#pragma warning(pop)

#include <algorithm>

#include "ui/UiCommon.h"

namespace ss {
namespace {
using namespace Gdiplus;

// True when GDI+ started. If it did not (low memory, blocked by policy) every shape falls back to plain GDI, so controls stay visible.
bool EnsureGdiplus() {
    static const bool ok = [] {
        ULONG_PTR t = 0;
        GdiplusStartupInput in;
        return GdiplusStartup(&t, &in, nullptr) == Ok;  // shut down by process exit
    }();
    return ok;
}

// Plain-GDI versions used only when GDI+ is unavailable (jagged edges, but everything is drawn).
void GdiRoundRect(HDC dc, const RECT& rc, int radius, COLORREF fill, bool hasFill, COLORREF line, float width) {
    HBRUSH b = hasFill ? CreateSolidBrush(fill) : static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    HPEN p = line == kNoColor ? static_cast<HPEN>(GetStockObject(NULL_PEN)) : CreatePen(PS_SOLID, std::max(1, static_cast<int>(width + 0.5f)), line);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius * 2, radius * 2);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    if (hasFill) DeleteObject(b);
    if (line != kNoColor) DeleteObject(p);
}

void GdiPolyline(HDC dc, const POINT* pts, int n, COLORREF color, float width) {
    HPEN p = CreatePen(PS_SOLID, std::max(1, static_cast<int>(width + 0.5f)), color);
    HGDIOBJ op = SelectObject(dc, p);
    Polyline(dc, pts, n);
    SelectObject(dc, op);
    DeleteObject(p);
}

Color Col(COLORREF c, BYTE a = 255) { return Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }

void RoundPath(GraphicsPath& p, float x, float y, float w, float h, float r) {
    r = std::min({r, w / 2, h / 2});
    if (r < 0.5f) {
        p.AddRectangle(RectF(x, y, w, h));
        return;
    }
    float d = r * 2;
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
}
}  // namespace

void FillRoundRectAA(HDC dc, const RECT& rc, int radius, COLORREF fill, COLORREF border, float borderW) {
    if (!EnsureGdiplus()) return GdiRoundRect(dc, rc, radius, fill, true, border, borderW);
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    float inset = border == kNoColor ? 0.f : borderW / 2;  // the stroke is centred on the path: keep it inside rc
    GraphicsPath p;
    RoundPath(p, rc.left + inset, rc.top + inset, static_cast<float>(rc.right - rc.left) - 2 * inset,
              static_cast<float>(rc.bottom - rc.top) - 2 * inset, static_cast<float>(radius) - inset);
    SolidBrush b(Col(fill));
    g.FillPath(&b, &p);
    if (border != kNoColor) {
        Pen pen(Col(border), borderW);
        g.DrawPath(&pen, &p);
    }
}

void StrokeRoundRectAA(HDC dc, const RECT& rc, int radius, COLORREF color, float width) {
    if (!EnsureGdiplus()) return GdiRoundRect(dc, rc, radius, 0, false, color, width);
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    float inset = width / 2;
    GraphicsPath p;
    RoundPath(p, rc.left + inset, rc.top + inset, static_cast<float>(rc.right - rc.left) - 2 * inset,
              static_cast<float>(rc.bottom - rc.top) - 2 * inset, static_cast<float>(radius) - inset);
    Pen pen(Col(color), width);
    g.DrawPath(&pen, &p);
}

void DrawCheckMarkAA(HDC dc, const RECT& box, COLORREF color) {
    float w = static_cast<float>(box.right - box.left), h = static_cast<float>(box.bottom - box.top);
    if (!EnsureGdiplus()) {
        POINT pts[3] = {{static_cast<LONG>(box.left + w * 0.24f), static_cast<LONG>(box.top + h * 0.52f)},
                        {static_cast<LONG>(box.left + w * 0.42f), static_cast<LONG>(box.top + h * 0.70f)},
                        {static_cast<LONG>(box.left + w * 0.76f), static_cast<LONG>(box.top + h * 0.30f)}};
        return GdiPolyline(dc, pts, 3, color, std::max(2.f, w / 7.f));
    }
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    Pen pen(Col(color), std::max(1.8f, w / 7.f));
    pen.SetLineJoin(LineJoinRound);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    PointF pts[3] = {{box.left + w * 0.24f, box.top + h * 0.52f}, {box.left + w * 0.42f, box.top + h * 0.70f}, {box.left + w * 0.76f, box.top + h * 0.30f}};
    g.DrawLines(&pen, pts, 3);
}

void DrawChevronAA(HDC dc, int cx, int cy, int half, COLORREF color, float width) {
    if (!EnsureGdiplus()) {
        POINT pts[3] = {{cx - half, cy - half / 2}, {cx, cy + half / 2}, {cx + half, cy - half / 2}};
        return GdiPolyline(dc, pts, 3, color, width);
    }
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    Pen pen(Col(color), width);
    pen.SetLineJoin(LineJoinRound);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    PointF pts[3] = {{static_cast<float>(cx - half), cy - half / 2.f}, {static_cast<float>(cx), cy + half / 2.f}, {static_cast<float>(cx + half), cy - half / 2.f}};
    g.DrawLines(&pen, pts, 3);
}

}  // namespace ss
