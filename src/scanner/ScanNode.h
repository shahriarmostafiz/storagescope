#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ss {

enum class NodeType : uint8_t { File, Directory, Reparse, Virtual };
enum class NodeStatus : uint8_t { Ok, AccessDenied, Unavailable, Junction, Symlink, ReparseOther };

struct DirData;

// One entry of the scan tree. Full paths are NOT stored: only the leaf name + parent pointer
// (scan roots store their full path). Directory-only data lives in DirData (files carry 8 bytes).
struct ScanNode {
    std::wstring name;
    ScanNode* parent = nullptr;
    uint64_t size = 0;       // logical bytes (aggregated for directories)
    uint64_t allocated = 0;  // approx. size on disk
    uint64_t modified = 0;   // FILETIME
    std::unique_ptr<DirData> dir;
    uint32_t attrs = 0;
    uint32_t files = 0;  // directories: files in subtree
    uint32_t dirs = 0;   // directories: folders in subtree
    NodeType type = NodeType::File;
    NodeStatus status = NodeStatus::Ok;
    bool removed = false;   // deleted after the scan; hidden from views
    bool scanRoot = false;  // `name` is a full path

    ScanNode();
    ~ScanNode();
    ScanNode(ScanNode&&) noexcept;
    ScanNode& operator=(ScanNode&&) noexcept;
    bool IsDir() const { return type == NodeType::Directory || type == NodeType::Virtual; }
};

struct DirData {
    std::vector<ScanNode> children;  // fixed after the directory has been enumerated
    // Scanner bookkeeping (valid during the scan only)
    std::atomic<uint32_t> pending{0};
    std::atomic<uint64_t> size{0}, allocated{0};
    std::atomic<uint32_t> files{0}, dirs{0};
};

struct ScanResult {
    std::unique_ptr<ScanNode> root;  // Virtual "This PC" for multi-root scans, else the scan root
    uint64_t files = 0, dirs = 0, bytes = 0;
    uint64_t errors = 0;           // directories that could not be read
    uint64_t skippedReparse = 0;   // directory reparse points not followed
    uint64_t elapsedMs = 0;
    bool canceled = false;
};

std::wstring BuildPath(const ScanNode* n);     // "" for the virtual root
std::wstring DisplayName(const ScanNode* n);   // "C:" for drive roots
const wchar_t* StatusText(const ScanNode* n);
// App data = "X:\ProgramData" and "X:\Users\<name>\AppData". Hidden by default (Settings > Scan > "Show app data").
bool IsAppDataPath(const std::wstring& path);
bool IsAppDataRoot(const ScanNode* n);  // a folder node that is one of those two
// Bytes held by app-data folders at or below `n`. Looks only where they can be (drive roots, Users, profiles), so it is cheap.
uint64_t AppDataBytes(const ScanNode* n);
// The app-data folders at or below `n` (same limited search).
void FindAppDataRoots(const ScanNode* n, std::vector<const ScanNode*>& out);
// Marks a node removed and subtracts its totals from every ancestor.
void DetachFromTree(ScanNode* n);
// Detaches every directory below `root` whose full path satisfies `isMatch` (not descending into matches); returns how many.
int DetachMatching(ScanNode* root, const std::function<bool(const std::wstring&)>& isMatch);
// Detaches the nodes whose file/folder no longer exists on disk; returns how many.
int DetachMissing(const std::vector<ScanNode*>& nodes);
// Adds (or, if negative, subtracts) to the totals of `start` and every ancestor above it; never wraps below zero.
void AdjustAncestors(ScanNode* start, int64_t dSize, int64_t dAlloc, int64_t dFiles, int64_t dDirs);
// The node for `path` (case-insensitive), or null if it is not in the tree (or was removed).
ScanNode* FindNodeByPath(ScanNode* root, const std::wstring& path);
// Replaces everything below `target` with the freshly scanned `fresh` (a scan root of the same folder), re-parents the
// new children and corrects every ancestor's totals. Returns the old contents so the caller can free them off-thread.
std::unique_ptr<DirData> ReplaceSubtree(ScanNode* target, ScanNode& fresh);

}  // namespace ss
