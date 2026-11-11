#include "operations/FileOperationService.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <memory>

#include "filesystem/PathUtils.h"
#include "utils/Logger.h"

namespace ss {

FileOperationError MapWin32Error(DWORD e) {
    switch (e) {
        case ERROR_SUCCESS: return FileOperationError::None;
        case ERROR_ACCESS_DENIED: return FileOperationError::AccessDenied;
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION: return FileOperationError::FileInUse;
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_NAME: return FileOperationError::PathNotFound;
        case ERROR_NOT_READY:
        case ERROR_BAD_UNIT:
        case ERROR_DEV_NOT_EXIST:
        case ERROR_BAD_NETPATH:
        case ERROR_UNEXP_NET_ERR:
        case ERROR_NO_SUCH_DEVICE:
        case ERROR_DEVICE_NOT_CONNECTED: return FileOperationError::DriveUnavailable;
        default: return FileOperationError::Unknown;
    }
}

const wchar_t* ErrorText(FileOperationError e) {
    switch (e) {
        case FileOperationError::None: return L"succeeded";
        case FileOperationError::AccessDenied: return L"access was denied";
        case FileOperationError::FileInUse: return L"they are currently in use";
        case FileOperationError::PathNotFound: return L"they no longer exist";
        case FileOperationError::ProtectedPath: return L"they are protected by Windows or by StorageScope's safety rules";
        case FileOperationError::DriveUnavailable: return L"the drive is not available";
        case FileOperationError::Canceled: return L"the operation was canceled";
        default: return L"of an unexpected error (see the log)";
    }
}

size_t DeleteResult::Succeeded() const {
    size_t n = 0;
    for (auto& i : items)
        if (i.error == FileOperationError::None) ++n;
    return n;
}
uint64_t DeleteResult::BytesRemoved() const {
    uint64_t n = 0;
    for (auto& i : items) n += i.bytes;
    return n;
}
size_t DeleteResult::CountOf(FileOperationError e) const {
    size_t n = 0;
    for (auto& i : items)
        if (i.error == e) ++n;
    return n;
}

namespace {

struct ComScope {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ~ComScope() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

struct TreeStats {
    uint64_t bytes = 0;
    DWORD firstError = 0;
    bool failed = false;
    void Fail() {
        failed = true;
        if (!firstError) firstError = GetLastError();
    }
};

// Never follows reparse points: junctions/symlinks are unlinked, their targets are untouched.
void DeleteTree(const std::wstring& dir, TreeStats& st) {
    auto fd = std::make_unique<WIN32_FIND_DATAW>();
    std::wstring base = dir + L"\\";
    HANDLE h = FindFirstFileExW((base + L"*").c_str(), FindExInfoBasic, fd.get(), FindExSearchNameMatch, nullptr, 0);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const wchar_t* nm = fd->cFileName;
            if (nm[0] == L'.' && (nm[1] == 0 || (nm[1] == L'.' && nm[2] == 0))) continue;
            std::wstring child = base + nm;
            DWORD a = fd->dwFileAttributes;
            if (a & FILE_ATTRIBUTE_DIRECTORY) {
                if (a & FILE_ATTRIBUTE_REPARSE_POINT) {
                    SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                    if (!RemoveDirectoryW(child.c_str())) st.Fail();
                } else {
                    DeleteTree(child, st);
                }
            } else {
                uint64_t sz = (static_cast<uint64_t>(fd->nFileSizeHigh) << 32) | fd->nFileSizeLow;
                if (a & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                if (DeleteFileW(child.c_str())) st.bytes += sz;
                else st.Fail();
            }
        } while (FindNextFileW(h, fd.get()));
        FindClose(h);
    } else if (GetLastError() != ERROR_FILE_NOT_FOUND) {
        st.Fail();
    }
    SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!RemoveDirectoryW(dir.c_str())) st.Fail();
}

ItemResult RecycleOne(const std::wstring& canonical, HWND owner) {
    ItemResult r;
    r.path = canonical;
    std::wstring p = canonical.size() >= 240 ? path::ToExtended(canonical) : canonical;

    IFileOperation* op = nullptr;
    IShellItem* item = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op));
    if (SUCCEEDED(hr)) {
        // No FOF_NOCONFIRMATION: if Windows cannot recycle the item (too big, bin disabled) it
        // asks the user instead of silently deleting it permanently.
        op->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOERRORUI | FOFX_ADDUNDORECORD | FOFX_RECYCLEONDELETE);
        if (owner) op->SetOwnerWindow(owner);
        hr = SHCreateItemFromParsingName(p.c_str(), nullptr, IID_PPV_ARGS(&item));
    }
    if (SUCCEEDED(hr)) hr = op->DeleteItem(item, nullptr);
    if (SUCCEEDED(hr)) hr = op->PerformOperations();
    BOOL aborted = FALSE;
    if (op) op->GetAnyOperationsAborted(&aborted);
    if (item) item->Release();
    if (op) op->Release();

    DWORD attr = GetFileAttributesW(path::ToExtended(canonical).c_str());
    DWORD le = GetLastError();
    if (attr == INVALID_FILE_ATTRIBUTES && (le == ERROR_FILE_NOT_FOUND || le == ERROR_PATH_NOT_FOUND)) return r;  // gone

    if (aborted || hr == E_ABORT) {
        r.error = FileOperationError::Canceled;
    } else {
        r.win32 = HRESULT_FACILITY(hr) == FACILITY_WIN32 ? HRESULT_CODE(hr) : static_cast<DWORD>(hr);
        r.error = MapWin32Error(r.win32);
        if (r.error == FileOperationError::None) r.error = FileOperationError::Unknown;
    }
    return r;
}

}  // namespace

