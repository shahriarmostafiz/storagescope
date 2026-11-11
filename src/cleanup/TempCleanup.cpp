#include "cleanup/TempCleanup.h"

#include <windows.h>

#include <memory>

#include "filesystem/PathUtils.h"
#include "utils/Logger.h"

namespace ss {
namespace {

constexpr int kMaxDepth = 128;

uint64_t Ft(const FILETIME& f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

struct Ctx {
    bool doDelete;
    uint64_t cutoff;  // files with write+creation time <= cutoff are eligible
    std::stop_token st;
    TempScanResult scan;
    TempCleanResult clean;
    const std::vector<std::wstring>* ignored = nullptr;  // user ignore list (canonical paths)
};

bool Walk(const std::wstring& dir, Ctx& c, int depth) {
    auto fd = std::make_unique<WIN32_FIND_DATAW>();
    std::wstring base = dir + L"\\";
    HANDLE h = FindFirstFileExW((base + L"*").c_str(), FindExInfoBasic, fd.get(), FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (c.st.stop_requested()) break;
        const wchar_t* nm = fd->cFileName;
        if (nm[0] == L'.' && (nm[1] == 0 || (nm[1] == L'.' && nm[2] == 0))) continue;
        std::wstring child = base + nm;
        DWORD a = fd->dwFileAttributes;
        bool old = Ft(fd->ftLastWriteTime) <= c.cutoff && Ft(fd->ftCreationTime) <= c.cutoff;
        if (a & FILE_ATTRIBUTE_DIRECTORY) {
            if (a & FILE_ATTRIBUTE_REPARSE_POINT) continue;  // never followed, never removed
            if (depth >= kMaxDepth) continue;
            if (c.ignored) {  // a folder the user put on the ignore list is left completely alone
                std::wstring plain = path::StripExtendedPrefix(child);
                bool skip = false;
                for (const auto& ig : *c.ignored) skip = skip || path::EqualsNoCase(plain, ig);
                if (skip) continue;
            }
            Walk(child, c, depth + 1);
            if (c.doDelete && old) RemoveDirectoryW(child.c_str());  // succeeds only if empty
            continue;
        }
        uint64_t sz = (static_cast<uint64_t>(fd->nFileSizeHigh) << 32) | fd->nFileSizeLow;
        if (!old) {
            if (c.doDelete) {
                ++c.clean.skippedFiles;
                c.clean.skippedBytes += sz;
            }
            continue;
        }
        if (!c.doDelete) {
            ++c.scan.files;
            c.scan.bytes += sz;
            continue;
        }
        if (a & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))
            SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (DeleteFileW(child.c_str())) {
            ++c.clean.cleanedFiles;
            c.clean.cleanedBytes += sz;
        } else {
            DWORD e = GetLastError();
            ++c.clean.skippedFiles;
            c.clean.skippedBytes += sz;
            if (e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION) ++c.clean.inUseFiles;
        }
    } while (FindNextFileW(h, fd.get()));
    FindClose(h);
    return true;
}

uint64_t Cutoff(uint32_t hours) {
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    uint64_t span = hours ? static_cast<uint64_t>(hours) * 3600ull : 60ull;  // always keep the last minute
    return Ft(now) - span * 10000000ull;
}

std::wstring EnvVar(const wchar_t* n) {
    DWORD len = GetEnvironmentVariableW(n, nullptr, 0);
    if (!len) return L"";
    std::wstring v(len, L'\0');
    len = GetEnvironmentVariableW(n, v.data(), len);
    v.resize(len);
    return v;
}

}  // namespace

bool TempCleanup::IsCategoryValid(const std::wstring& dir) const {
    std::wstring c = path::Canonicalize(dir);
    if (c.empty() || path::IsDriveRoot(c)) return false;
    for (const auto& ig : ignored_)
        if (path::IsSameOrUnder(c, ig)) return false;  // the whole category is inside an ignored folder
    std::wstring leaf = path::Leaf(c);
    if (!path::EqualsNoCase(leaf, L"Temp") && !path::EqualsNoCase(leaf, L"Tmp") && !path::EqualsNoCase(leaf, L"CrashDumps"))
        return false;
    DWORD a = GetFileAttributesW(path::ToExtended(c).c_str());
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY) || (a & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    return prot_.Classify(c).level <= ProtectionLevel::Caution;  // %TEMP% lives in AppData => Caution is expected
}

std::vector<TempCategory> TempCleanup::DiscoverCategories() const {
    std::vector<TempCategory> out;
    auto add = [&](const wchar_t* name, const std::wstring& raw, TempRisk risk) {
        std::wstring c = path::Canonicalize(raw);
        if (c.empty() || !IsCategoryValid(c)) return;
        for (auto& e : out)
            if (path::EqualsNoCase(e.path, c)) return;
        out.push_back({name, c, risk});
    };
    add(L"User temporary files", EnvVar(L"TEMP"), TempRisk::Low);
    std::wstring local = EnvVar(L"LOCALAPPDATA");
    if (!local.empty()) {
        add(L"Application temporary files", path::Join(local, L"Temp"), TempRisk::Low);
        add(L"Crash dumps", path::Join(local, L"CrashDumps"), TempRisk::Low);
    }
    return out;
}

TempScanResult TempCleanup::Scan(const TempCategory& cat, uint32_t hours, std::stop_token st) const {
    if (!IsCategoryValid(cat.path)) return {};
    Ctx c{false, Cutoff(hours), st, {}, {}, &ignored_};
    Walk(path::ToExtended(path::Canonicalize(cat.path)), c, 0);
    return c.scan;
}

TempCleanResult TempCleanup::Clean(const TempCategory& cat, uint32_t hours, std::stop_token st) const {
    if (!IsCategoryValid(cat.path)) {  // re-validated right before deleting
        TempCleanResult r;
        r.rejected = true;
        Logger::Get().Error(L"temp cleanup rejected: category failed validation");
        return r;
    }
    Ctx c{true, Cutoff(hours), st, {}, {}, &ignored_};
    Walk(path::ToExtended(path::Canonicalize(cat.path)), c, 0);
    Logger::Get().Info(L"temp cleanup: cleaned=%llu files (%llu bytes) skipped=%llu files (%llu in use)",
                       c.clean.cleanedFiles, c.clean.cleanedBytes, c.clean.skippedFiles, c.clean.inUseFiles);
    return c.clean;
}

}  // namespace ss
