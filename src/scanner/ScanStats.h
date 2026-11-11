#pragma once
#include <cstdint>
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

// One pass over the in-memory tree (no disk access). Removed nodes are skipped; hidden/system nodes are skipped
// (with their contents) unless allowed.
ScanStats ComputeStats(ScanNode* root, bool includeHidden, bool includeSystem, size_t topN = 10);

}  // namespace ss
