#pragma once
#include <cstdarg>
#include <mutex>
#include <string>

namespace ss {

// Tiny rotating log. Max 1 MB per file, 5 files (storagescope.log + .1 .. .4) => 5 MB total.
// No-op until Init() is called (so unit tests do not write logs).
// Policy: never log full user file names; log counts, sizes and error codes.
class Logger {
public:
    static Logger& Get();
    void Init(const std::wstring& dir);
    const std::wstring& Dir() const { return dir_; }
    void Info(const wchar_t* fmt, ...);
    void Error(const wchar_t* fmt, ...);

    static constexpr unsigned long long kMaxFileBytes = 1024ull * 1024ull;
    static constexpr int kFiles = 5;

private:
    void Write(const wchar_t* level, const wchar_t* fmt, va_list ap);
    void Rotate();
    std::mutex m_;
    std::wstring dir_;
};

}  // namespace ss
