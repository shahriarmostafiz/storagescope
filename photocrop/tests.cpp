// Tests for the parts of PhotoCrop that can break quietly. Run with:  build.bat test
//   tests.exe            runs the checks below, exit code 0 = all passed
//   tests.exe <folder>   prints what auto-detect would do for every image in a folder (for tuning)
#include "core.h"
#include <cstdio>
#include <cstdlib>

static int g_fail = 0, g_run = 0;
#define CHECK(cond, name) do { g_run++; if (!(cond)) { g_fail++; printf("FAIL: %s\n", name); } else printf("ok:   %s\n", name); } while (0)

// A page like a scanned form: lines of text and one coloured photo block at (px,py,pw,ph).
static void MakePage(const fs::path& file, int px, int py, int pw, int ph) {
    Bitmap bmp(1240, 1754, PixelFormat32bppARGB);
    {
        Graphics g(&bmp);
        g.Clear(Color(255, 255, 255));
        Font font(L"Arial", 22.0f, FontStyleRegular, UnitPixel);
        SolidBrush black(Color(255, 0, 0, 0));
        Pen pen(Color(255, 0, 0, 0), 3);
        for (int i = 0; i < 14; i++) {
            g.DrawString(L"Name / Address / Date of birth handwritten text here", -1, &font, PointF(60, (REAL)(150 + i * 90)), &black);
            g.DrawLine(&pen, 60, 190 + i * 90, 900, 190 + i * 90);
        }
        LinearGradientBrush photo(Rect(px, py, pw, ph), Color(255, 70, 130, 180), Color(255, 210, 180, 140), LinearGradientModeVertical);
        g.FillRectangle(&photo, px, py, pw, ph);
        SolidBrush face(Color(255, 205, 133, 63));
        g.FillEllipse(&face, px + pw * 3 / 10, py + ph / 5, pw * 2 / 5, ph * 7 / 20);
    }
    CLSID jpg;
    GetEncoder(L"image/jpeg", &jpg);
    bmp.Save(file.c_str(), &jpg, nullptr);
}

static bool Near(const Sel& s, double x0, double y0, double x1, double y1, double tol = 0.03) {
    return s.has && fabs(s.x0 - x0) <= tol && fabs(s.y0 - y0) <= tol && fabs(s.x1 - x1) <= tol && fabs(s.y1 - y1) <= tol;
}

static Item Make(const wchar_t* name, char src, bool saved, bool has) {
    Item i;
    i.path = fs::path(L"C:/scans") / name;
    i.src = src;
    i.saved = saved;
    if (has) i.sel = {0.1, 0.2, 0.3, 0.4, true};
    return i;
}

static void TestDetection(const fs::path& tmp) {
    fs::path page = tmp / L"page.jpg";
    MakePage(page, 900, 120, 260, 330);
    const double ex0 = 900 / 1240.0, ey0 = 120 / 1754.0, ex1 = 1160 / 1240.0, ey1 = 450 / 1754.0;
    {
        Bitmap b(page.c_str());
        Analysis a;
        Sel s;
        CHECK(Analyze(b, a) && FindStrict(a, s) && Near(s, ex0, ey0, ex1, ey1), "strict detection finds the photo within 3% on a generated page");
        Sel guess;
        CHECK(FindNear(a, ex1 - ex0, ey1 - ey0, guess) && Near(guess, ex0, ey0, ex1, ey1, 0.05), "size-prior search lands on the same photo");
    }
    // the real two-pass job and the save job, end to end on 8 pages (some with a slightly different photo size)
    fs::path src = tmp / L"src", dst = tmp / L"dst";
    fs::create_directories(src);
    fs::create_directories(dst);
    Job j;
    for (int k = 0; k < 8; k++) {
        fs::path f = src / (L"p" + std::to_wstring(k) + L".jpg");
        MakePage(f, 880 + k * 4, 110 + k * 3, 260 + (k % 2) * 20, 330 + (k % 2) * 20);
        j.paths.push_back(f);
    }
    j.todo.assign(j.paths.size(), 1);
    g_cancel = false;
    RunDetect(j);
    int autoN = 0;
    for (size_t i = 0; i < j.paths.size(); i++) autoN += j.src[i] == S_AUTO && Near(j.sel[i], ex0, ey0, ex1 + 0.03, ey1 + 0.03, 0.05);
    CHECK(autoN == 8, "detect job marks all 8 generated pages as auto with a box on the photo");
    j.dst = dst;
    j.todo.assign(j.paths.size(), 1);
    RunSave(j);
    int saved = 0;
    for (size_t i = 0; i < j.paths.size(); i++) {
        fs::path out = dst / j.paths[i].filename();
        Bitmap o(out.c_str());
        saved += j.done[i] && o.GetLastStatus() == Ok && o.GetWidth() > 200 && o.GetWidth() < 330 && o.GetHeight() > 280 && o.GetHeight() < 420;
    }
    CHECK(saved == 8, "save job writes 8 cropped files with the same names and photo-sized dimensions");
}

