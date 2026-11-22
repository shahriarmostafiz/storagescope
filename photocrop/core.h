// PhotoCrop core: photo detection, background jobs, export selection and the remembered-state text format.
// No window code in here, so tests.cpp can use it directly.
#pragma once
#define NOMINMAX
#include <algorithm>
#include <windows.h>
using std::min;  // gdiplus headers call unqualified min/max
using std::max;
#include <gdiplus.h>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")

using namespace Gdiplus;
namespace fs = std::filesystem;

// ---------------------------------------------------------------- data
// A selection is stored as fractions of the image (0..1) so it fits scans of different sizes. It is a rectangle
// (x0..y1), or a free four-corner shape when quad is set: the corners go top-left, top-right, bottom-right,
// bottom-left in qx/qy, and the rectangle then holds the shape's bounding box.
struct Sel { double x0 = 0, y0 = 0, x1 = 0, y1 = 0; bool has = false; bool quad = false; double qx[4] = {0, 0, 0, 0}, qy[4] = {0, 0, 0, 0}; };

void QuadBounds(Sel& s) {  // the rectangle = bounding box of the four corners
    s.x0 = *std::min_element(s.qx, s.qx + 4); s.x1 = *std::max_element(s.qx, s.qx + 4);
    s.y0 = *std::min_element(s.qy, s.qy + 4); s.y1 = *std::max_element(s.qy, s.qy + 4);
}
void ToQuad(Sel& s) {  // turn the rectangle into a four-corner shape with the same corners
    s.quad = true;
    s.qx[0] = s.qx[3] = s.x0; s.qx[1] = s.qx[2] = s.x1;
    s.qy[0] = s.qy[1] = s.y0; s.qy[2] = s.qy[3] = s.y1;
}
void ToRect(Sel& s) { QuadBounds(s); s.quad = false; }  // back to the bounding rectangle (the tilt is dropped)
enum Src : char { S_NONE = 0, S_AUTO = 1, S_GUESS = 2, S_USER = 3 };  // where an image's box came from
struct Item { fs::path path; Sel sel; char src = S_NONE; bool saved = false; bool failed = false; };  // failed: the last export of this image failed
bool NeedsCheck(const Item& i) { return i.src == S_NONE || i.src == S_GUESS; }

// What the operator sees next to every file:
//   ST_DONE    exported with a confident or confirmed box
//   ST_REVIEW  only a guess (exported or not): it may need a better crop
//   ST_MISSING not completed: no photo box yet, or the last export failed
//   ST_WAITING has a box that is not exported yet (or changed since it was)
enum FileStatus { ST_DONE, ST_REVIEW, ST_MISSING, ST_WAITING };
FileStatus StatusOf(const Item& i) {
    if (!i.sel.has || i.failed) return ST_MISSING;
    if (i.src == S_GUESS) return ST_REVIEW;
    return i.saved ? ST_DONE : ST_WAITING;
}
// List filter tabs: 0 = All, 1 = Done, 2 = Needs a better crop, 3 = Not completed. Waiting images show under All only.
bool InFilter(const Item& i, int filter) {
    FileStatus s = StatusOf(i);
    return filter == 0 || (filter == 1 && s == ST_DONE) || (filter == 2 && s == ST_REVIEW) || (filter == 3 && s == ST_MISSING);
}

std::wstring Lower(std::wstring s) { for (auto& c : s) c = towlower(c); return s; }

bool IsImage(const fs::path& p) {
    auto e = Lower(p.extension().wstring());
    return e == L".jpg" || e == L".jpeg" || e == L".png" || e == L".bmp" || e == L".tif" || e == L".tiff" || e == L".gif";
}

bool GetEncoder(const wchar_t* mime, CLSID* clsid) {
    UINT n = 0, sz = 0;
    GetImageEncodersSize(&n, &sz);
    std::vector<char> buf(sz);
    auto* e = (ImageCodecInfo*)buf.data();
    GetImageEncoders(n, sz, e);
    for (UINT i = 0; i < n; i++)
        if (!wcscmp(e[i].MimeType, mime)) { *clsid = e[i].Clsid; return true; }
    return false;
}

