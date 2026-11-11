#pragma once
#include "scanner/IScanner.h"

namespace ss {

// Native FindFirstFileExW/FindNextFileW scanner with a fixed pool of worker threads.
// Sizes are aggregated bottom-up during the one and only traversal (no re-scan for parents).
class DirectoryScanner : public IScanner {
public:
    void Scan(const std::vector<std::wstring>& roots, const ScanOptions& options, ScanCallbacks callbacks,
              std::stop_token stop) override;
};

}  // namespace ss
