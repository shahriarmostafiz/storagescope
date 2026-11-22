#pragma once
#include <atomic>
#include <cstdint>
#include <stop_token>
#include <string>
#include <vector>

namespace ss {

struct DupInput {
    std::wstring path;  // canonical full path
    uint64_t size = 0;
    uint64_t modified = 0;  // FILETIME
};

// Live progress, read by the UI while FindDuplicateGroups runs on a worker thread.
struct DupProgress {
    std::atomic<int> stage{0};  // 1 = quick check (start and end of each file), 2 = full comparison
    std::atomic<uint64_t> done{0}, total{0};  // files handled / to handle in the current stage
};

// Finds files with identical content among `in` (same size first, then a hash of the start and end of the file, then a hash of
// the whole file: only files that look alike are ever read in full). Returns groups of indexes into `in`; every group has two or
// more files and its first entry is the one to keep (oldest modified time, then the shortest path). Files that cannot be
// read are left out. `threads` > 1 only makes sense on solid-state storage: on a hard disk one reader is faster.
// Returns an empty list when `st` is requested (a half-finished comparison is not shown).
std::vector<std::vector<uint32_t>> FindDuplicateGroups(const std::vector<DupInput>& in, int threads, std::stop_token st,
                                                      DupProgress* prog = nullptr);

}  // namespace ss
