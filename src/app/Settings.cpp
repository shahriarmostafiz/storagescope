#include "app/Settings.h"

#include <windows.h>

#include <string>

#include "filesystem/PathUtils.h"

namespace ss {

std::wstring Settings::AppDataDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring d = (n && n < MAX_PATH) ? std::wstring(buf) : L".";
    d += L"\\StorageScope";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

namespace {

std::string ReadAll(const std::wstring& f) {
    HANDLE h = CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER sz{};
    std::string s;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart <= (1 << 20)) {  // a settings file is tiny; refuse anything over 1 MB
        s.resize(static_cast<size_t>(sz.QuadPart));
        DWORD got = 0;
        if (ReadFile(h, s.data(), static_cast<DWORD>(s.size()), &got, nullptr)) s.resize(got);
        else s.clear();
    }
    CloseHandle(h);
    return s;
}

// Finds  "key": value  and returns the raw value text ("" if missing). Scalars only.
std::string Raw(const std::string& j, const char* key) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = j.find(k);
    if (p == std::string::npos) return {};
    p = j.find(':', p + k.size());
    if (p == std::string::npos) return {};
    ++p;
    while (p < j.size() && (j[p] == ' ' || j[p] == '\t')) ++p;
    size_t e = j.find_first_of(",}\r\n", p);
    return j.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

// String value with \\ and \" escapes (paths may contain commas, so Raw() is not usable for them).
bool ReadString(const std::string& j, const char* key, std::string& out) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = j.find(k);
    if (p == std::string::npos) return false;
    p = j.find(':', p + k.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < j.size() && (j[p] == ' ' || j[p] == '\t')) ++p;
    if (p >= j.size() || j[p] != '"') return false;
    ++p;
    while (p < j.size() && j[p] != '"') {
        if (j[p] == '\\' && p + 1 < j.size()) {
            out += j[p + 1];
            p += 2;
        } else {
            out += j[p++];
        }
    }
    return true;
}

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

}  // namespace

void Settings::Load() {
    std::string j = ReadAll(AppDataDir() + L"\\settings.json");
    if (j.empty()) return;
    auto b = [&](const char* k, bool& v) {
        std::string r = Raw(j, k);
        if (r.rfind("true", 0) == 0) v = true;
        else if (r.rfind("false", 0) == 0) v = false;
    };
    auto u = [&](const char* k, uint32_t& v, uint32_t lo, uint32_t hi) {
        std::string r = Raw(j, k);
        if (r.empty()) return;
        unsigned long x = strtoul(r.c_str(), nullptr, 10);
        if (x >= lo && x <= hi) v = static_cast<uint32_t>(x);
    };
    b("showHidden", showHidden);
    // showSystem is deliberately never read: system files are always hidden (an old settings file cannot turn them on)
    b("askBeforeRecycle", askBeforeRecycle);
    b("requireSecondPermanentConfirm", requireSecondPermanentConfirm);
    b("keepHistory", keepHistory);
    b("rememberScan", rememberScan);
    b("autoUpdate", autoUpdate);
    b("firstRunDone", firstRunDone);
    {
        std::string win;  // "x,y,w,h,maximized,page"
        int x, y, w, h, mx, pg;
        if (ReadString(j, "window", win) && sscanf_s(win.c_str(), "%d,%d,%d,%d,%d,%d", &x, &y, &w, &h, &mx, &pg) == 6 && w >= 300 && h >= 200) {
            winX = x;
            winY = y;
            winW = w;
            winH = h;
            maximized = mx != 0;
            lastPage = pg >= 0 && pg < 7 ? pg : 0;
        }
    }
    u("lastFullScanDay", lastFullScanDay, 0, 0xFFFFFFFFu);
    u("tempAgeHours", tempAgeHours, 24, 720);
    u("workers", workers, 0, 16);
    u("theme", theme, 0, 2);
    if (tempAgeHours != 24 && tempAgeHours != 168 && tempAgeHours != 720) tempAgeHours = 24;

    std::string ig;  // "path|path|..." ('|' cannot occur in a Windows path)
    if (ReadString(j, "ignored", ig)) {
        ignored.clear();
        std::wstring all = FromUtf8(ig);
        size_t pos = 0;
        while (pos <= all.size()) {
            size_t e = all.find(L'|', pos);
            std::wstring c = path::Canonicalize(all.substr(pos, e == std::wstring::npos ? std::wstring::npos : e - pos));
            if (!c.empty()) ignored.push_back(c);
            if (e == std::wstring::npos) break;
            pos = e + 1;
        }
    }
}

void Settings::Save() const {
    char buf[512];
    sprintf_s(buf,
              "{\n  \"showHidden\": %s,\n  \"showSystem\": %s,\n  \"askBeforeRecycle\": %s,\n"
              "  \"requireSecondPermanentConfirm\": %s,\n  \"tempAgeHours\": %u,\n  \"workers\": %u,\n  \"theme\": %u,\n",
              showHidden ? "true" : "false", showSystem ? "true" : "false", askBeforeRecycle ? "true" : "false",
              requireSecondPermanentConfirm ? "true" : "false", tempAgeHours, workers, theme);
    std::wstring joined;
    for (const auto& p : ignored) joined += (joined.empty() ? L"" : L"|") + p;
    std::string esc;
    for (char c : ToUtf8(joined)) {
        if (c == '\\' || c == '"') esc += '\\';
        esc += c;
    }
    char extra[400];
    sprintf_s(extra,
              "  \"keepHistory\": %s,\n  \"rememberScan\": %s,\n  \"autoUpdate\": %s,\n  \"firstRunDone\": %s,\n  \"lastFullScanDay\": %u,\n"
              "  \"window\": \"%d,%d,%d,%d,%d,%d\",\n",
              keepHistory ? "true" : "false", rememberScan ? "true" : "false", autoUpdate ? "true" : "false", firstRunDone ? "true" : "false",
              lastFullScanDay, winX, winY, winW, winH, maximized ? 1 : 0, lastPage);
    std::string j = std::string(buf) + extra + "  \"ignored\": \"" + esc + "\"\n}\n";

    std::wstring f = AppDataDir() + L"\\settings.json", tmp = f + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    bool ok = WriteFile(h, j.data(), static_cast<DWORD>(j.size()), &w, nullptr) && w == static_cast<DWORD>(j.size());
    CloseHandle(h);
    // write-then-rename: a crash mid-write can never leave an empty settings file
    if (ok) MoveFileExW(tmp.c_str(), f.c_str(), MOVEFILE_REPLACE_EXISTING);
    else DeleteFileW(tmp.c_str());
}

}  // namespace ss
