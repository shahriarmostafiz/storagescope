#pragma once
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

#include "scanner/ScanNode.h"

namespace ss {

struct ScanProgress {
    uint64_t files = 0, dirs = 0, bytes = 0, errors = 0;
    std::wstring currentPath;
    uint64_t elapsedMs = 0;
};

struct ScanOptions {
    unsigned workers = 0;  // 0 = choose automatically
    std::vector<std::wstring> ignored;  // folders that are skipped completely: no node, not counted in any total
};

struct ScanCallbacks {
    std::function<void(const ScanProgress&)> onProgress;             // throttled (~10/s), called on the Scan() thread
    std::function<void(std::unique_ptr<ScanResult>)> onFinished;     // exactly once (also when canceled)
};

// Implementations: DirectoryScanner (v1). A future NtfsMftScanner can implement this too
// (FSCTL_ENUM_USN_DATA / MFT parsing) without touching the UI.
class IScanner {
public:
    virtual ~IScanner() = default;
    // Blocks until finished or stop requested. Several roots => virtual "This PC" root.
    virtual void Scan(const std::vector<std::wstring>& roots, const ScanOptions& options, ScanCallbacks callbacks,
                      std::stop_token stop) = 0;
};

}  // namespace ss