// ---------------------------------------------------------------- free-shape (four-corner) crop
// Size that keeps the shape's real resolution: the average of opposite edge lengths, in source pixels.
void QuadSize(const Sel& s, int W, int H, int& ow, int& oh) {
    auto len = [&](int a, int b) { return hypot((s.qx[a] - s.qx[b]) * W, (s.qy[a] - s.qy[b]) * H); };
    ow = std::clamp((int)((len(0, 1) + len(3, 2)) / 2 + .5), 4, 8000);
    oh = std::clamp((int)((len(0, 3) + len(1, 2)) / 2 + .5), 4, 8000);
}

// Straightens a four-corner shape into an upright rectangle: a perspective warp (the unit square is mapped onto
// the corners) with bilinear sampling. outW x outH = 0 means the natural size. Null when the corners are unusable.
std::unique_ptr<Bitmap> WarpQuad(Bitmap& src, const Sel& s, int outW = 0, int outH = 0) {
    int W = src.GetWidth(), H = src.GetHeight();
    if (W < 2 || H < 2) return nullptr;
    int nw, nh;
    QuadSize(s, W, H, nw, nh);
    if (outW <= 0 || outH <= 0) { outW = nw; outH = nh; }
    double x[4], y[4];
    for (int i = 0; i < 4; i++) { x[i] = s.qx[i] * W; y[i] = s.qy[i] * H; }
    double dx1 = x[1] - x[2], dx2 = x[3] - x[2], dx3 = x[0] - x[1] + x[2] - x[3];
    double dy1 = y[1] - y[2], dy2 = y[3] - y[2], dy3 = y[0] - y[1] + y[2] - y[3];
    double g = 0, hh = 0, a, b, c = x[0], d, e, f = y[0];
    if (fabs(dx3) < 1e-9 && fabs(dy3) < 1e-9) { a = x[1] - x[0]; b = x[3] - x[0]; d = y[1] - y[0]; e = y[3] - y[0]; }  // a parallelogram
    else {
        double den = dx1 * dy2 - dx2 * dy1;
        if (fabs(den) < 1e-9) return nullptr;
        g = (dx3 * dy2 - dx2 * dy3) / den;
        hh = (dx1 * dy3 - dx3 * dy1) / den;
        a = x[1] - x[0] + g * x[1]; b = x[3] - x[0] + hh * x[3];
        d = y[1] - y[0] + g * y[1]; e = y[3] - y[0] + hh * y[3];
    }
    PixelFormat pf = src.GetPixelFormat();  // read in the bitmap's own 32-bit format when it has one (no copy)
    if (pf != PixelFormat32bppARGB && pf != PixelFormat32bppPARGB) pf = PixelFormat32bppARGB;
    BitmapData sd;
    Rect sr(0, 0, W, H);
    if (src.LockBits(&sr, ImageLockModeRead, pf, &sd) != Ok) return nullptr;
    auto out = std::make_unique<Bitmap>(outW, outH, PixelFormat32bppARGB);
    BitmapData od;
    Rect orc(0, 0, outW, outH);
    if (out->LockBits(&orc, ImageLockModeWrite, PixelFormat32bppARGB, &od) != Ok) { src.UnlockBits(&sd); return nullptr; }
    auto px = [&](int xx, int yy) { return (const uint8_t*)sd.Scan0 + (ptrdiff_t)std::clamp(yy, 0, H - 1) * sd.Stride + (ptrdiff_t)std::clamp(xx, 0, W - 1) * 4; };
    for (int j = 0; j < outH; j++) {
        uint8_t* row = (uint8_t*)od.Scan0 + (ptrdiff_t)j * od.Stride;
        double v = (j + .5) / outH;
        for (int i = 0; i < outW; i++) {
            double u = (i + .5) / outW, k = g * u + hh * v + 1;
            if (fabs(k) < 1e-9) k = 1e-9;
            double sx = (a * u + b * v + c) / k - .5, sy = (d * u + e * v + f) / k - .5;
            int xi = (int)floor(sx), yi = (int)floor(sy);
            double fx = sx - xi, fy = sy - yi;
            const uint8_t *p00 = px(xi, yi), *p10 = px(xi + 1, yi), *p01 = px(xi, yi + 1), *p11 = px(xi + 1, yi + 1);
            for (int ch = 0; ch < 3; ch++)
                row[i * 4 + ch] = (uint8_t)(p00[ch] * (1 - fx) * (1 - fy) + p10[ch] * fx * (1 - fy) + p01[ch] * (1 - fx) * fy + p11[ch] * fx * fy + .5);
            row[i * 4 + 3] = 255;
        }
    }
    out->UnlockBits(&od);
    src.UnlockBits(&sd);
    return out;
}

