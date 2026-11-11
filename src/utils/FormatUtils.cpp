#include "utils/FormatUtils.h"

#include <windows.h>

#include <cstdio>

namespace ss {

std::wstring FormatBytes(uint64_t bytes) {
    if (bytes < 1024) return std::to_wstring(bytes) + L" B";
    static const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB", L"PB"};
    double v = static_cast<double>(bytes);
    int i = 0;
    while (v >= 1024.0 && i < 5) {
        v /= 1024.0;
        ++i;
    }
    wchar_t buf[48];
    swprintf_s(buf, i >= 4 ? L"%.2f %s" : L"%.1f %s", v, units[i]);
    return buf;
}

std::wstring FormatCount(uint64_t n) {
    std::wstring s = std::to_wstring(n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), L",");
    return s;
}

std::wstring FormatFileTime(uint64_t ft) {
    if (ft == 0) return L"";
    FILETIME f{static_cast<DWORD>(ft), static_cast<DWORD>(ft >> 32)}, local;
    SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(&f, &local) || !FileTimeToSystemTime(&local, &st)) return L"";
    wchar_t d[64] = L"", t[64] = L"";
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &st, nullptr, d, 64, nullptr);
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, t, 64);
    return std::wstring(d) + L" " + t;
}

std::wstring FormatAge(uint64_t ft) {
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    uint64_t n = (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    uint64_t s = n > ft ? (n - ft) / 10000000ull : 0;
    if (s < 60) return L"just now";
    if (s < 3600) return std::to_wstring(s / 60) + L" min ago";
    if (s < 86400) return std::to_wstring(s / 3600) + L" h ago";
    uint64_t d = s / 86400;
    return std::to_wstring(d) + (d == 1 ? L" day ago" : L" days ago");
}

std::wstring FormatDuration(uint64_t ms) {
    uint64_t s = ms / 1000;
    wchar_t buf[32];
    swprintf_s(buf, L"%02llu:%02llu:%02llu", s / 3600, (s / 60) % 60, s % 60);
    return buf;
}

}  // namespace ss
