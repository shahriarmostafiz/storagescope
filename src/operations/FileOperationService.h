#pragma once
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "safety/PathProtectionService.h"

namespace ss {

enum class FileOperationError { None, AccessDenied, FileInUse, PathNotFound, ProtectedPath, DriveUnavailable, Canceled, Unknown };

struct ItemResult {
    std::wstring path;
    FileOperationError error = FileOperationError::None;
    DWORD win32 = 0;          // technical code, for the log only
    uint64_t bytes = 0;       // bytes removed (permanent delete only)
};

struct DeleteResult {
    std::vector<ItemResult> items;  // same order as the request
    size_t Succeeded() const;
    size_t Failed() const { return items.size() - Succeeded(); }
    uint64_t BytesRemoved() const;
    size_t CountOf(FileOperationError e) const;
};

const wchar_t* ErrorText(FileOperationError e);  // short, user-readable reason

// All deletions go through here. Every call re-canonicalises each path, checks it still exists
// and asks PathProtectionService again right before touching the file system (layers 2-4),
// regardless of what the UI believed.
class FileOperationService {
public:
    explicit FileOperationService(const PathProtectionService& protection) : prot_(protection) {}

    DeleteResult MoveToRecycleBin(const std::vector<std::wstring>& paths, HWND owner = nullptr) const;
    DeleteResult PermanentlyDelete(const std::vector<std::wstring>& paths) const;

private:
    FileOperationError Validate(const std::wstring& path, bool permanent, std::wstring& canonical, DWORD& win32) const;
    const PathProtectionService& prot_;
};

FileOperationError MapWin32Error(DWORD e);

}  // namespace ss