// Crops one file into dst under the same name and format. A free shape is straightened into a rectangle.
bool SaveCrop(const fs::path& p, const Sel& sel, const fs::path& dst) {
    Bitmap src(p.c_str());
    if (src.GetLastStatus() != Ok || !sel.has) return false;
    std::unique_ptr<Bitmap> c;
    if (sel.quad) c = WarpQuad(src, sel);
    else {
        int W = src.GetWidth(), H = src.GetHeight();
        int x0 = std::clamp((int)(sel.x0 * W + .5), 0, W), x1 = std::clamp((int)(sel.x1 * W + .5), 0, W);
        int y0 = std::clamp((int)(sel.y0 * H + .5), 0, H), y1 = std::clamp((int)(sel.y1 * H + .5), 0, H);
        if (x1 - x0 < 1 || y1 - y0 < 1) return false;
        c.reset(src.Clone(x0, y0, x1 - x0, y1 - y0, PixelFormatDontCare));
    }
    if (!c) return false;
    c->SetResolution(src.GetHorizontalResolution(), src.GetVerticalResolution());

    auto e = Lower(p.extension().wstring());
    const wchar_t* mime = L"image/png";
    if (e == L".jpg" || e == L".jpeg") mime = L"image/jpeg";
    else if (e == L".bmp") mime = L"image/bmp";
    else if (e == L".tif" || e == L".tiff") mime = L"image/tiff";
    else if (e == L".gif") mime = L"image/gif";
    CLSID clsid;
    if (!GetEncoder(mime, &clsid)) return false;

    ULONG q = 95;
    EncoderParameters ep{};
    ep.Count = 1;
    ep.Parameter[0].Guid = EncoderQuality;
    ep.Parameter[0].Type = EncoderParameterValueTypeLong;
    ep.Parameter[0].NumberOfValues = 1;
    ep.Parameter[0].Value = &q;
    bool jpg = !wcscmp(mime, L"image/jpeg");
    return c->Save((dst / p.filename()).c_str(), &clsid, jpg ? &ep : nullptr) == Ok;
}

// ---------------------------------------------------------------- photo detection
// The photo is a big block where most pixels differ from the paper, unlike sparse handwriting/lines.
// Tunables (shortcut: tuned on one office's scans, retune if forms change):
const int    kWork = 500;       // working size (longest side) in pixels
const int    kInkDelta = 40;    // darker than the local paper by this much counts as ink
const int    kPaperSat = 45;    // colour saturation above this counts as ink
const double kDense = 0.75;     // window fill needed to count as "photo-like"
const double kMinArea = 0.006, kMaxArea = 0.30;  // blob bbox as share of page
const double kMinAspect = 0.5, kMaxAspect = 1.3; // width / height
const double kSizeTol = 0.22;   // a box this far from the typical photo size is not trusted
const double kNearMin = 0.10;   // share of dense pixels the size-prior window must hold
const int    kEdge = 10;        // average grey step per pixel row/column that counts as a visible photo border
const double kNearColor = 0.10; // share of colourful pixels it must hold (rejects handwriting/tables)

struct Analysis {
    int w = 0, h = 0, half = 0;
    std::vector<uint8_t> L, ink, dense;  // L = greyscale thumbnail
    std::vector<int> I, D, C;  // integral images of ink, dense and colourful pixels
    int box(const std::vector<int>& T, int xa, int ya, int xb, int yb) const {  // [xa,xb) x [ya,yb)
        return T[(size_t)yb * (w + 1) + xb] - T[(size_t)ya * (w + 1) + xb] - T[(size_t)yb * (w + 1) + xa] + T[(size_t)ya * (w + 1) + xa];
    }
};

