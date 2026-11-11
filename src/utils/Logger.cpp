#include "utils/Logger.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace ss {

Logger& Logger::Get() {
    static Logger l;
    return l;
}

void Logger::Init(const std::wstring& dir) {
    std::lock_guard lk(m_);
    dir_ = dir;
    CreateDirectoryW(dir.c_str(), nullptr);  // parent (StorageScope) must already exist
}

void Logger::Info(const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    Write(L"INFO ", fmt, ap);
    va_end(ap);
}

void Logger::Error(const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    Write(L"ERROR", fmt, ap);
    va_end(ap);
}

void Logger::Rotate() {
    auto name = [&](int i) { return dir_ + (i == 0 ? L"\\storagescope.log" : L"\\storagescope.log." + std::to_wstring(i)); };
    DeleteFileW(name(kFiles - 1).c_str());
    for (int i = kFiles - 2; i >= 0; --i) MoveFileExW(name(i).c_str(), name(i + 1).c_str(), MOVEFILE_REPLACE_EXISTING);
}

void Logger::Write(const wchar_t* level, const wchar_t* fmt, va_list ap) {
    std::lock_guard lk(m_);
    if (dir_.empty()) return;
    wchar_t msg[1024];
    vswprintf_s(msg, fmt, ap);
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t line[1200];
    swprintf_s(line, L"%04d-%02d-%02d %02d:%02d:%02d %s %s\r\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
               st.wSecond, level, msg);
    char utf8[4800];
    int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof utf8, nullptr, nullptr);
    if (n <= 1) return;
    --n;

    std::wstring file = dir_ + L"\\storagescope.log";
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &fa)) {
        unsigned long long size = (static_cast<unsigned long long>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
        if (size + static_cast<unsigned long long>(n) > kMaxFileBytes) Rotate();
    }
    HANDLE h = CreateFileW(file.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, utf8, static_cast<DWORD>(n), &w, nullptr);
    CloseHandle(h);
}

}  // namespace ss
