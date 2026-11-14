#include "safety/PathProtectionService.h"

#include <windows.h>

#include "filesystem/PathUtils.h"

namespace ss {
namespace {

const wchar_t* kWindows = L"Protected by Windows";
const wchar_t* kInstalledApps = L"Installed applications should normally be removed through Windows Settings.";
const wchar_t* kDataRoot = L"This folder holds the data of every application and cannot be deleted. You can delete items inside it.";
const wchar_t* kAppData =L"May contain important application data. Only delete what you recognize.";

// Names directly under any drive root that are boot/recovery/system files.
const wchar_t* kRootNames[] = {L"System Volume Information", L"Recovery",     L"EFI",          L"Boot",
                               L"bootmgr",                    L"BOOTNXT",      L"BOOTSECT.BAK", L"pagefile.sys",
                               L"hiberfil.sys",               L"swapfile.sys", L"Config.Msi",   L"Documents and Settings"};

std::wstring Env(const wchar_t* name) {
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (!n) return L"";
    std::wstring v(n, L'\0');
    n = GetEnvironmentVariableW(name, v.data(), n);
    v.resize(n);
    return v;
}

std::wstring Canon(const std::wstring& s) { return s.empty() ? L"" : path::Canonicalize(s); }

}  // namespace

PathProtectionService::Config PathProtectionService::ConfigFromEnvironment() {
    Config c;
    c.systemRoot = Env(L"SystemRoot");
    if (c.systemRoot.empty()) {
        wchar_t buf[MAX_PATH];
        if (GetWindowsDirectoryW(buf, MAX_PATH)) c.systemRoot = buf;
    }
    for (const wchar_t* v : {L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramW6432"}) {
        std::wstring p = Env(v);
        if (!p.empty()) c.programDirs.push_back(p);
    }
    std::wstring drive = Env(L"SystemDrive");
    if (drive.empty()) drive = L"C:";
    c.usersRoot = drive + L"\\Users";
    c.programData = Env(L"ProgramData");
    c.userProfile = Env(L"USERPROFILE");
    c.appData = Env(L"APPDATA");
    c.localAppData = Env(L"LOCALAPPDATA");
    return c;
}

PathProtectionService::PathProtectionService(Config cfg) : cfg_(std::move(cfg)) {
    cfg_.systemRoot = Canon(cfg_.systemRoot);
    for (auto& p : cfg_.programDirs) p = Canon(p);
    cfg_.usersRoot = Canon(cfg_.usersRoot);
    cfg_.programData = Canon(cfg_.programData);
    cfg_.userProfile = Canon(cfg_.userProfile);
    cfg_.appData = Canon(cfg_.appData);
    cfg_.localAppData = Canon(cfg_.localAppData);
}

ProtectionInfo PathProtectionService::ClassifyCanonical(const std::wstring& c) const {
    using L = ProtectionLevel;
    if (c.empty()) return {L::Critical, L"Invalid or unsupported path"};
    if (c[0] == L'\\') return {L::Critical, L"Network locations are not managed by this application"};
    if (path::IsDriveRoot(c)) return {L::Critical, L"A drive cannot be deleted"};
    if (path::IsSameOrUnder(c, cfg_.systemRoot)) return {L::Critical, kWindows};

    // First component below the drive root.
    size_t e = c.find(L'\\', 3);
    std::wstring first = c.substr(3, e == std::wstring::npos ? std::wstring::npos : e - 3);
    if (!first.empty() && first[0] == L'$') return {L::Critical, kWindows};  // $Recycle.Bin, $MFT, $WinREAgent ...
    for (const wchar_t* n : kRootNames)
        if (path::EqualsNoCase(first, n)) return {L::Critical, kWindows};

    for (const auto& pd : cfg_.programDirs)
        if (path::IsSameOrUnder(c, pd)) return {L::NoDelete, kInstalledApps};

    if (!cfg_.usersRoot.empty()) {
        if (path::EqualsNoCase(c, cfg_.usersRoot)) return {L::Critical, L"The Users folder is protected"};
        if (path::EqualsNoCase(path::Parent(c), cfg_.usersRoot)) return {L::Critical, L"User profile folders are protected"};
    }
    // The data folders themselves are never deletable (one click would wipe every app's settings);
    // what is inside them is "Caution" below.
    if (path::EqualsNoCase(c, cfg_.appData) || path::EqualsNoCase(c, cfg_.localAppData) || path::EqualsNoCase(c, cfg_.programData) ||
        (!cfg_.userProfile.empty() && path::EqualsNoCase(c, path::Join(cfg_.userProfile, L"AppData"))))
        return {L::Critical, kDataRoot};
    if (!cfg_.userProfile.empty()) {
        if (path::EqualsNoCase(c, cfg_.userProfile)) return {L::Critical, L"User profile folders are protected"};
        if (path::EqualsNoCase(path::Parent(c), cfg_.userProfile) &&
            path::StartsWithNoCase(path::Leaf(c), L"NTUSER.DAT"))
            return {L::Critical, L"Registry hive"};
        if (path::IsSameOrUnder(c, path::Join(cfg_.userProfile, L"AppData"))) return {L::Caution, kAppData};
    }
    for (const auto& r : {cfg_.appData, cfg_.localAppData, cfg_.programData})
        if (!r.empty() && path::IsSameOrUnder(c, r)) return {L::Caution, kAppData};
    return {L::None, L""};
}

ProtectionInfo PathProtectionService::Classify(const std::wstring& p) const {
    std::wstring c = path::Canonicalize(p);
    ProtectionInfo r = ClassifyCanonical(c);
    if (c.empty() || r.level == ProtectionLevel::Critical) return r;
    std::wstring f = path::ResolveFinal(c);
    if (!f.empty()) {
        std::wstring fc = path::Canonicalize(f);
        if (!path::EqualsNoCase(fc, c)) {
            ProtectionInfo r2 = ClassifyCanonical(fc);  // unresolvable/odd targets come back Critical
            if (r2.level > r.level) r = r2;
        }
    }
    return r;
}

bool PathProtectionService::IsProtectedPath(const std::wstring& p) const {
    return Classify(p).level == ProtectionLevel::Critical;
}
bool PathProtectionService::CanDelete(const std::wstring& p) const {
    return Classify(p).level <= ProtectionLevel::Caution;
}
bool PathProtectionService::CanPermanentlyDelete(const std::wstring& p) const {
    return Classify(p).level <= ProtectionLevel::Caution;
}
bool PathProtectionService::NeedsWarning(const std::wstring& p) const {
    return Classify(p).level == ProtectionLevel::Caution;
}
bool PathProtectionService::CanOpen(const std::wstring& p) const { return !path::Canonicalize(p).empty(); }
bool PathProtectionService::CanScan(const std::wstring& p) const { return !path::Canonicalize(p).empty(); }

}  // namespace ss
