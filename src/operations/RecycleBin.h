#pragma once
#include <windows.h>
#include <shobjidl.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ss {

struct BinItem {
    std::wstring name, from;  // display name, folder it was deleted from
    uint64_t size = 0, deleted = 0;  // bytes, FILETIME deleted
    bool isDir = false;
    IShellItem* item = nullptr;  // owned reference (released by RecycleBin)
};

// The Windows Recycle Bin of all drives, through the shell (the same data Explorer shows). Needs COM on the calling thread.
class RecycleBin {
public:
    ~RecycleBin() { Clear(); }
    void Reload();  // re-reads the bin
    const std::vector<BinItem>& items() const { return items_; }
    // Permanently deletes the items at `idx` (cannot be undone). Returns how many are really gone afterwards.
    size_t Purge(const std::vector<size_t>& idx);
    // Puts the items at `idx` back where they were deleted from (Windows asks about name conflicts itself).
    size_t Restore(const std::vector<size_t>& idx, HWND owner);

private:
    void Clear();
    std::vector<BinItem> items_;
};

}  // namespace ss