std::vector<int> Integral(const std::vector<uint8_t>& m, int w, int h) {
    std::vector<int> T((size_t)(w + 1) * (h + 1), 0);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            T[(size_t)(y + 1) * (w + 1) + x + 1] = m[(size_t)y * w + x] + T[(size_t)y * (w + 1) + x + 1] + T[(size_t)(y + 1) * (w + 1) + x] - T[(size_t)y * (w + 1) + x];
    return T;
}

bool Analyze(Bitmap& src, Analysis& a) {
    int W = src.GetWidth(), H = src.GetHeight();
    if (W < 20 || H < 20) return false;
    double sc = std::min(1.0, (double)kWork / std::max(W, H));
    int w = std::max(1, (int)(W * sc)), h = std::max(1, (int)(H * sc));
    Bitmap thumb(w, h, PixelFormat32bppARGB);
    {
        Graphics g(&thumb);
        g.SetInterpolationMode(InterpolationModeHighQualityBilinear);
        g.DrawImage(&src, Rect(0, 0, w, h), 0, 0, W, H, UnitPixel);
    }
    BitmapData d;
    Rect all(0, 0, w, h);
    if (thumb.LockBits(&all, ImageLockModeRead, PixelFormat32bppARGB, &d) != Ok) return false;
    std::vector<uint8_t> L((size_t)w * h), sat((size_t)w * h);
    for (int y = 0; y < h; y++) {
        const uint8_t* row = (const uint8_t*)d.Scan0 + (size_t)y * d.Stride;
        for (int x = 0; x < w; x++) {
            int b = row[x * 4], gg = row[x * 4 + 1], r = row[x * 4 + 2];
            L[(size_t)y * w + x] = (uint8_t)((r * 30 + gg * 59 + b * 11) / 100);
            sat[(size_t)y * w + x] = (uint8_t)(std::max({r, gg, b}) - std::min({r, gg, b}));
        }
    }
    thumb.UnlockBits(&d);

    // Local paper brightness: 80th percentile of each cell, then the max over nearby cells
    // (so a cell sitting fully inside the photo still sees the paper around it). Handles grey scans.
    int cell = std::max(4, w / 32), cw = (w + cell - 1) / cell, chh = (h + cell - 1) / cell;
    std::vector<int> pc((size_t)cw * chh), bg((size_t)cw * chh);
    for (int cy = 0; cy < chh; cy++)
        for (int cx = 0; cx < cw; cx++) {
            int hist[256] = {0}, n = 0;
            for (int y = cy * cell; y < std::min(h, (cy + 1) * cell); y++)
                for (int x = cx * cell; x < std::min(w, (cx + 1) * cell); x++) { hist[L[(size_t)y * w + x]]++; n++; }
            int acc = 0, v = 0;
            while (v < 255 && acc + hist[v] < n * 0.8) acc += hist[v++];
            pc[(size_t)cy * cw + cx] = v;
        }
    const int rr = 4;
    for (int cy = 0; cy < chh; cy++)
        for (int cx = 0; cx < cw; cx++) {
            int mx = 0;
            for (int y = std::max(0, cy - rr); y <= std::min(chh - 1, cy + rr); y++)
                for (int x = std::max(0, cx - rr); x <= std::min(cw - 1, cx + rr); x++) mx = std::max(mx, pc[(size_t)y * cw + x]);
            bg[(size_t)cy * cw + cx] = mx;
        }
    a.w = w; a.h = h;
    a.L = L;
    a.ink.assign((size_t)w * h, 0);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            size_t i = (size_t)y * w + x;
            a.ink[i] = L[i] < bg[(size_t)(y / cell) * cw + x / cell] - kInkDelta || sat[i] > kPaperSat;
        }
    a.I = Integral(a.ink, w, h);

    a.half = std::max(3, (int)(w * 0.02));
    int mg = std::max(1, (int)(std::min(w, h) * 0.02));  // ignore scanner edges
    a.dense.assign((size_t)w * h, 0);
    for (int y = mg; y < h - mg; y++)
        for (int x = mg; x < w - mg; x++) {
            int xa = std::max(0, x - a.half), xb = std::min(w, x + a.half + 1), ya = std::max(0, y - a.half), yb = std::min(h, y + a.half + 1);
            a.dense[(size_t)y * w + x] = a.box(a.I, xa, ya, xb, yb) >= kDense * (xb - xa) * (yb - ya);
        }
    a.D = Integral(a.dense, w, h);
    std::vector<uint8_t> col((size_t)w * h);
    for (size_t i = 0; i < col.size(); i++) col[i] = sat[i] > kPaperSat;
    a.C = Integral(col, w, h);  // faces/clothes are colourful, handwriting is not
    return true;
}

