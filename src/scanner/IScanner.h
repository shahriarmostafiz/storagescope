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
    uint64_t allocated = 0;  // size on disk found so far
    uint64_t expected = 0;   // used space of the scanned drives (x scale below); 0 = unknown (folder scans)
    bool hdd = false;        // a spinning disk is being scanned: gentle mode (one thread per disk)
    std::wstring currentPath;
    uint64_t elapsedMs = 0;
};

struct ScanOptions {
    unsigned workers = 0;  // 0 = choose automatically
    std::vector<std::wstring> ignored;  // folders that are skipped completely: no node, not counted in any total
    uint32_t testListDelayUs = 0;       // tests only: pretend every folder listing takes this long (a slow disk)
    // Tests / simulation only. Called on the worker thread before each folder listing (kind 0) and before each exact-size file open
    // (kind 1): lets a test model a disk (one head, seeks) with real or virtual time.
    std::function<void(int kind, uint32_t root)> testDiskHook;  // root = index of the scan root (each root is its own disk in a model)
    bool testDisableGuards = false;     // simulation only: the behaviour before the slow-disk protections (no adaptation, no lookup budget)
    bool testAssumeHdd = false;         // simulation only: treat every root as a known hard drive
    double expectedScale = 1.0;         // how much of a drive's used space past scans actually found (system areas are not files)
};

struct ScanCallbacks {
    // The (still empty) result object, once, before any worker runs. It stays valid until onFinished hands it over, and may
    // be browsed meanwhile through KidsOf(); sizes of unfinished folders grow as they are discovered.
    std::function<void(ScanResult*)> onStarted;
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
