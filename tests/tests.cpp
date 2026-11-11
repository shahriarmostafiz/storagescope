// StorageScope test runner (no framework). Exit code 0 == all checks passed.
// Safety tests come first; file-system tests use a private directory under %TEMP%.
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "app/History.h"
#include "app/Settings.h"
#include "scanner/FileTypes.h"
#include "scanner/DirectoryEnum.h"
#include "scanner/ScanSnapshot.h"
#include "scanner/TreeSync.h"
#include "scanner/ScanStats.h"
#include "cleanup/TempCleanup.h"
#include "filesystem/ChangeWatcher.h"
#include "filesystem/PathUtils.h"
#include "operations/FileOperationService.h"
#include "operations/RecycleBin.h"
#include "safety/PathProtectionService.h"
#include "scanner/DirectoryScanner.h"
#include "utils/FormatUtils.h"

using namespace ss;

static int g_pass = 0, g_fail = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (c) ++g_pass;                                                 \
        else { ++g_fail; printf("  FAIL line %d: %s\n", __LINE__, #c); } \
    } while (0)
#define SECTION(n) printf("[%s]\n", n)

// ---------------------------------------------------------------- helpers
static std::wstring Ext(const std::wstring& p) { return path::ToExtended(p); }
static bool Exists(const std::wstring& p) { return GetFileAttributesW(Ext(p).c_str()) != INVALID_FILE_ATTRIBUTES; }
static void MkDir(const std::wstring& p) { CreateDirectoryW(Ext(p).c_str(), nullptr); }
static void MkDirs(const std::wstring& p) {
    for (size_t i = 3; i < p.size(); ++i)
        if (p[i] == L'\\') MkDir(p.substr(0, i));
    MkDir(p);
}
static void MkFile(const std::wstring& p, DWORD bytes) {
    HANDLE h = CreateFileW(Ext(p).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::string d(bytes, 'x');
    DWORD w;
    WriteFile(h, d.data(), bytes, &w, nullptr);
    CloseHandle(h);
}
static void SetAge(const std::wstring& p, int days) {
    HANDLE h = CreateFileW(Ext(p).c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER u{{now.dwLowDateTime, now.dwHighDateTime}};
    u.QuadPart -= static_cast<ULONGLONG>(days) * 24 * 3600 * 10000000ull;
    FILETIME ft{u.LowPart, u.HighPart};
    SetFileTime(h, &ft, &ft, &ft);
    CloseHandle(h);
}
static bool MkJunction(const std::wstring& link, const std::wstring& target) {
    std::wstring cmd = L"cmd.exe /c mklink /J \"" + link + L"\" \"" + target + L"\" >nul";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, 10000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return Exists(link);
}
static std::wstring TempBase() {
    wchar_t t[MAX_PATH];
    GetTempPathW(MAX_PATH, t);
    std::wstring b = path::Canonicalize(t) + L"\\StorageScopeTest_" + std::to_wstring(GetCurrentProcessId());
    MkDirs(b);
    return b;
}
static ScanNode* Child(ScanNode* n, const wchar_t* name) {
    if (!n->dir) return nullptr;
    for (auto& c : n->dir->children)
        if (c.name == name) return &c;
    return nullptr;
}
static std::unique_ptr<ScanResult> RunScan(const std::vector<std::wstring>& roots, std::stop_token st, unsigned workers = 4) {
    DirectoryScanner ds;
    std::unique_ptr<ScanResult> out;
    ScanCallbacks cb;
    cb.onFinished = [&](std::unique_ptr<ScanResult> r) { out = std::move(r); };
    ds.Scan(roots, ScanOptions{workers}, cb, st);
    return out;
}

static PathProtectionService FixedService() {
    PathProtectionService::Config c;
    c.systemRoot = L"C:\\Windows";
    c.programDirs = {L"C:\\Program Files", L"C:\\Program Files (x86)"};
    c.usersRoot = L"C:\\Users";
    c.programData = L"C:\\ProgramData";
    c.userProfile = L"C:\\Users\\John";
    c.appData = L"C:\\Users\\John\\AppData\\Roaming";
    c.localAppData = L"C:\\Users\\John\\AppData\\Local";
    return PathProtectionService(c);
}

// ---------------------------------------------------------------- tests
static void TestFormat() {
    SECTION("FormatBytes");
    CHECK(FormatBytes(923) == L"923 B");
    CHECK(FormatBytes(55603) == L"54.3 KB");
    CHECK(FormatBytes(28734259) == L"27.4 MB");
    CHECK(FormatBytes(9341000000ull) == L"8.7 GB");
    CHECK(FormatBytes(1374389534720ull) == L"1.25 TB");
    CHECK(FormatCount(428392) == L"428,392");
    CHECK(FormatCount(12) == L"12");
}

static void TestCanonicalize() {
    SECTION("Canonicalize");
    CHECK(path::Canonicalize(L"C:/a//b/./c/../d/") == L"C:\\a\\b\\d");
    CHECK(path::Canonicalize(L"c:\\") == L"C:\\");
    CHECK(path::Canonicalize(L"c:") == L"C:\\");
    CHECK(path::Canonicalize(L"C:\\a\\..\\..\\..") == L"C:\\");
    CHECK(path::Canonicalize(L"\\\\?\\C:\\x\\y") == L"C:\\x\\y");
    CHECK(path::Canonicalize(L"\\\\?\\UNC\\srv\\share\\f") == L"\\\\srv\\share\\f");
    CHECK(path::Canonicalize(L"relative\\path").empty());
    CHECK(path::Canonicalize(L"C:foo").empty());
    CHECK(path::Canonicalize(L"\\Windows").empty());
    CHECK(path::Canonicalize(L"\\\\.\\PhysicalDrive0").empty());
    CHECK(path::Canonicalize(L"C:\\a:stream").empty());
    CHECK(path::Canonicalize(L"C:\\a*").empty());
    CHECK(path::Canonicalize(L"C:\\Windows. ") == L"C:\\Windows");
    SetEnvironmentVariableW(L"SS_TEST_VAR", L"C:\\Foo");
    CHECK(path::Canonicalize(L"%SS_TEST_VAR%\\Bar") == L"C:\\Foo\\Bar");
    CHECK(path::IsSameOrUnder(L"C:\\Windows\\x", L"C:\\Windows"));
    CHECK(path::IsSameOrUnder(L"c:\\WINDOWS", L"C:\\Windows"));
    CHECK(!path::IsSameOrUnder(L"C:\\Windows2\\x", L"C:\\Windows"));
    CHECK(path::IsSameOrUnder(L"C:\\x", L"C:\\"));
}

static void TestProtection() {
    SECTION("PathProtectionService (fixed config, C:\\Users\\John)");
    auto p = FixedService();
    auto del = [&](const wchar_t* s) { return p.CanDelete(s) && p.CanPermanentlyDelete(s); };

    // Critical Windows locations
    for (const wchar_t* s : {L"C:\\Windows", L"C:\\Windows\\System32", L"C:\\Windows\\SysWOW64", L"C:\\Windows\\WinSxS",
                             L"C:\\Windows\\Boot", L"C:\\Windows\\System32\\drivers\\etc\\hosts", L"C:\\Windows\\Temp"}) {
        CHECK(!p.CanDelete(s));
        CHECK(!p.CanPermanentlyDelete(s));
        CHECK(p.IsProtectedPath(s));
    }
    // bypass attempts
    CHECK(!del(L"c:\\WINDOWS\\system32"));
    CHECK(!del(L"C:\\Windows\\System32\\"));
    CHECK(!del(L"C:/Windows/System32"));
    CHECK(!del(L"C:\\Windows\\Temp\\..\\System32"));
    CHECK(!del(L"C:\\Users\\John\\Downloads\\..\\..\\..\\Windows"));
    CHECK(!del(L"C:\\Users\\John\\Downloads\\.\\..\\..\\..\\windows\\system32\\."));
    CHECK(!del(L"\\\\?\\C:\\Windows\\System32"));
    CHECK(!del(L"\\\\?\\C:\\Windows\\..\\Windows\\System32"));
    CHECK(!del(L"C:\\Windows\\System32::$DATA"));
    CHECK(!del(L"C:\\Windows. "));
    CHECK(!del(L"C:\\Windows\\System32\\*"));
    CHECK(!del(L"Windows\\System32"));
    CHECK(!del(L"C:Windows"));
    CHECK(!del(L""));
    {  // %SystemRoot% expansion: only meaningful when the machine's SystemRoot is C:\Windows
        wchar_t sr[MAX_PATH] = L"";
        GetEnvironmentVariableW(L"SystemRoot", sr, MAX_PATH);
        if (_wcsicmp(sr, L"C:\\Windows") == 0) CHECK(!del(L"%SystemRoot%\\System32"));
        else printf("  (skipped %%SystemRoot%% test: SystemRoot=%ls)\n", sr);
    }
    {  // 8.3 short names resolve to the real protected folder
        wchar_t s[MAX_PATH] = L"";
        if (GetShortPathNameW(L"C:\\Program Files", s, MAX_PATH) && _wcsicmp(s, L"C:\\Program Files") != 0)
            CHECK(!del((std::wstring(s) + L"\\Test").c_str()));
    }
    // look-alike prefix must NOT match
    CHECK(del(L"C:\\Windows2\\example.txt"));
    CHECK(del(L"C:\\Windows2"));
    CHECK(del(L"C:\\WindowsOld\\a"));
    CHECK(del(L"C:\\Program Files2\\x"));
    // boot / recovery / system files
    for (const wchar_t* s : {L"C:\\", L"D:\\", L"C:\\pagefile.sys", L"C:\\hiberfil.sys", L"C:\\swapfile.sys",
                             L"C:\\System Volume Information", L"C:\\Recovery", L"C:\\$Recycle.Bin", L"C:\\EFI\\Boot",
                             L"D:\\Boot", L"C:\\bootmgr", L"C:\\$WinREAgent", L"C:\\Users", L"C:\\Users\\John",
                             L"C:\\Users\\Public", L"C:\\Users\\John\\NTUSER.DAT", L"\\\\server\\share\\x"})
        CHECK(!del(s));
    // installed applications: no deletion, but opening/scanning is fine
    for (const wchar_t* s : {L"C:\\Program Files\\Test", L"C:\\Program Files", L"C:\\Program Files (x86)\\App\\a.dll"}) {
        CHECK(!p.CanDelete(s));
        CHECK(!p.CanPermanentlyDelete(s));
        CHECK(p.CanOpen(s));
        CHECK(p.CanScan(s));
        CHECK(p.Classify(s).level == ProtectionLevel::NoDelete);
    }
    CHECK(!p.Classify(L"C:\\Program Files\\Test").reason.empty());
    // user files
    CHECK(del(L"C:\\Users\\John\\Downloads\\movie.mkv"));
    CHECK(del(L"C:\\Users\\John\\Documents"));
    CHECK(del(L"C:\\Users\\John\\Desktop\\x"));
    CHECK(!p.NeedsWarning(L"C:\\Users\\John\\Downloads\\movie.mkv"));
    CHECK(del(L"C:\\Users\\John\\Downloads\\\u0444\u0438\u043b\u044c\u043c_\u65e5\u672c\u8a9e.mkv"));  // Unicode
    CHECK(del(L"D:\\VideoArchive"));
    // AppData: deletable only with warning
    CHECK(del(L"C:\\Users\\John\\AppData\\Local\\SomeApp"));
    CHECK(p.NeedsWarning(L"C:\\Users\\John\\AppData\\Local\\SomeApp"));
    CHECK(p.NeedsWarning(L"C:\\Users\\John\\AppData\\Roaming\\SomeApp"));
    CHECK(p.NeedsWarning(L"C:\\ProgramData\\Vendor"));
    // the data folders themselves are protected; their contents (incl. Temp) stay deletable
    for (const wchar_t* s : {L"C:\\ProgramData", L"C:\\Users\\John\\AppData", L"C:\\Users\\John\\AppData\\Local",
                             L"C:\\Users\\John\\AppData\\Roaming", L"c:\\users\\john\\appdata\\local\\"})
        CHECK(!del(s));
    CHECK(del(L"C:\\Users\\John\\AppData\\Local\\Temp"));
    CHECK(del(L"C:\\Users\\John\\AppData\\Local\\Temp\\old.tmp"));
    CHECK(del(L"C:\\Users\\John\\AppData\\LocalLow\\SomeApp"));
    CHECK(del(L"C:\\Users\\John\\Pictures\\holiday.jpg"));
    CHECK(del(L"C:\\Users\\John\\Downloads"));
    // long paths
    std::wstring deep = L"C:\\Users\\John\\Downloads";
    for (int i = 0; i < 6; ++i) deep += L"\\" + std::wstring(60, L'd');
    CHECK(deep.size() > 300);
    CHECK(del(deep.c_str()));
    std::wstring deepWin = L"C:\\Windows";
    for (int i = 0; i < 6; ++i) deepWin += L"\\" + std::wstring(60, L'd');
    CHECK(!del(deepWin.c_str()));
    CHECK(!del((L"\\\\?\\" + deepWin).c_str()));
}

static void TestReparseProtection(const std::wstring& base) {
    SECTION("Protection vs. junctions");
    PathProtectionService svc(PathProtectionService::ConfigFromEnvironment());
    std::wstring link = base + L"\\winlink";
    if (!MkJunction(link, L"C:\\Windows\\System32")) {
        printf("  (could not create junction; skipped)\n");
        return;
    }
    CHECK(!svc.CanDelete(link));
    CHECK(!svc.CanPermanentlyDelete(link + L"\\drivers"));
    CHECK(svc.IsProtectedPath(link + L"\\drivers\\etc"));
    FileOperationService ops(svc);
    auto r = ops.PermanentlyDelete({link});
    CHECK(r.items[0].error == FileOperationError::ProtectedPath);
    CHECK(Exists(L"C:\\Windows\\System32\\kernel32.dll"));
    RemoveDirectoryW(link.c_str());
}

static void TestScanner(const std::wstring& base) {
    SECTION("DirectoryScanner");
    std::wstring root = base + L"\\scan";
    MkDirs(root + L"\\sub\\deep");
    MkFile(root + L"\\a.bin", 1000);
    MkFile(root + L"\\sub\\b.bin", 5000);
    MkFile(root + L"\\sub\\deep\\c.txt", 10);
    std::wstring uni = root + L"\\\u00fcn\u00ef-\u65e5\u672c\u8a9e";
    MkDirs(uni);
    MkFile(uni + L"\\\u0444\u0430\u0439\u043b.txt", 7);
    std::wstring lng = root + L"\\L";
    for (int i = 0; i < 3; ++i) lng += L"\\" + std::wstring(100, L'l');
    MkDirs(lng);
    CHECK(lng.size() > 300);
    MkFile(lng + L"\\f.txt", 3);
    std::wstring other = base + L"\\other";
    MkDirs(other);
    MkFile(other + L"\\canary.bin", 100000);
    bool junction = MkJunction(root + L"\\jn", other);

    auto res = RunScan({root}, {});
    CHECK(res && !res->canceled);
    if (!res) return;
    ScanNode* r = res->root.get();
    CHECK(r->size == 6020);  // junction target (100000 bytes) must NOT be counted
    CHECK(r->files == 5);
    CHECK(r->dirs == 7);  // sub, deep, unicode dir, L + 3 long-path levels
    CHECK(r->allocated >= r->size);
    ScanNode* sub = Child(r, L"sub");
    CHECK(sub && sub->size == 5010 && sub->files == 2 && sub->dirs == 1);
    CHECK(Child(r, L"\u00fcn\u00ef-\u65e5\u672c\u8a9e") && Child(r, L"\u00fcn\u00ef-\u65e5\u672c\u8a9e")->size == 7);
    CHECK(BuildPath(Child(sub, L"deep")) == root + L"\\sub\\deep");
    ScanNode* l = Child(r, L"L");
    CHECK(l && l->size == 3);  // long path (> MAX_PATH)
    if (junction) {
        ScanNode* jn = Child(r, L"jn");
        CHECK(jn && jn->type == NodeType::Reparse && jn->status == NodeStatus::Junction && jn->size == 0);
        CHECK(res->skippedReparse == 1);
    }
    // removal bookkeeping
    DetachFromTree(Child(r, L"sub"));
    CHECK(r->size == 1010 && r->files == 3 && r->dirs == 5);

    // multi-root => virtual root
    auto multi = RunScan({root, other}, {});
    CHECK(multi && multi->root->type == NodeType::Virtual && multi->root->size == 6020 + 100000);

    // unavailable / inaccessible
    auto gone = RunScan({L"Q:\\definitely\\not\\here"}, {});
    CHECK(gone && gone->root->status == NodeStatus::Unavailable && gone->errors == 1);
    auto svi = RunScan({L"C:\\System Volume Information"}, {});
    printf("  info: 'System Volume Information' status=%d (AccessDenied=1 for non-admin)\n",
           svi ? static_cast<int>(svi->root->status) : -1);
    CHECK(svi && !svi->canceled);  // must not crash; keeps going
}


static void TestReplaceSubtree(const std::wstring& base) {
    SECTION("ReplaceSubtree (Refresh Folder)");
    std::wstring root = base + L"\\refresh";
    MkDirs(root + L"\\sub");
    MkFile(root + L"\\a.bin", 1000);
    MkFile(root + L"\\sub\\b.bin", 5000);
    auto full = RunScan({root}, {});
    ScanNode* r = full->root.get();
    ScanNode* sub = Child(r, L"sub");
    CHECK(sub && r->size == 6000 && r->files == 2 && r->dirs == 1);
    // change the folder on disk: remove b.bin, add c.bin and a new sub-folder with d.bin
    DeleteFileW(Ext(root + L"\\sub\\b.bin").c_str());
    MkFile(root + L"\\sub\\c.bin", 300);
    MkDirs(root + L"\\sub\\deeper");
    MkFile(root + L"\\sub\\deeper\\d.bin", 40);
    auto fresh = RunScan({root + L"\\sub"}, {});
    auto old = ReplaceSubtree(sub, *fresh->root);
    CHECK(old != nullptr);
    CHECK(sub->size == 340 && sub->files == 2 && sub->dirs == 1);
    CHECK(r->size == 1340 && r->files == 3 && r->dirs == 2);  // ancestors corrected without a rescan
    for (auto& c : sub->dir->children) CHECK(c.parent == sub);
    CHECK(BuildPath(Child(sub, L"deeper")) == root + L"\\sub\\deeper");
}

static void TestIgnoreAndSettings(const std::wstring& base) {
    SECTION("Ignore list + settings persistence");
    std::wstring root = base + L"\\ign";
    MkDirs(root + L"\\keep");
    MkDirs(root + L"\\skip\\inner");
    MkDirs(root + L"\\UPPER");
    MkFile(root + L"\\keep\\a.bin", 100);
    MkFile(root + L"\\skip\\b.bin", 5000);
    MkFile(root + L"\\skip\\inner\\c.bin", 700);
    MkFile(root + L"\\UPPER\\d.bin", 30);
    {  // ignored folders are skipped completely (case-insensitive); siblings are untouched
        DirectoryScanner ds;
        std::unique_ptr<ScanResult> out;
        ScanCallbacks cb;
        cb.onFinished = [&](std::unique_ptr<ScanResult> r) { out = std::move(r); };
        ScanOptions o{4};
        o.ignored = {root + L"\\SKIP", root + L"\\upper"};
        ds.Scan({root}, o, cb, {});
        CHECK(out && out->root->size == 100 && out->root->files == 1 && out->root->dirs == 1);
        CHECK(out && Child(out->root.get(), L"skip") == nullptr && Child(out->root.get(), L"UPPER") == nullptr);
        CHECK(out && Child(out->root.get(), L"keep") != nullptr);
    }
    auto all = RunScan({root}, {});  // without the list everything is there
    CHECK(all && all->root->size == 5830 && all->root->dirs == 4);

    // settings round trip, incl. a comma, Unicode and a drive root in the ignore list
    wchar_t oldVal[MAX_PATH] = L"";
    GetEnvironmentVariableW(L"LOCALAPPDATA", oldVal, MAX_PATH);
    std::wstring fake = base + L"\\fakeappdata";
    MkDirs(fake);
    SetEnvironmentVariableW(L"LOCALAPPDATA", fake.c_str());
    Settings s;
    s.theme = 2;
    s.workers = 4;
    s.showSystem = true;
    s.ignored = {L"D:\\", root + L"\\a,b", root + L"\\\u00fcn\u00ef-\u65e5\u672c\u8a9e"};
    s.Save();
    Settings t;
    t.Load();
    SetEnvironmentVariableW(L"LOCALAPPDATA", oldVal);
    CHECK(t.theme == 2 && t.workers == 4 && !t.showSystem);  // showSystem=true in a settings file is ignored: system files are never shown
    CHECK(t.ignored.size() == 3 && t.ignored[0] == L"D:\\" && t.ignored[1] == root + L"\\a,b" &&
          t.ignored[2] == root + L"\\\u00fcn\u00ef-\u65e5\u672c\u8a9e");
}

static void TestTreeEdits(const std::wstring& base) {
    SECTION("Tree edits: DetachMatching / DetachMissing");
    std::wstring root = base + L"\\edits";
    MkDirs(root + L"\\keep\\sub");
    MkDirs(root + L"\\hide\\inner");
    MkFile(root + L"\\keep\\a.bin", 100);
    MkFile(root + L"\\keep\\sub\\b.bin", 20);
    MkFile(root + L"\\hide\\c.bin", 5000);
    MkFile(root + L"\\hide\\inner\\d.bin", 700);
    MkFile(root + L"\\top.bin", 3);
    auto res = RunScan({root}, {});
    ScanNode* r = res->root.get();
    CHECK(r->size == 5823 && r->files == 5 && r->dirs == 4);
    int n = DetachMatching(r, [&](const std::wstring& p) { return path::EqualsNoCase(p, root + L"\\HIDE"); });
    CHECK(n == 1 && r->size == 123 && r->files == 3 && r->dirs == 2);  // "hide" and its "inner" folder are gone from every total
    DeleteFileW(Ext(root + L"\\keep\\a.bin").c_str());  // vanishes on disk; b.bin is untouched
    ScanNode* keep = Child(r, L"keep");
    ScanNode* a = Child(keep, L"a.bin");
    ScanNode* b = Child(Child(keep, L"sub"), L"b.bin");
    int m = DetachMissing({a, b});
    CHECK(m == 1 && a->removed && !b->removed && r->size == 23 && r->files == 2);
}

static void TestTempIgnore(const std::wstring& base) {
    SECTION("TempCleanup honours the ignore list");
    PathProtectionService prot(PathProtectionService::ConfigFromEnvironment());
    std::wstring dir = base + L"\\ignTemp\\Temp";
    MkDirs(dir + L"\\ign");
    MkDirs(dir + L"\\norm");
    MkFile(dir + L"\\ign\\x.tmp", 400);
    MkFile(dir + L"\\norm\\y.tmp", 200);
    for (const wchar_t* p : {L"\\ign\\x.tmp", L"\\ign", L"\\norm\\y.tmp", L"\\norm"}) SetAge(dir + p, 3);
    TempCategory cat{L"t", dir, TempRisk::Low};
    TempCleanup tc(prot, {dir + L"\\ign"});
    auto s = tc.Scan(cat, 24, {});
    CHECK(s.files == 1 && s.bytes == 200);  // the ignored sub-folder is not even counted
    auto c = tc.Clean(cat, 24, {});
    CHECK(c.cleanedFiles == 1 && !c.rejected);
    CHECK(Exists(dir + L"\\ign\\x.tmp") && !Exists(dir + L"\\norm\\y.tmp"));
    TempCleanup inside(prot, {base});  // the whole category lies inside an ignored folder
    CHECK(!inside.IsCategoryValid(dir));
    CHECK(inside.Clean(cat, 24, {}).rejected);
}

static void TestStatsAndHistory(const std::wstring& base) {
    SECTION("FileTypes / ScanStats / History");
    CHECK(Categorize(L"movie.MKV") == FileCategory::Video);
    CHECK(Categorize(L"photo.JPEG") == FileCategory::Picture);
    CHECK(Categorize(L"disk.vdi") == FileCategory::DiskImage);
    CHECK(Categorize(L"setup.exe") == FileCategory::Installer);
    CHECK(Categorize(L"noext") == FileCategory::Other);
    CHECK(Categorize(L".hidden") == FileCategory::Other);
    CHECK(Categorize(L"a.verylongextension") == FileCategory::Other);

    std::wstring root = base + L"\\stats";
    MkDirs(root + L"\\media\\clips");
    MkDirs(root + L"\\docs");
    MkFile(root + L"\\media\\a.mp4", 9000);
    MkFile(root + L"\\media\\clips\\b.mkv", 1000);
    MkFile(root + L"\\media\\pic.jpg", 400);
    MkFile(root + L"\\docs\\r.pdf", 250);
    MkFile(root + L"\\docs\\notes", 50);
    auto res = RunScan({root}, {});
    auto st = ComputeStats(res->root.get(), true, true, 3);
    CHECK(st.bytes[static_cast<int>(FileCategory::Video)] == 10000);
    CHECK(st.bytes[static_cast<int>(FileCategory::Picture)] == 400);
    CHECK(st.bytes[static_cast<int>(FileCategory::Document)] == 250);
    CHECK(st.bytes[static_cast<int>(FileCategory::Other)] == 50);
    CHECK(st.TotalBytes() == 10700);
    CHECK(st.topFiles.size() == 3 && st.topFiles[0].size == 9000 && st.topFiles[1].size == 1000 && st.topFiles[2].size == 400);
    // folders ranked by the files directly inside them: media 9400, clips 1000, docs 300
    CHECK(st.topFolders.size() == 3 && st.topFolders[0].node->name == L"media" && st.topFolders[0].size == 9400 &&
          st.topFolders[1].node->name == L"clips" && st.topFolders[2].node->name == L"docs" && st.topFolders[2].size == 300);
    DetachFromTree(Child(res->root.get(), L"media"));  // removed nodes are not counted
    CHECK(ComputeStats(res->root.get(), true, true).TotalBytes() == 300);

    wchar_t oldVal[MAX_PATH] = L"";
    GetEnvironmentVariableW(L"LOCALAPPDATA", oldVal, MAX_PATH);
    std::wstring fake = base + L"\\histappdata";
    MkDirs(fake);
    SetEnvironmentVariableW(L"LOCALAPPDATA", fake.c_str());
    DriveInfo d;
    d.letter = L'C';
    d.type = DRIVE_FIXED;
    d.ready = true;
    d.total = 1000;
    d.free = 600;
    History::Record({d}, 100);
    d.free = 500;
    History::Record({d}, 100);  // same day again: ignored
    d.free = 450;
    History::Record({d}, 103);
    d.free = 300;
    History::Record({d}, 107);
    int64_t delta = 0;
    int age = 0;
    CHECK(History::Delta(L'C', 108, 250, 7, delta, age) && age == 8 && delta == -350);  // closest to 7 days ago: day 100
    CHECK(!History::Delta(L'D', 108, 250, 7, delta, age));
    CHECK(!History::Delta(L'C', 100, 250, 7, delta, age));  // nothing older than today
    History::Clear();
    CHECK(!History::Delta(L'C', 108, 250, 7, delta, age));
    SetEnvironmentVariableW(L"LOCALAPPDATA", oldVal);
}

// Walks two trees in lock step: same shape, names, sizes, times, attributes, status and aggregated totals.
static bool SameTree(const ScanNode* a, const ScanNode* b) {
    std::vector<std::pair<const ScanNode*, const ScanNode*>> st{{a, b}};
    while (!st.empty()) {
        auto [x, y] = st.back();
        st.pop_back();
        if (x->name != y->name || x->type != y->type || x->status != y->status || x->size != y->size || x->allocated != y->allocated ||
            x->files != y->files || x->dirs != y->dirs || x->attrs != y->attrs || x->scanRoot != y->scanRoot ||
            x->modified / 10000000ull != y->modified / 10000000ull)
            return false;
        size_t nx = x->dir ? x->dir->children.size() : 0, ny = y->dir ? y->dir->children.size() : 0;
        if (nx != ny) return false;
        for (size_t i = 0; i < nx; ++i) {
            if (y->dir->children[i].parent != y) return false;
            st.push_back({&x->dir->children[i], &y->dir->children[i]});
        }
    }
    return true;
}

static void TestSnapshot(const std::wstring& base) {
    SECTION("ScanSnapshot (saved scan)");
    std::wstring root = base + L"\\snap";
    MkDirs(root + L"\\sub\\deep");
    MkDirs(root + L"\\\u00fcn\u00ef-\u65e5\u672c\u8a9e");
    std::wstring lng = root + L"\\L";
    for (int i = 0; i < 3; ++i) lng += L"\\" + std::wstring(100, L'l');
    MkDirs(lng);
    MkFile(root + L"\\a.bin", 1000);
    MkFile(root + L"\\sub\\b.mp4", 5000);
    MkFile(root + L"\\sub\\deep\\c.txt", 10);
    MkFile(root + L"\\\u00fcn\u00ef-\u65e5\u672c\u8a9e\\\u0444\u0430\u0439\u043b.txt", 7);
    MkFile(lng + L"\\f.txt", 3);
    std::wstring other = base + L"\\snapother";
    MkDirs(other);
    MkFile(other + L"\\x.bin", 100000);
    bool jn = MkJunction(root + L"\\jn", other);
    auto scan = RunScan({root}, {});
    DetachFromTree(Child(scan->root.get(), L"sub"));  // removed nodes must not be written
    auto stats = ComputeStats(scan->root.get(), true, true);
    std::wstring file = base + L"\\lastscan.bin";
    CHECK(SaveSnapshot(*scan, {root}, HashIgnored({L"D:\\x"}), stats, file));
    CHECK(!Exists(file + L".tmp"));

    SnapshotSummary sum;
    CHECK(ReadSnapshotSummary(file, sum));
    CHECK(sum.roots.size() == 1 && sum.roots[0] == root && sum.files == scan->root->files && sum.bytes == scan->root->size);
    CHECK(sum.ignoredHash == HashIgnored({L"D:\\x"}) && sum.ignoredHash != HashIgnored({}));
    CHECK(sum.catBytes[static_cast<int>(FileCategory::Other)] == stats.bytes[static_cast<int>(FileCategory::Other)]);
    CHECK(!sum.topFiles.empty() && sum.topFiles[0].size == stats.topFiles[0].size);

    auto loaded = LoadSnapshot(file);
    CHECK(loaded != nullptr);
    if (loaded) {
        // the loaded tree omits the removed "sub" folder, so compare against a fresh scan with it detached and compacted
        CHECK(loaded->root->size == scan->root->size && loaded->root->files == scan->root->files && loaded->root->dirs == scan->root->dirs);
        CHECK(Child(loaded->root.get(), L"sub") == nullptr);
        ScanNode* l = Child(loaded->root.get(), L"L");
        CHECK(l && l->size == 3);
        CHECK(Child(loaded->root.get(), L"\u00fcn\u00ef-\u65e5\u672c\u8a9e") != nullptr);
        if (jn) {
            ScanNode* j = Child(loaded->root.get(), L"jn");
            CHECK(j && j->type == NodeType::Reparse && j->status == NodeStatus::Junction);
        }
        CHECK(BuildPath(Child(loaded->root.get(), L"a.bin")) == root + L"\\a.bin");
    }

    // a complete (nothing removed) tree round-trips exactly
    auto full = RunScan({root}, {});
    CHECK(SaveSnapshot(*full, {root}, 0, ComputeStats(full->root.get(), true, true), file));
    auto again = LoadSnapshot(file);
    CHECK(again && SameTree(full->root.get(), again->root.get()));

    // multi-root (virtual "This PC") also round-trips
    auto multi = RunScan({root, other}, {});
    CHECK(SaveSnapshot(*multi, {root, other}, 0, ComputeStats(multi->root.get(), true, true), file));
    auto m2 = LoadSnapshot(file);
    CHECK(m2 && m2->root->type == NodeType::Virtual && SameTree(multi->root.get(), m2->root.get()));

    // damaged files are rejected, never crash
    std::string bytes;
    {
        HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        DWORD sz = GetFileSize(h, nullptr), got = 0;
        bytes.resize(sz);
        ReadFile(h, bytes.data(), sz, &got, nullptr);
        CloseHandle(h);
    }
    auto writeFile = [&](const std::wstring& p, const std::string& d) {
        HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD w;
        WriteFile(h, d.data(), static_cast<DWORD>(d.size()), &w, nullptr);
        CloseHandle(h);
    };
    std::wstring bad = base + L"\\bad.bin";
    writeFile(bad, bytes.substr(0, bytes.size() / 2));  // truncated
    CHECK(LoadSnapshot(bad) == nullptr);
    std::string corrupt = bytes;
    for (size_t i = corrupt.size() / 2; i < corrupt.size() / 2 + 64 && i < corrupt.size(); ++i) corrupt[i] ^= 0x5A;  // garbled body
    writeFile(bad, corrupt);
    (void)LoadSnapshot(bad);  // may fail or not, must simply not crash
    std::string wrongVersion = bytes;
    wrongVersion[4] = 9;
    writeFile(bad, wrongVersion);
    CHECK(LoadSnapshot(bad) == nullptr && !ReadSnapshotSummary(bad, sum));
    writeFile(bad, "not a snapshot at all");
    CHECK(LoadSnapshot(bad) == nullptr && !ReadSnapshotSummary(bad, sum));
    CHECK(LoadSnapshot(base + L"\\missing.bin") == nullptr);
}

// path (lower case) -> file size, plus the set of folders, ignoring removed nodes: lets us compare a synced tree with a fresh scan.
static void CollectShape(const ScanNode* root, std::map<std::wstring, int64_t>& out) {
    std::vector<std::pair<const ScanNode*, std::wstring>> st{{root, BuildPath(root)}};
    while (!st.empty()) {
        auto [n, p] = st.back();
        st.pop_back();
        if (!n->dir) continue;
        for (const auto& c : n->dir->children) {
            if (c.removed) continue;
            std::wstring cp = path::Join(p, c.name);
            for (auto& ch : cp) ch = static_cast<wchar_t>(towlower(ch));
            out[cp] = c.type == NodeType::File ? static_cast<int64_t>(c.size) : -1;
            if (c.type == NodeType::Directory) st.push_back({&c, path::Join(p, c.name)});
        }
    }
}

// What the app does for a changed folder, repeated for folders that appear on the way.
static void SyncFolder(ScanNode* root, const std::wstring& folder) {
    std::vector<std::wstring> todo{folder};
    uint32_t cluster = ClusterSizeOf(folder);
    while (!todo.empty()) {
        std::wstring p = todo.back();
        todo.pop_back();
        ScanNode* n = FindNodeByPath(root, p);
        if (!n) continue;
        std::vector<DirEntry> listing;
        ListStatus st = ListDirectory(p, cluster, listing);
        auto r = ApplyListing(n, listing, st);
        for (auto& d : r.newDirs) todo.push_back(d);
    }
}

static void TestTreeSync(const std::wstring& base) {
    SECTION("TreeSync: ApplyListing / ValidateDirs / FindNodeByPath");
    std::wstring root = base + L"\\sync";
    for (const wchar_t* d : {L"\\sub\\deeper", L"\\quiet", L"\\gone"}) MkDirs(root + d);
    MkFile(root + L"\\keep.bin", 1000);
    MkFile(root + L"\\grow.bin", 2000);
    MkFile(root + L"\\sub\\a.txt", 30);
    MkFile(root + L"\\sub\\del.bin", 90);
    MkFile(root + L"\\sub\\deeper\\d.bin", 400);
    MkFile(root + L"\\quiet\\q.bin", 50);
    MkFile(root + L"\\gone\\g.bin", 7000);
    for (const wchar_t* d : {L"", L"\\sub", L"\\sub\\deeper", L"\\quiet", L"\\gone"}) SetAge(root + d, 3);  // so later edits change the folder time

    auto res = RunScan({root}, {});
    ScanNode* r = res->root.get();
    CHECK(FindNodeByPath(r, root + L"\\SUB\\deeper") && FindNodeByPath(r, root + L"\\SUB\\deeper")->name == L"deeper");
    CHECK(FindNodeByPath(r, root) == r && FindNodeByPath(r, root + L"\\nope") == nullptr && FindNodeByPath(r, L"Z:\\elsewhere") == nullptr);
    std::vector<DirCheck> checks;
    CollectDirChecks(r, checks);
    CHECK(checks.size() == 5);
    CHECK(ValidateDirs(checks, 2, {}).empty());  // nothing changed: nothing flagged

    MkFile(root + L"\\grow.bin", 9000);  // grows in place (folder time does not change)
    MkFile(root + L"\\new1.bin", 300);
    MkDirs(root + L"\\newdir\\inner");
    MkFile(root + L"\\newdir\\inner\\n.bin", 5555);
    MkFile(root + L"\\newdir\\m.bin", 11);
    DeleteFileW(Ext(root + L"\\sub\\del.bin").c_str());
    DeleteFileW(Ext(root + L"\\gone\\g.bin").c_str());
    RemoveDirectoryW(Ext(root + L"\\gone").c_str());

    auto changed = ValidateDirs(checks, 3, {});
    CHECK(changed.size() == 2);  // the root and "sub"; "gone" no longer exists, "deeper" and "quiet" are untouched
    bool hasRoot = false, hasSub = false;
    for (auto& p : changed) {
        hasRoot = hasRoot || path::EqualsNoCase(p, root);
        hasSub = hasSub || path::EqualsNoCase(p, root + L"\\sub");
    }
    CHECK(hasRoot && hasSub);

    std::vector<DirEntry> listing;
    CHECK(ListDirectory(root, ClusterSizeOf(root), listing) == ListStatus::Ok);
    CHECK(NeedsStructuralChange(r, listing));  // new entries appeared
    for (auto& p : changed) SyncFolder(r, p);
    listing.clear();
    ListDirectory(root, ClusterSizeOf(root), listing);
    CHECK(!NeedsStructuralChange(r, listing));  // now in sync
    CHECK(!ApplyListing(r, listing, ListStatus::Ok).Changed());  // idempotent

    auto fresh = RunScan({root}, {});
    CHECK(r->size == fresh->root->size && r->allocated == fresh->root->allocated && r->files == fresh->root->files && r->dirs == fresh->root->dirs);
    std::map<std::wstring, int64_t> a, b;
    CollectShape(r, a);
    CollectShape(fresh->root.get(), b);
    CHECK(a == b);
    ScanNode* nd = FindNodeByPath(r, root + L"\\newdir\\inner");
    CHECK(nd && nd->size == 5555 && nd->parent->parent == r);  // parent links survived the rebuild

    // multi-root ("This PC") lookup
    std::wstring other = base + L"\\syncother";
    MkDirs(other + L"\\x");
    auto multi = RunScan({root, other}, {});
    CHECK(FindNodeByPath(multi->root.get(), other + L"\\x") != nullptr && FindNodeByPath(multi->root.get(), root + L"\\sub") != nullptr);
}

static bool WaitFor(const std::function<bool()>& cond, int ms) {
    for (int t = 0; t < ms; t += 25) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return cond();
}

static void TestChangeWatcher(const std::wstring& base) {
    SECTION("ChangeWatcher");
    std::wstring dir = base + L"\\watch";
    MkDirs(dir + L"\\a\\b");
    MkDirs(dir + L"\\ignored");
    std::atomic<int> notes{0};
    ChangeWatcher w;
    w.Start({dir}, {dir + L"\\Ignored"}, [&] { ++notes; });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    MkFile(dir + L"\\a\\b\\f.txt", 5);
    MkFile(dir + L"\\ignored\\i.txt", 5);
    CHECK(WaitFor([&] { return w.Pending() > 0; }, 4000));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    auto d = w.TakeDirty(100);
    bool hasB = false, hasIgnored = false;
    for (auto& p : d) {
        hasB = hasB || path::EqualsNoCase(p, dir + L"\\a\\b");
        hasIgnored = hasIgnored || path::EqualsNoCase(p, dir + L"\\ignored");
    }
    CHECK(hasB);          // the folder that holds the new file is reported
    CHECK(!hasIgnored);   // ignored folders never are
    CHECK(notes >= 1 && w.Pending() == 0);
    MkFile(dir + L"\\a\\b\\f.txt", 500);  // growth is reported too
    CHECK(WaitFor([&] { return w.Pending() > 0; }, 4000));
    w.Stop();
    CHECK(!w.Running());

    // a burst far bigger than a tiny buffer: either every folder is tracked or the overflow flag tells the caller to re-check
    std::wstring dir2 = base + L"\\watch2";
    MkDirs(dir2);
    ChangeWatcher w2;
    w2.Start({dir2}, {}, nullptr, 4096);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    for (int i = 0; i < 1500; ++i) MkFile(dir2 + L"\\f" + std::to_wstring(i), 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    CHECK(w2.Pending() > 0 || w2.TakeOverflow());
    w2.Stop();
    ChangeWatcher w3;  // a root that does not exist cannot be promised to be current: reported as overflow, no crash
    w3.Start({base + L"\\no\\such\\root"}, {}, nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(w3.TakeOverflow());
}

// Uses a generated tree, not C:\ : the result must not depend on the machine's disk or load.
static void TestCancel(const std::wstring& base) {
    SECTION("Scan cancellation");
    std::wstring root = base + L"\\cancel";
    for (int d = 0; d < 150; ++d) {
        std::wstring dir = root + L"\\d" + std::to_wstring(d);
        MkDirs(dir);
        for (int f = 0; f < 20; ++f) MkFile(dir + L"\\f" + std::to_wstring(f), 1);
    }
    {  // requested before the scan starts: always reported as canceled; Scan() joins its workers before returning
        std::stop_source ss;
        ss.request_stop();
        auto res = RunScan({root}, ss.get_token(), 8);
        CHECK(res && res->canceled);
    }
    {  // requested while running: caught (canceled) or already finished, but never a hang
        std::stop_source ss;
        std::jthread killer([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            ss.request_stop();
        });
        auto t0 = std::chrono::steady_clock::now();
        auto res = RunScan({root}, ss.get_token(), 8);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(res);
        CHECK(ms < 5000);
        printf("  mid-scan cancel: canceled=%d, returned in %lld ms\n", res ? res->canceled : -1, static_cast<long long>(ms));
    }
    auto full = RunScan({root}, {});  // the same tree still scans correctly afterwards
    CHECK(full && !full->canceled && full->root->files == 3000 && full->root->dirs == 150);
}

static void TestDeletion(const std::wstring& base) {
    SECTION("FileOperationService");
    PathProtectionService prot(PathProtectionService::ConfigFromEnvironment());
    FileOperationService ops(prot);

    for (const wchar_t* s : {L"C:\\Windows", L"C:\\Windows\\System32", L"C:\\Program Files\\Test", L"C:\\", L"C:\\Users"}) {
        auto a = ops.PermanentlyDelete({s});
        auto b = ops.MoveToRecycleBin({s});
        CHECK(a.items[0].error == FileOperationError::ProtectedPath);
        CHECK(b.items[0].error == FileOperationError::ProtectedPath);
    }
    CHECK(Exists(L"C:\\Windows\\System32"));

    auto nf = ops.PermanentlyDelete({base + L"\\does\\not\\exist"});
    CHECK(nf.items[0].error == FileOperationError::PathNotFound);

    // permanent delete of a tree containing a junction: junction removed, target survives
    std::wstring victim = base + L"\\victim", keep = base + L"\\keep";
    MkDirs(victim + L"\\sub");
    MkDirs(keep);
    MkFile(keep + L"\\canary.txt", 50);
    MkFile(victim + L"\\f.txt", 100);
    MkFile(victim + L"\\sub\\g.txt", 200);
    SetFileAttributesW(Ext(victim + L"\\sub\\g.txt").c_str(), FILE_ATTRIBUTE_READONLY);
    bool jn = MkJunction(victim + L"\\jn", keep);
    auto r = ops.PermanentlyDelete({victim});
    CHECK(r.items[0].error == FileOperationError::None);
    CHECK(!Exists(victim));
    CHECK(r.items[0].bytes == 300);
    if (jn) CHECK(Exists(keep + L"\\canary.txt"));

    // recycle bin (leaves one tiny file in the user's Recycle Bin)
    std::wstring rf = base + L"\\recycle_me.txt";
    MkFile(rf, 10);
    auto rb = ops.MoveToRecycleBin({rf});
    CHECK(rb.items[0].error == FileOperationError::None);
    CHECK(!Exists(rf));
    printf("  recycle error code (0 = ok): %d\n", static_cast<int>(rb.items[0].error));

    // the Recycle Bin listing finds that file; purging it removes exactly that one (and cleans up after this test)
    {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        RecycleBin bin;
        bin.Reload();
        std::vector<size_t> mine;  // only the file this run recycled (name + this run's private temp folder); nothing else is touched
        for (size_t i = 0; i < bin.items().size(); ++i) {
            const BinItem& b = bin.items()[i];
            if (b.name == L"recycle_me.txt" && path::EqualsNoCase(path::Canonicalize(b.from), path::Canonicalize(base))) mine.push_back(i);
        }
        CHECK(mine.size() == 1);
        if (mine.size() == 1) {
            CHECK(bin.items()[mine[0]].size == 10 && !bin.items()[mine[0]].isDir && bin.items()[mine[0]].deleted != 0);
            size_t total = bin.items().size();
            CHECK(bin.Purge(mine) == 1);
            CHECK(bin.items().size() == total - 1);
        }
    }

    // locked file => FileInUse, nothing crashes
    std::wstring lf = base + L"\\locked.txt";
    MkFile(lf, 10);
    HANDLE h = CreateFileW(Ext(lf).c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    auto lk = ops.PermanentlyDelete({lf});
    CHECK(lk.items[0].error == FileOperationError::FileInUse);
    CloseHandle(h);
}

static void TestTemp(const std::wstring& base) {
    SECTION("TempCleanup");
    PathProtectionService prot(PathProtectionService::ConfigFromEnvironment());
    TempCleanup tc(prot);
    std::wstring dir = base + L"\\controlled\\Temp";
    MkDirs(dir + L"\\sub");
    MkFile(dir + L"\\old.tmp", 1000);
    MkFile(dir + L"\\new.tmp", 500);
    MkFile(dir + L"\\sub\\old2.tmp", 2000);
    MkFile(dir + L"\\locked.tmp", 300);
    SetAge(dir + L"\\old.tmp", 3);
    SetAge(dir + L"\\sub\\old2.tmp", 3);
    SetAge(dir + L"\\sub", 3);
    SetAge(dir + L"\\locked.tmp", 3);
    HANDLE lockH = CreateFileW(Ext(dir + L"\\locked.tmp").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);

    TempCategory cat{L"test", dir, TempRisk::Low};
    CHECK(tc.IsCategoryValid(dir));
    auto s = tc.Scan(cat, 24, {});
    CHECK(s.files == 3 && s.bytes == 3300);
    auto s2 = tc.Scan(cat, 24 * 30, {});
    CHECK(s2.files == 0);  // nothing is 30 days old
    auto c = tc.Clean(cat, 24, {});
    CHECK(!c.rejected);
    CHECK(c.cleanedFiles == 2 && c.cleanedBytes == 3000);
    CHECK(c.inUseFiles == 1);
    CHECK(c.skippedFiles == 2);  // new.tmp (too recent) + locked.tmp
    CHECK(!Exists(dir + L"\\old.tmp"));
    CHECK(!Exists(dir + L"\\sub"));
    CHECK(Exists(dir + L"\\new.tmp"));
    CHECK(Exists(dir + L"\\locked.tmp"));
    if (lockH != INVALID_HANDLE_VALUE) CloseHandle(lockH);

    // validation refuses anything that is not a known temp location
    CHECK(!tc.IsCategoryValid(L"C:\\Windows\\Temp"));
    CHECK(!tc.IsCategoryValid(L"C:\\"));
    CHECK(!tc.IsCategoryValid(base));  // leaf is not Temp/Tmp/CrashDumps
    CHECK(!tc.IsCategoryValid(L"C:\\Users"));
    TempCategory bad{L"bad", L"C:\\Windows\\Temp", TempRisk::Low};
    CHECK(tc.Clean(bad, 0, {}).rejected);
    TempCategory docs{L"docs", base, TempRisk::Low};
    CHECK(tc.Clean(docs, 0, {}).rejected);
    auto cats = tc.DiscoverCategories();
    printf("  discovered %zu temp categories on this machine\n", cats.size());
}

int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS);
    TestFormat();
    TestCanonicalize();
    TestProtection();  // safety rules are verified before any deletion test runs
    if (g_fail) {
        printf("\nSAFETY TESTS FAILED (%d) - aborting before deletion tests\n", g_fail);
        return 1;
    }
    std::wstring base = TempBase();
    TestReparseProtection(base);
    TestScanner(base);
    TestDeletion(base);
    TestTemp(base);
    TestReplaceSubtree(base);
    TestIgnoreAndSettings(base);
    TestTreeEdits(base);
    TestTempIgnore(base);
    TestStatsAndHistory(base);
    TestSnapshot(base);
    TestTreeSync(base);
    TestChangeWatcher(base);
    TestCancel(base);
    // cleanup of the private test directory (our own safe deleter)
    PathProtectionService prot(PathProtectionService::ConfigFromEnvironment());
    FileOperationService(prot).PermanentlyDelete({base});
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
