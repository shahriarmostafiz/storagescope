#include "app/History.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "app/Settings.h"

namespace ss {
namespace {

struct Row {
    int day;
    wchar_t drive;
    uint64_t freeB, total;
};

std::wstring FilePath() { return Settings::AppDataDir() + L"\\history.csv"; }

std::vector<Row> Load() {
    std::vector<Row> rows;
    HANDLE h = CreateFileW(FilePath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return rows;
    LARGE_INTEGER sz{};
    std::string s;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart <= (1 << 20)) {
        s.resize(static_cast<size_t>(sz.QuadPart));
        DWORD got = 0;
        if (ReadFile(h, s.data(), static_cast<DWORD>(s.size()), &got, nullptr)) s.resize(got);
        else s.clear();
    }
    CloseHandle(h);
    size_t pos = 0;
    while (pos < s.size()) {
        size_t e = s.find('\n', pos);
        std::string line = s.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        pos = e == std::string::npos ? s.size() : e + 1;
        int day = 0;
        char drv = 0;
        unsigned long long f = 0, t = 0;
        if (sscanf_s(line.c_str(), "%d,%c,%llu,%llu", &day, &drv, 1, &f, &t) == 4)
            rows.push_back({day, static_cast<wchar_t>(drv), f, t});
    }
    return rows;
}

void Save(const std::vector<Row>& rows) {
    std::string s;
    char buf[96];
    for (const auto& r : rows) {
        sprintf_s(buf, "%d,%c,%llu,%llu\n", r.day, static_cast<char>(r.drive), r.freeB, r.total);
        s += buf;
    }
    std::wstring f = FilePath(), tmp = f + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    bool ok = WriteFile(h, s.data(), static_cast<DWORD>(s.size()), &w, nullptr) && w == s.size();
    CloseHandle(h);
    if (ok) MoveFileExW(tmp.c_str(), f.c_str(), MOVEFILE_REPLACE_EXISTING);
    else DeleteFileW(tmp.c_str());
}

}  // namespace

int History::Today() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return static_cast<int>(t / (864000000000ull));  // 100 ns ticks per day
}

void History::Record(const std::vector<DriveInfo>& drives, int today) {
    auto rows = Load();
    bool changed = false;
    for (const auto& d : drives) {
        if (!d.IsFixed() || !d.ready || d.total == 0) continue;
        bool have = false;
        for (const auto& r : rows) have = have || (r.day == today && r.drive == d.letter);
        if (!have) {
            rows.push_back({today, d.letter, d.free, d.total});
            changed = true;
        }
    }
    if (!changed) return;
    if (rows.size() > 1000) rows.erase(rows.begin(), rows.begin() + static_cast<long long>(rows.size() - 1000));
    Save(rows);
}

bool History::Delta(wchar_t drive, int today, uint64_t currentFree, int wantDays, int64_t& delta, int& ageDays) {
    const Row* best = nullptr;
    int bestGap = 1 << 30;
    auto rows = Load();
    for (const auto& r : rows) {
        int age = today - r.day;
        if (r.drive != drive || age < 1) continue;
        int gap = std::abs(age - wantDays);
        if (gap < bestGap) {
            bestGap = gap;
            best = &r;
        }
    }
    if (!best) return false;
    delta = static_cast<int64_t>(currentFree) - static_cast<int64_t>(best->freeB);
    ageDays = today - best->day;
    return true;
}

void History::Clear() { DeleteFileW(FilePath().c_str()); }

}  // namespace ss