// Strict: the biggest photo-shaped solid block on the page.
bool FindStrict(const Analysis& a, Sel& s) {
    int w = a.w, h = a.h, half = a.half;
    std::vector<uint8_t> seen((size_t)w * h, 0);
    std::vector<int> stack;
    int best = 0, bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    for (int sy = 0; sy < h; sy++)
        for (int sx = 0; sx < w; sx++) {
            size_t si = (size_t)sy * w + sx;
            if (!a.dense[si] || seen[si]) continue;
            int cnt = 0, x0 = sx, x1 = sx, y0 = sy, y1 = sy;
            stack.assign(1, (int)si); seen[si] = 1;
            while (!stack.empty()) {
                int i = stack.back(); stack.pop_back();
                int x = i % w, y = i / w;
                cnt++; x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y);
                const int nx[4] = {x - 1, x + 1, x, x}, ny[4] = {y, y, y - 1, y + 1};
                for (int k = 0; k < 4; k++) {
                    if (nx[k] < 0 || ny[k] < 0 || nx[k] >= w || ny[k] >= h) continue;
                    size_t ni = (size_t)ny[k] * w + nx[k];
                    if (a.dense[ni] && !seen[ni]) { seen[ni] = 1; stack.push_back((int)ni); }
                }
            }
            double bw = x1 - x0 + 1, bh = y1 - y0 + 1, area = bw * bh / ((double)w * h);
            if (area < kMinArea || area > kMaxArea || cnt < 0.6 * bw * bh) continue;
            if (bw / bh < kMinAspect || bw / bh > kMaxAspect) continue;
            if (cnt > best) { best = cnt; bx0 = x0; bx1 = x1; by0 = y0; by1 = y1; }
        }
    if (!best) return false;
    // dense map is eroded by the window; grow the box, then trim edges that are mostly paper
    bx0 = std::max(0, bx0 - half); by0 = std::max(0, by0 - half);
    bx1 = std::min(w - 1, bx1 + half); by1 = std::min(h - 1, by1 + half);
    auto rowFill = [&](int y) { return a.box(a.I, bx0, y, bx1 + 1, y + 1) / (double)(bx1 - bx0 + 1); };
    auto colFill = [&](int x) { return a.box(a.I, x, by0, x + 1, by1 + 1) / (double)(by1 - by0 + 1); };
    while (bx0 < bx1 && colFill(bx0) < 0.5) bx0++;
    while (bx1 > bx0 && colFill(bx1) < 0.5) bx1--;
    while (by0 < by1 && rowFill(by0) < 0.5) by0++;
    while (by1 > by0 && rowFill(by1) < 0.5) by1--;
    s = {(double)bx0 / w, (double)by0 / h, (double)(bx1 + 1) / w, (double)(by1 + 1) / h, true};
    return true;
}

