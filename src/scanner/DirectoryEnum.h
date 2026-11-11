#pragma once
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ss {

struct DirEntry {
    std::wstring name;
    uint32_t attrs = 0;
    uint64_t size = 0, allocated = 0, modified = 0;  // modified = FILETIME
    uint32_t reparseTag = 0;
    bool IsDir() const { return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0; }
    bool IsReparseDir() const { return IsDir() && (attrs & FILE_ATTRIBUTE_REPARSE_POINT); }
};

enum class ListStatus { Ok, AccessDenied, Unavailable };

// Cluster size of the volume holding `anyPath` (4096 if unknown): used for the "size on disk" estimate.
uint32_t ClusterSizeOf(const std::wstring& anyPath);
// Size on disk estimate: cloud placeholders/empty = 0, compressed/sparse = real value, otherwise rounded up to the cluster size.
// `dirPrefix` is the extended directory path ending in '\\' (needed only for compressed/sparse files).
uint64_t AllocatedSizeOf(uint32_t attrs, uint64_t size, uint32_t cluster, const std::wstring& dirPrefix, const wchar_t* name);

// One-level listing of a directory (canonical path). Thread-safe; touches nothing but the disk.
ListStatus ListDirectory(const std::wstring& canonicalDir, uint32_t cluster, std::vector<DirEntry>& out);

}  // namespace ss
