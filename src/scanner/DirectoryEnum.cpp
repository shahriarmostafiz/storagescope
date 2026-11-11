#include "scanner/DirectoryEnum.h"

#include <memory>

#include "filesystem/PathUtils.h"

namespace ss {

uint32_t ClusterSizeOf(const std::wstring& root) {
    wchar_t vol[MAX_PATH + 1];
    DWORD spc = 0, bps = 0, f = 0, t = 0;
    if (GetVolumePathNameW(root.c_str(), vol, MAX_PATH) && GetDiskFreeSpaceW(vol, &spc, &bps, &f, &t) && spc && bps) return spc * bps;
    return 4096;
}

uint64_t AllocatedSizeOf(uint32_t a, uint64_t size, uint32_t cluster, const std::wstring& dirPrefix, const wchar_t* name) {
    if (size == 0 || (a & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS))) return 0;
    if (a & (FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_SPARSE_FILE)) {
        DWORD hi = 0;
        DWORD lo = GetCompressedFileSizeW((dirPrefix + name).c_str(), &hi);
        if (lo != INVALID_FILE_SIZE || GetLastError() == NO_ERROR) return (static_cast<uint64_t>(hi) << 32) | lo;
    }
    return (size + cluster - 1) / cluster * cluster;
}

ListStatus ListDirectory(const std::wstring& dir, uint32_t cluster, std::vector<DirEntry>& out) {
    out.clear();
    std::wstring base = path::ToExtended(dir);
    if (base.back() != L'\\') base += L'\\';
    auto fd = std::make_unique<WIN32_FIND_DATAW>();
    HANDLE h = FindFirstFileExW((base + L"*").c_str(), FindExInfoBasic, fd.get(), FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND) return ListStatus::Ok;  // empty volume root
        return (e == ERROR_ACCESS_DENIED || e == ERROR_PRIVILEGE_NOT_HELD || e == ERROR_SHARING_VIOLATION) ? ListStatus::AccessDenied
                                                                                                           : ListStatus::Unavailable;
    }
    do {
        const wchar_t* nm = fd->cFileName;
        if (nm[0] == L'.' && (nm[1] == 0 || (nm[1] == L'.' && nm[2] == 0))) continue;
        DirEntry e;
        e.name = nm;
        e.attrs = fd->dwFileAttributes;
        e.modified = (static_cast<uint64_t>(fd->ftLastWriteTime.dwHighDateTime) << 32) | fd->ftLastWriteTime.dwLowDateTime;
        if (e.IsDir()) {
            e.reparseTag = fd->dwReserved0;
        } else {
            e.size = (static_cast<uint64_t>(fd->nFileSizeHigh) << 32) | fd->nFileSizeLow;
            e.allocated = AllocatedSizeOf(e.attrs, e.size, cluster, base, nm);
        }
        out.push_back(std::move(e));
    } while (FindNextFileW(h, fd.get()));
    DWORD le = GetLastError();
    FindClose(h);
    return le == ERROR_NO_MORE_FILES ? ListStatus::Ok : ListStatus::Unavailable;
}

void RemoveIgnoredDirs(const std::wstring& dir, const std::vector<std::wstring>& ignored, std::vector<DirEntry>& list) {
    if (ignored.empty()) return;
    std::erase_if(list, [&](const DirEntry& e) {
        if (!e.IsDir()) return false;
        std::wstring cp = path::Join(dir, e.name);
        for (const auto& ig : ignored)
            if (path::EqualsNoCase(cp, ig)) return true;
        return false;
    });
}

}  // namespace ss