// Fallback for photos with a light backdrop: slide a window of the typical photo size (pw x ph, as
// fractions of the page) to where the most solid pixels sit. A guess, so the caller marks it "check".
bool FindNear(const Analysis& a, double pw, double ph, Sel& s) {
    int w = a.w, h = a.h, ww = std::max(4, (int)(pw * w + .5)), wh = std::max(4, (int)(ph * h + .5));
    if (ww >= w || wh >= h) return false;
    int best = -1, bx = 0, by = 0;
    for (int y = 0; y + wh <= h; y += 2)
        for (int x = 0; x + ww <= w; x += 2) {
            if (a.box(a.C, x, y, x + ww, y + wh) < kNearColor * ww * wh) continue;
            int v = a.box(a.D, x, y, x + ww, y + wh);
            if (v > best) { best = v; bx = x; by = y; }
        }
    if (best < kNearMin * ww * wh) return false;
    // Photos are cut off at the chest, so the solid mass (suit/clothes) ends at the photo's bottom edge:
    // line the window's bottom up with it (the dense map is eroded a little, hence the margin).
    {
        int y0 = std::max(0, by - wh / 4), y1 = std::min(h, by + wh + wh / 4), bot = -1;
        for (int y = y0; y < y1; y++)
            if (a.box(a.D, bx, y, bx + ww, y + 1) >= 0.15 * ww) bot = y;
        if (bot >= by + wh * 0.55 && bot <= by + wh * 1.2) by = std::clamp(bot + 1 + (int)(a.half * 0.6) - wh, 0, h - wh);
        double sx = 0, n = 0;  // centre sideways on the mass
        for (int y = by; y < by + wh; y++)
            for (int x = bx; x < bx + ww; x++)
                if (a.dense[(size_t)y * w + x]) { sx += x; n++; }
        if (n > 0) bx = std::clamp((int)(sx / n - ww / 2.0 + .5), 0, w - ww);
    }
    // Snap each edge to a visible photo border (paper edge / shadow line) when there is one nearby.
    {
        int d = std::max(3, (int)(ww * 0.07)), x0 = bx, x1 = bx + ww, y0 = by, y1 = by + wh;
        auto vert = [&](int x, int ya, int yb) {  // grey step across column x
            if (x < 1 || x > w - 2) return 0.0;
            double t = 0;
            for (int y = ya; y < yb; y++) t += abs((int)a.L[(size_t)y * w + x + 1] - (int)a.L[(size_t)y * w + x - 1]);
            return t / std::max(1, yb - ya);
        };
        auto horz = [&](int y, int xa, int xb) {
            if (y < 1 || y > h - 2) return 0.0;
            double t = 0;
            for (int x = xa; x < xb; x++) t += abs((int)a.L[(size_t)(y + 1) * w + x] - (int)a.L[(size_t)(y - 1) * w + x]);
            return t / std::max(1, xb - xa);
        };
        int ya = by + wh / 6, yb = by + wh - wh / 6, xa = bx + ww / 6, xb = bx + ww - ww / 6;
        auto snapV = [&](int x) { int best = x; double bs = kEdge; for (int c = x - d; c <= x + d; c++) { double g = vert(c, ya, yb); if (g > bs) { bs = g; best = c; } } return best; };
        auto snapH = [&](int y) { int best = y; double bs = kEdge; for (int c = y - d; c <= y + d; c++) { double g = horz(c, xa, xb); if (g > bs) { bs = g; best = c; } } return best; };
        int nx0 = snapV(x0), nx1 = snapV(x1), ny0 = snapH(y0), ny1 = snapH(y1);
        if (nx1 - nx0 > 0.85 * ww && nx1 - nx0 < 1.15 * ww && ny1 - ny0 > 0.85 * wh && ny1 - ny0 < 1.15 * wh) { bx = nx0; ww = nx1 - nx0; by = ny0; wh = ny1 - ny0; }
        bx = std::clamp(bx, 0, w - ww); by = std::clamp(by, 0, h - wh);
    }
    s = {(double)bx / w, (double)by / h, (double)(bx + ww) / w, (double)(by + wh) / h, true};
    return true;
}

// ---------------------------------------------------------------- background jobs (no UI here)
std::atomic<bool> g_cancel{false};
std::atomic<int> g_doneN{0}, g_totalN{0}, g_phase{0};

template <class F> void ParallelFor(int n, F f) {
    std::atomic<int> next{0};
    int nt = std::max(1, std::min(4, (int)std::thread::hardware_concurrency()));
    std::vector<std::thread> t;
    for (int k = 0; k < nt; k++)
        t.emplace_back([&] {
            for (;;) {
                int i = next++;
                if (i >= n || g_cancel) break;
                f(i);
            }
        });
    for (auto& x : t) x.join();
}

struct Job {
    std::vector<fs::path> paths;
    std::vector<char> todo;
    std::vector<Sel> sel;    // detect: result; save: input
    std::vector<char> src;   // detect: result source
    std::vector<char> done;  // detect: processed; save: saved ok
    fs::path dst;
};

