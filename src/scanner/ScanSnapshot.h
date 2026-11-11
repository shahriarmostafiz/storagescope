#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "scanner/FileTypes.h"
#include "scanner/ScanNode.h"
#include "scanner/ScanStats.h"

namespace ss {

// A remembered scan on disk. File layout: small uncompressed header (totals, roots, a stats summary for the Dashboard)
// followed by the tree as pre-order records in independently compressed blocks (Windows Compression API, XPRESS_HUFF).
// Directory totals are NOT stored: they are recomputed on load so the data can never be inconsistent.
struct SavedTop {
    std::wstring name, location;
    uint64_t size = 0;
};

struct SnapshotSummary {
    uint64_t created = 0;  // FILETIME
    uint64_t files = 0, dirs = 0, bytes = 0, elapsedMs = 0, nodes = 0, ignoredHash = 0, fileBytes = 0;
    std::vector<std::wstring> roots;
    uint64_t catBytes[kCategoryCount]{}, catFiles[kCategoryCount]{};
    std::vector<SavedTop> topFolders, topFiles;
};

uint64_t HashIgnored(const std::vector<std::wstring>& ignored);

// Writes to `<path>.tmp` then renames, so a crash never leaves a half-written file. `stats` feeds the header summary.
bool SaveSnapshot(const ScanResult& scan, const std::vector<std::wstring>& roots, uint64_t ignoredHash, const ScanStats& stats,
                  const std::wstring& path);
// Header only (fast): false if the file is missing, damaged or from another format version.
bool ReadSnapshotSummary(const std::wstring& path, SnapshotSummary& out);
// Whole tree. The file is treated as untrusted: every count/length is bounded; returns null on any problem.
std::unique_ptr<ScanResult> LoadSnapshot(const std::wstring& path);

}  // namespace ss