static void TestExportSet() {
    std::vector<Item> it = {
        Make(L"a.jpg", S_AUTO, false, true),   // new auto box            -> export
        Make(L"b.jpg", S_AUTO, true, true),    // exported, unchanged     -> skip
        Make(L"c.jpg", S_USER, false, true),   // edited by the user      -> export
        Make(L"d.jpg", S_GUESS, false, true),  // unconfirmed guess       -> only with withGuess
        Make(L"e.jpg", S_NONE, false, false),  // no box                  -> never
        Make(L"f.jpg", S_GUESS, true, true),   // exported guess          -> only when exporting all
    };
    auto r = ExportSet(it, false, false);
    CHECK(r[0] && !r[1] && r[2] && !r[3] && !r[4] && !r[5], "changed-only export writes new/edited images and skips exported and unconfirmed ones");
    r = ExportSet(it, true, false);
    CHECK(r[0] && !r[1] && r[2] && r[3] && !r[4] && !r[5], "changed-only export with guesses also writes unconfirmed guesses");
    r = ExportSet(it, false, true);
    CHECK(r[0] && r[1] && r[2] && r[3] && !r[4] && r[5], "export-all writes every image that has a box, and never one without");
}

static void TestStatus() {
    Item done = Make(L"a.jpg", S_AUTO, true, true);       // confident box, exported            -> done
    Item userDone = Make(L"b.jpg", S_USER, true, true);   // confirmed by the user, exported    -> done
    Item waiting = Make(L"c.jpg", S_AUTO, false, true);   // box not exported yet                -> waiting
    Item guessOut = Make(L"d.jpg", S_GUESS, true, true);  // exported, but only a guess          -> review
    Item guessNew = Make(L"e.jpg", S_GUESS, false, true); // guess not exported                  -> review
    Item none = Make(L"f.jpg", S_NONE, false, false);     // no photo found                      -> missing
    Item failed = Make(L"g.jpg", S_AUTO, false, true);    // export failed                       -> missing
    failed.failed = true;
    CHECK(StatusOf(done) == ST_DONE && StatusOf(userDone) == ST_DONE && StatusOf(waiting) == ST_WAITING, "status: exported images are done, unexported boxes are waiting");
    CHECK(StatusOf(guessOut) == ST_REVIEW && StatusOf(guessNew) == ST_REVIEW, "status: a guess needs a better crop whether or not it was exported");
    CHECK(StatusOf(none) == ST_MISSING && StatusOf(failed) == ST_MISSING, "status: no box or a failed export counts as not completed");
    std::vector<Item> all = {done, userDone, waiting, guessOut, guessNew, none, failed};
    int n[4] = {0, 0, 0, 0};
    for (auto& i : all) for (int f = 0; f < 4; f++) n[f] += InFilter(i, f);
    CHECK(n[0] == 7 && n[1] == 2 && n[2] == 2 && n[3] == 2, "filter tabs: All shows everything, Done 2, Needs better crop 2, Not completed 2");
    guessOut.sel = {0.2, 0.2, 0.4, 0.4, true};
    guessOut.src = S_USER; guessOut.saved = false;  // the operator fixed it: it leaves the Review tab and waits for export
    CHECK(StatusOf(guessOut) == ST_WAITING && !InFilter(guessOut, 2), "status: a corrected guess moves out of the review list");
}