// Pass 1 finds strict boxes; the typical photo size is the median of those. Pass 2 re-checks every
// image whose box is missing or far from that size using the size-prior window.
void RunDetect(Job& j) {
    int n = (int)j.paths.size();
    j.sel.assign(n, Sel()); j.src.assign(n, S_NONE); j.done.assign(n, 0);
    std::vector<int> idx;
    for (int i = 0; i < n; i++) if (j.todo[i]) idx.push_back(i);
    std::vector<char> found(n, 0);
    std::vector<Sel> s1(n);
    g_phase = 1; g_doneN = 0; g_totalN = (int)idx.size();
    ParallelFor((int)idx.size(), [&](int k) {
        int i = idx[k];
        Bitmap b(j.paths[i].c_str());
        Analysis a;
        if (b.GetLastStatus() == Ok && Analyze(b, a)) found[i] = FindStrict(a, s1[i]);
        g_doneN++;
    });
    if (g_cancel) return;
    std::vector<double> ws, hs;
    for (int i : idx) if (found[i]) { ws.push_back(s1[i].x1 - s1[i].x0); hs.push_back(s1[i].y1 - s1[i].y0); }
    bool prior = ws.size() >= 5;
    double pw = 0, ph = 0;
    if (prior) {
        std::nth_element(ws.begin(), ws.begin() + ws.size() / 2, ws.end());
        std::nth_element(hs.begin(), hs.begin() + hs.size() / 2, hs.end());
        pw = ws[ws.size() / 2]; ph = hs[hs.size() / 2];
    }
    std::vector<int> need;
    for (int i : idx) {
        bool good = found[i];
        if (good && prior) {
            double w = s1[i].x1 - s1[i].x0, h = s1[i].y1 - s1[i].y0;
            good = fabs(w - pw) <= kSizeTol * pw && fabs(h - ph) <= kSizeTol * ph;
        }
        if (good) { j.sel[i] = s1[i]; j.src[i] = S_AUTO; j.done[i] = 1; }
        else need.push_back(i);
    }
    g_phase = 2; g_doneN = 0; g_totalN = (int)need.size();
    ParallelFor((int)need.size(), [&](int k) {
        int i = need[k];
        Sel s;
        bool g = false;
        if (prior) {
            Bitmap b(j.paths[i].c_str());
            Analysis a;
            if (b.GetLastStatus() == Ok && Analyze(b, a)) g = FindNear(a, pw, ph, s);
        }
        if (g) { j.sel[i] = s; j.src[i] = S_GUESS; }
        else if (found[i]) { j.sel[i] = s1[i]; j.src[i] = S_GUESS; }  // odd-sized strict box: show it, but ask for a check
        j.done[i] = 1;
        g_doneN++;
    });
}

void RunSave(Job& j) {
    std::vector<int> idx;
    for (int i = 0; i < (int)j.paths.size(); i++) if (j.todo[i]) idx.push_back(i);
    j.done.assign(j.paths.size(), 0);
    g_phase = 3; g_doneN = 0; g_totalN = (int)idx.size();
    ParallelFor((int)idx.size(), [&](int k) {
        int i = idx[k];
        j.done[i] = SaveCrop(j.paths[i], j.sel[i], j.dst);
        g_doneN++;
    });
}

// ---------------------------------------------------------------- export selection and remembered state
// Which images "Done & Export" writes: all = every image that has a box; otherwise only new or changed
// ones, and unconfirmed guesses only when withGuess.
std::vector<char> ExportSet(const std::vector<Item>& items, bool withGuess, bool all) {
    std::vector<char> r;
    for (auto& i : items) {
        bool ok = i.sel.has && (all || i.src == S_AUTO || i.src == S_USER || (withGuess && i.src == S_GUESS));
        if (ok && !all && i.saved) ok = false;  // already exported and unchanged: leave that file alone
        r.push_back(ok);
    }
    return r;
}

// Writes text so a crash or a full disk never leaves a half-written file: temp file first, then replace.
bool WriteTextAtomic(const fs::path& target, const std::wstring& text) {
    fs::path tmp = target;
    tmp += L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"w, ccs=UTF-8");
    if (!f) return false;
    bool ok = fputws(text.c_str(), f) >= 0;
    ok = fclose(f) == 0 && ok;
    if (!ok) { DeleteFileW(tmp.c_str()); return false; }
    return MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

