#include "operations/ExplorerService.h"

#include <shellapi.h>
#include <shlobj.h>

#include "filesystem/PathUtils.h"

namespace ss::explorer {

static bool Exec(const wchar_t* verb, const std::wstring& file, ULONG mask = 0, HWND owner = nullptr) {
    SHELLEXECUTEINFOW sei{sizeof sei};
    sei.fMask = mask | SEE_MASK_NOASYNC;
    sei.hwnd = owner;
    sei.lpVerb = verb;
    sei.lpFile = file.c_str();
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&sei) != 0;
}

bool OpenFolder(const std::wstring& dir) { return Exec(L"open", dir); }

bool OpenPathNoSelect(const std::wstring& path) { return Exec(L"open", path); }

bool Reveal(const std::wstring& path, bool isDir) {
    if (isDir) return OpenFolder(path);
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str());  // may fail for >MAX_PATH; fall back to the parent folder
    if (pidl) {
        HRESULT hr = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ILFree(pidl);
        if (SUCCEEDED(hr)) return true;
    }
    return OpenFolder(ss::path::Parent(path));
}

bool IsRiskyToLaunch(const std::wstring& p) {
    size_t dot = p.rfind(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = p.substr(dot);
    for (const wchar_t* e : {L".exe", L".com", L".bat", L".cmd", L".msi", L".msix", L".appx", L".ps1", L".vbs", L".vbe",
                             L".js", L".jse", L".wsf", L".scr", L".lnk", L".reg", L".dll", L".jar", L".hta", L".cpl", L".pif"})
        if (ss::path::EqualsNoCase(ext, e)) return true;
    return false;
}

bool OpenItem(const std::wstring& path, bool isDir) {
    if (isDir) return OpenFolder(path);
    if (IsRiskyToLaunch(path)) return Reveal(path, false);
    return Exec(L"open", path);
}

bool ShowProperties(HWND owner, const std::wstring& path) {
    return Exec(L"properties", path, SEE_MASK_INVOKEIDLIST, owner);
}

bool CopyText(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool ok = false;
    if (g) {
        if (void* p = GlobalLock(g)) {
            memcpy(p, text.c_str(), bytes);
            GlobalUnlock(g);
            ok = SetClipboardData(CF_UNICODETEXT, g) != nullptr;
        }
        if (!ok) GlobalFree(g);  // ownership passes to the clipboard on success
    }
    CloseClipboard();
    return ok;
}

bool OpenInstalledApps() { return Exec(L"open", L"ms-settings:appsfeatures"); }

}  // namespace ss::explorer