// A skewed, tilted green shape on a white page with a black dot near its top-left corner: straightening it must give an
// upright rectangle of the right size that is all shape (no page), with the dot still top-left.
static void TestWarp(const fs::path& tmp) {
    const int W = 800, H = 600;
    const double px[4] = {200, 600, 560, 150}, py[4] = {100, 160, 500, 420};  // TL, TR, BR, BL in pixels
    Bitmap bmp(W, H, PixelFormat32bppARGB);
    {
        Graphics g(&bmp);
        g.Clear(Color(255, 255, 255));
        g.SetSmoothingMode(SmoothingModeNone);
        PointF poly[4];
        for (int i = 0; i < 4; i++) poly[i] = PointF((REAL)px[i], (REAL)py[i]);
        SolidBrush green(Color(255, 40, 170, 90)), black(Color(255, 0, 0, 0));
        g.FillPolygon(&green, poly, 4);
        double mx = px[0] + 0.1 * (px[1] - px[0]) + 0.1 * (px[3] - px[0]), my = py[0] + 0.1 * (py[1] - py[0]) + 0.1 * (py[3] - py[0]);
        g.FillEllipse(&black, (REAL)(mx - 10), (REAL)(my - 10), 20.0f, 20.0f);
    }
    Sel s;
    s.has = true;
    ToQuad(s);
    for (int i = 0; i < 4; i++) { s.qx[i] = px[i] / W; s.qy[i] = py[i] / H; }
    QuadBounds(s);
    int ow, oh;
    QuadSize(s, W, H, ow, oh);
    CHECK(abs(ow - 411) <= 2 && abs(oh - 333) <= 2, "free shape: output size is the shape's real width and height");
    auto out = WarpQuad(bmp, s);
    bool ok = out && (int)out->GetWidth() == ow && (int)out->GetHeight() == oh;
    int white = 0, dark = 0;
    double sx = 0, sy = 0;
    if (ok)
        for (int y = 0; y < oh; y++)
            for (int x = 0; x < ow; x++) {
                Color c;
                out->GetPixel(x, y, &c);
                white += c.GetR() > 235 && c.GetG() > 235 && c.GetB() > 235;
                if (c.GetR() < 60 && c.GetG() < 60 && c.GetB() < 60) { dark++; sx += x; sy += y; }
            }
    CHECK(ok && white < ow * oh / 50, "free shape: the straightened image contains the shape only, no page background");
    double cu = dark ? sx / dark / ow : -1, cv = dark ? sy / dark / oh : -1;
    CHECK(dark > 20 && cu > 0.04 && cu < 0.22 && cv > 0.04 && cv < 0.22, "free shape: a mark near the top-left corner stays top-left (not mirrored or rotated)");
    fs::path page = tmp / L"skew.jpg", dst = tmp / L"skew_out";
    fs::create_directories(dst);
    CLSID jpg;
    GetEncoder(L"image/jpeg", &jpg);
    bmp.Save(page.c_str(), &jpg, nullptr);
    bool saved = SaveCrop(page, s, dst);
    Bitmap o((dst / L"skew.jpg").c_str());
    CHECK(saved && o.GetLastStatus() == Ok && abs((int)o.GetWidth() - ow) <= 1 && abs((int)o.GetHeight() - oh) <= 1, "free shape: saving writes the straightened photo with the same name");
    Sel bad = s;
    bad.qx[1] = bad.qx[0]; bad.qy[1] = bad.qy[0]; bad.qx[2] = bad.qx[0]; bad.qy[2] = bad.qy[0];  // all corners on one line: no usable shape
    auto none = WarpQuad(bmp, bad);
    CHECK(!none || (none->GetWidth() <= 8000 && none->GetHeight() <= 8000), "free shape: degenerate corners do not crash");
}