std::wstring ReadText(const fs::path& p) {
    std::wstring t;
    FILE* f = _wfopen(p.c_str(), L"r, ccs=UTF-8");
    if (!f) return t;
    wchar_t buf[2048];
    while (fgetws(buf, 2048, f)) t += buf;
    fclose(f);
    return t;
}

// The remembered state is one text line per image: name|source|saved|x0|y0|x1|y1, followed for a free shape by
// |1|x|y|x|y|x|y|x|y (its four corners). A bar separates the fields because file names cannot contain one. Line 1 is the source folder,
// line 2 is "dst=" plus the folder the images were exported to.
std::wstring SerializeState(const fs::path& src, const fs::path& dst, const std::vector<Item>& items) {
    const wchar_t NL = 10;  // line break written as a number so no escape sequence is needed
    std::wstring t = src.wstring() + NL + L"dst=" + dst.wstring() + NL;
    wchar_t buf[256];
    for (auto& i : items)
        if (i.src != S_NONE || i.sel.has || i.saved) {
            swprintf_s(buf, L"|%d|%d|%.6f|%.6f|%.6f|%.6f", (int)i.src, (int)i.saved, i.sel.x0, i.sel.y0, i.sel.x1, i.sel.y1);
            t += i.path.filename().wstring() + buf;
            if (i.sel.quad) {
                const Sel& q = i.sel;
                swprintf_s(buf, L"|1|%.6f|%.6f|%.6f|%.6f|%.6f|%.6f|%.6f|%.6f", q.qx[0], q.qy[0], q.qx[1], q.qy[1], q.qx[2], q.qy[2], q.qx[3], q.qy[3]);
                t += buf;
            }
            t += NL;
        }
    return t;
}

// Puts saved boxes back on the items; returns how many images got a box. An image counts as exported only
// if it went to the current destination and exists(file name) says the exported file is still there.
template <class F> int ParseState(const std::wstring& text, const fs::path& dstNow, std::vector<Item>& items, F exists) {
    std::unordered_map<std::wstring, size_t> byName;
    for (size_t i = 0; i < items.size(); i++) byName[Lower(items[i].path.filename().wstring())] = i;
    std::wstring savedDst;
    int n = 0, lineNo = 0;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find(wchar_t(10), pos);
        std::wstring t = text.substr(pos, e == std::wstring::npos ? std::wstring::npos : e - pos);
        pos = e == std::wstring::npos ? text.size() : e + 1;
        while (!t.empty() && t.back() == 13) t.pop_back();
        if (lineNo++ == 0) continue;  // line 1: the source folder
        if (lineNo == 2 && t.rfind(L"dst=", 0) == 0) { savedDst = t.substr(4); continue; }
        std::wstring fld[16];
        size_t p = 0;
        int k = 0;
        for (; k < 16; k++) {
            size_t b = t.find(L'|', p);
            fld[k] = t.substr(p, b == std::wstring::npos ? std::wstring::npos : b - p);
            if (b == std::wstring::npos) { k++; break; }
            p = b + 1;
        }
        if (k < 7) continue;
        auto it = byName.find(Lower(fld[0]));
        if (it == byName.end()) continue;
        Item& m = items[it->second];
        m.src = (char)std::clamp(_wtoi(fld[1].c_str()), 0, 3);
        m.saved = _wtoi(fld[2].c_str()) != 0;
        m.sel = {_wtof(fld[3].c_str()), _wtof(fld[4].c_str()), _wtof(fld[5].c_str()), _wtof(fld[6].c_str()), false};
        m.sel.has = m.sel.x1 > m.sel.x0 && m.sel.y1 > m.sel.y0;
        if (m.sel.has && k >= 16 && fld[7] == L"1") {  // a free shape
            m.sel.quad = true;
            for (int c = 0; c < 4; c++) { m.sel.qx[c] = _wtof(fld[8 + 2 * c].c_str()); m.sel.qy[c] = _wtof(fld[9 + 2 * c].c_str()); }
        }
        n += m.sel.has;
    }
    bool sameDst = !dstNow.empty() && Lower(savedDst) == Lower(dstNow.wstring());
    for (auto& m : items) m.saved = m.saved && sameDst && exists(m.path.filename());
    return n;
}

