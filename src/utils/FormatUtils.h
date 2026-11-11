#pragma once
#include <cstdint>
#include <string>

namespace ss {

// 923 B, 54.3 KB, 27.4 MB, 8.7 GB, 1.25 TB (binary units, 1024-based).
std::wstring FormatBytes(uint64_t bytes);
std::wstring FormatCount(uint64_t n);          // 428,392
std::wstring FormatFileTime(uint64_t ft);      // FILETIME (UTC) -> local short date/time; "" when 0
std::wstring FormatDuration(uint64_t ms);      // hh:mm:ss
std::wstring FormatAge(uint64_t ft);           // "just now", "5 min ago", "3 h ago", "2 days ago" (FILETIME in the past)

}  // namespace ss
