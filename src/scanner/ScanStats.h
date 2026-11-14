#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "scanner/FileTypes.h"
#include "scanner/ScanNode.h"

namespace ss {

struct TopItem {
    ScanNode* node = nullptr;
    uint64_t size = 0;  // files: file size; folders: size of the files directly inside (not sub-folders)
};

struct ScanStats {
    uint64_t bytes[kCategoryCount]{};  // logical bytes per file category
    uint64_t files[kCategoryCount]{};
    std::vector<TopItem> topFolders;   // biggest folders by their OWN files, largest first
    std::vector<TopItem> topFiles;     // biggest files, largest first
    uint64_t TotalBytes() const {
        uint64_t t = 0;
        for (uint64_t b : bytes) t += b;
        return t;
    }
};

// View-only summary of the hidden app data: total bytes and the biggest app folders (children of AppData\Local, \Roaming,
// \LocalLow and ProgramData), each labelled with where it lives.
struct AppDataItem {
    const ScanNode* node = nullptr;
    uint64_t size = 0;
    std::wstring label;  // "Docker (Local)"
};
struct AppDataInfo {
    uint64_t total = 0;
    std::vector<AppDataItem> top;  // largest first
};
AppDataInfo ComputeAppData(const ScanNode* root, size_t topN = 5);

// One pass over the in-memory tree (no disk access). Removed nodes are skipped; hidden/system nodes are skipped
// (with their contents) unless allowed; app-data folders (see IsAppDataRoot) likewise.
ScanStats ComputeStats(ScanNode* root, bool includeHidden, bool includeSystem, size_t topN = 10, bool includeAppData = true);

}  // namespace ss
