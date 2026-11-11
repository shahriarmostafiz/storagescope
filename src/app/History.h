#pragma once
#include <cstdint>
#include <vector>

#include "filesystem/DriveManager.h"

namespace ss {

// A few numbers per drive per day (date, free bytes, total bytes) in %LOCALAPPDATA%\StorageScope\history.csv,
// used for "free space vs. a week ago". No file names, no scan data. Capped at 1000 lines.
class History {
public:
    static int Today();                                              // day number (days since 1601-01-01)
    static void Record(const std::vector<DriveInfo>& drives, int today);  // at most one sample per drive per day
    // Free-space change of `drive` since the sample whose age is closest to `wantDays` (older than today).
    static bool Delta(wchar_t drive, int today, uint64_t currentFree, int wantDays, int64_t& delta, int& ageDays);
    static void Clear();
};

}  // namespace ss