FileOperationError FileOperationService::Validate(const std::wstring& p, bool permanent, std::wstring& canonical,
                                                  DWORD& win32) const {
    canonical = path::Canonicalize(p);                      // layer 3: canonical form
    if (canonical.empty()) {
        win32 = ERROR_INVALID_NAME;
        return FileOperationError::ProtectedPath;
    }
    bool ok = permanent ? prot_.CanPermanentlyDelete(canonical) : prot_.CanDelete(canonical);  // layers 2 + 4
    if (!ok) {
        win32 = ERROR_ACCESS_DENIED;
        return FileOperationError::ProtectedPath;
    }
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path::ToExtended(canonical).c_str(), GetFileExInfoStandard, &fa)) {
        win32 = GetLastError();
        FileOperationError e = MapWin32Error(win32);
        if (e == FileOperationError::PathNotFound && GetFileAttributesW(canonical.substr(0, 3).c_str()) == INVALID_FILE_ATTRIBUTES)
            e = FileOperationError::DriveUnavailable;
        return e == FileOperationError::None ? FileOperationError::Unknown : e;
    }
    return FileOperationError::None;
}

DeleteResult FileOperationService::MoveToRecycleBin(const std::vector<std::wstring>& paths, HWND owner) const {
    DeleteResult res;
    ComScope com;
    for (const auto& p : paths) {
        std::wstring canon;
        ItemResult r;
        r.path = p;
        r.error = Validate(p, false, canon, r.win32);
        if (r.error == FileOperationError::None) r = RecycleOne(canon, owner);
        res.items.push_back(std::move(r));
    }
    Logger::Get().Info(L"delete(recycle): requested=%zu ok=%zu failed=%zu", paths.size(), res.Succeeded(), res.Failed());
    for (auto& i : res.items)
        if (i.error != FileOperationError::None)
            Logger::Get().Error(L"recycle failed: error=%d win32=%lu", static_cast<int>(i.error), i.win32);
    return res;
}

DeleteResult FileOperationService::PermanentlyDelete(const std::vector<std::wstring>& paths) const {
    DeleteResult res;
    for (const auto& p : paths) {
        std::wstring canon;
        ItemResult r;
        r.path = p;
        r.error = Validate(p, true, canon, r.win32);
        if (r.error == FileOperationError::None) {
            r.path = canon;
            std::wstring ext = path::ToExtended(canon);
            DWORD a = GetFileAttributesW(ext.c_str());  // re-read right before acting
            if (a == INVALID_FILE_ATTRIBUTES) {
                r.win32 = GetLastError();
                r.error = MapWin32Error(r.win32);
            } else if ((a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT)) {
                TreeStats st;
                DeleteTree(ext, st);
                r.bytes = st.bytes;
                if (st.failed) {
                    r.win32 = st.firstError;
                    r.error = MapWin32Error(st.firstError);
                    if (r.error == FileOperationError::None) r.error = FileOperationError::Unknown;
                }
            } else {
                BOOL ok;
                SetFileAttributesW(ext.c_str(), FILE_ATTRIBUTE_NORMAL);
                if (a & FILE_ATTRIBUTE_DIRECTORY) ok = RemoveDirectoryW(ext.c_str());  // junction/symlink: unlink only
                else ok = DeleteFileW(ext.c_str());
                if (!ok) {
                    r.win32 = GetLastError();
                    r.error = MapWin32Error(r.win32);
                    if (r.error == FileOperationError::None) r.error = FileOperationError::Unknown;
                }
            }
        }
        res.items.push_back(std::move(r));
    }
    Logger::Get().Info(L"delete(permanent): requested=%zu ok=%zu failed=%zu bytes=%llu", paths.size(), res.Succeeded(),
                       res.Failed(), res.BytesRemoved());
    for (auto& i : res.items)
        if (i.error != FileOperationError::None)
            Logger::Get().Error(L"permanent delete failed: error=%d win32=%lu", static_cast<int>(i.error), i.win32);
    return res;
}

}  // namespace ss
