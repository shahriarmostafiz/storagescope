#pragma once
#include <cstdint>
#include <stop_token>
#include <string>
#include <vector>

#include "safety/PathProtectionService.h"

namespace ss {

enum class TempRisk { Low };

struct TempCategory {
    std::wstring name;   // "User temporary files"
    std::wstring path;   // canonical directory
    TempRisk risk = TempRisk::Low;
};

struct TempScanResult {
    uint64_t files = 0, bytes = 0;
};

struct TempCleanResult {
    uint64_t cleanedFiles = 0, cleanedBytes = 0;
    uint64_t skippedFiles = 0, skippedBytes = 0;
    uint64_t inUseFiles = 0;
    bool rejected = false;  // category failed the safety validation; nothing was touched
};

// Conservative temp cleaner. Only operates on directories that pass IsCategoryValid(), never
// follows reparse points, skips anything it cannot delete and keeps going.
class TempCleanup {
public:
    // `ignored`: the user's ignore list. A category inside an ignored folder is rejected; ignored sub-folders are never touched.
    explicit TempCleanup(const PathProtectionService& p, std::vector<std::wstring> ignored = {}) : prot_(p), ignored_(std::move(ignored)) {}

    // %TEMP%, %LOCALAPPDATA%\Temp (if different), %LOCALAPPDATA%\CrashDumps. Only valid, existing ones.
    std::vector<TempCategory> DiscoverCategories() const;
    // Leaf must be Temp/Tmp/CrashDumps, not a reparse point, not a drive root, not Critical/NoDelete.
    bool IsCategoryValid(const std::wstring& dir) const;

    // minAgeHours == 0 means "all eligible" (files touched in the last minute are still always kept).
    TempScanResult Scan(const TempCategory& c, uint32_t minAgeHours, std::stop_token st) const;
    TempCleanResult Clean(const TempCategory& c, uint32_t minAgeHours, std::stop_token st) const;

private:
    const PathProtectionService& prot_;
    std::vector<std::wstring> ignored_;
};

}  // namespace ss