static void TestState() {
    std::vector<Item> a = {
        Make(L"one.jpg", S_AUTO, true, true),
        Make(L"\u09AB\u09BE\u0987\u09B2 \u09E7.jpg", S_USER, false, true),  // a Bengali file name with a space
        Make(L"three.jpg", S_GUESS, false, true),
        Make(L"four.jpg", S_NONE, false, false),
    };
    a[0].sel = {0.125, 0.25, 0.5, 0.75, true};
    std::wstring text = SerializeState(L"C:/scans", L"C:/out", a);
    auto fresh = [&]() { std::vector<Item> v; for (auto& i : a) { Item n; n.path = i.path; v.push_back(n); } return v; };

    auto b = fresh();
    int n = ParseState(text, L"C:/out", b, [](const fs::path&) { return true; });
    bool same = n == 3;
    for (size_t i = 0; i < a.size(); i++) same = same && b[i].src == a[i].src && b[i].saved == a[i].saved && b[i].sel.has == a[i].sel.has;
    same = same && fabs(b[0].sel.x0 - 0.125) < 1e-6 && fabs(b[0].sel.y1 - 0.75) < 1e-6;
    CHECK(same, "saved state comes back exactly: boxes, sources, exported marks, non-English file names");

    b = fresh();
    ParseState(text, L"C:/elsewhere", b, [](const fs::path&) { return true; });
    CHECK(!b[0].saved && b[0].sel.has, "a different destination clears the exported marks but keeps the boxes");

    b = fresh();
    ParseState(text, L"C:/out", b, [](const fs::path&) { return false; });
    CHECK(!b[0].saved && b[0].sel.has, "an exported file that was deleted loses its exported mark");

    Item q = Make(L"q.jpg", S_USER, false, true);  // a free shape survives a restart, rectangles stay rectangles
    ToQuad(q.sel);
    q.sel.qx[1] = 0.35; q.sel.qy[1] = 0.18; q.sel.qx[2] = 0.28; q.sel.qy[3] = 0.43;
    QuadBounds(q.sel);
    std::vector<Item> qa = {q}, qb(1);
    qb[0].path = q.path;
    ParseState(SerializeState(L"C:/scans", L"C:/out", qa), L"C:/out", qb, [](const fs::path&) { return true; });
    bool qok = qb[0].sel.has && qb[0].sel.quad;
    for (int c = 0; c < 4 && qok; c++) qok = fabs(qb[0].sel.qx[c] - q.sel.qx[c]) < 1e-6 && fabs(qb[0].sel.qy[c] - q.sel.qy[c]) < 1e-6;
    CHECK(qok && fabs(qb[0].sel.x1 - 0.35) < 1e-6 && fabs(qb[0].sel.y1 - 0.43) < 1e-6, "saved state keeps a free shape's four corners and its bounding box");
    b = fresh();
    ParseState(text, L"C:/out", b, [](const fs::path&) { return true; });
    CHECK(!b[0].sel.quad && !b[2].sel.quad, "older state lines without corners load as plain rectangles");

    b = fresh();
    std::wstring broken = text + L"garbage line without fields\n|||||\nmissing.jpg|1|0|0.1|0.1|0.2|0.2\n";
    CHECK(ParseState(broken, L"C:/out", b, [](const fs::path&) { return true; }) == 3, "damaged or unknown lines are ignored");
}

static void TestAtomicWrite(const fs::path& tmp) {
    fs::path f = tmp / L"state.txt";
    std::wstring first = L"first \u09AB\u09BE\u0987\u09B2", second = L"second";
    bool ok = WriteTextAtomic(f, first) && ReadText(f) == first;
    ok = ok && WriteTextAtomic(f, second) && ReadText(f) == second;
    std::error_code ec;
    CHECK(ok && !fs::exists(fs::path(f.wstring() + L".tmp"), ec), "atomic write replaces the file and leaves no temp file behind");
}

int main(int argc, char** argv) {
    GdiplusStartupInput gi;
    ULONG_PTR tok;
    GdiplusStartup(&tok, &gi, nullptr);
    {
        if (argc > 1) {  // folder mode: report what auto-detect does, for tuning on real scans
            Job j;
            for (auto& e : fs::directory_iterator(fs::u8path(argv[1]))) if (e.is_regular_file() && IsImage(e.path())) j.paths.push_back(e.path());
            std::sort(j.paths.begin(), j.paths.end());
            j.todo.assign(j.paths.size(), 1);
            RunDetect(j);
            int c[3] = {0, 0, 0};
            const char* nm[3] = {"none", "auto", "guess"};
            for (size_t i = 0; i < j.paths.size(); i++) {
                c[(int)j.src[i]]++;
                const Sel& s = j.sel[i];
                printf("%s %s %.3f %.3f %.3f %.3f\n", j.paths[i].filename().string().c_str(), nm[(int)j.src[i]], s.x0, s.y0, s.x1, s.y1);
            }
            printf("TOTAL %d AUTO %d GUESS %d NONE %d\n", (int)j.paths.size(), c[1], c[2], c[0]);
        } else {
            wchar_t base[MAX_PATH];
            GetTempPathW(MAX_PATH, base);
            fs::path tmp = fs::path(base) / L"photocrop_tests";
            std::error_code ec;
            fs::remove_all(tmp, ec);
            fs::create_directories(tmp);
            TestExportSet();
            TestStatus();
            TestState();
            TestWarp(tmp);
            TestAtomicWrite(tmp);
            TestDetection(tmp);
            fs::remove_all(tmp, ec);
            printf("\n%d of %d checks passed\n", g_run - g_fail, g_run);
        }
    }
    GdiplusShutdown(tok);
    return g_fail ? 1 : 0;
}
