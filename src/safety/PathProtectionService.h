#pragma once
#include <string>
#include <vector>

namespace ss {

enum class ProtectionLevel {
    None = 0,      // ordinary user data
    Caution = 1,   // deletable, but warn (AppData, ProgramData)
    NoDelete = 2,  // may be scanned/opened, never deleted by this app (Program Files)
    Critical = 3,  // Windows / boot / drive roots / profile roots / invalid paths
};

struct ProtectionInfo {
    ProtectionLevel level = ProtectionLevel::None;
    std::wstring reason;
};

// The single place where "may this path be deleted?" is decided. UI and FileOperationService
// both call it; nothing else contains protected-path knowledge.
class PathProtectionService {
public:
    struct Config {
        std::wstring systemRoot;                 // C:\Windows
        std::vector<std::wstring> programDirs;   // C:\Program Files, C:\Program Files (x86)
        std::wstring usersRoot;                  // C:\Users
        std::wstring programData;                // C:\ProgramData
        std::wstring userProfile;                // C:\Users\John
        std::wstring appData;                    // ...\AppData\Roaming
        std::wstring localAppData;               // ...\AppData\Local
    };
    static Config ConfigFromEnvironment();

    explicit PathProtectionService(Config cfg);

    // Classifies the lexical canonical path AND (if it exists) the path after junction/symlink
    // resolution; the stricter result wins.
    ProtectionInfo Classify(const std::wstring& path) const;

    bool IsProtectedPath(const std::wstring& path) const;       // Critical only
    bool CanDelete(const std::wstring& path) const;             // recycle
    bool CanPermanentlyDelete(const std::wstring& path) const;
    bool CanOpen(const std::wstring& path) const;
    bool CanScan(const std::wstring& path) const;
    bool NeedsWarning(const std::wstring& path) const;          // Caution level

private:
    ProtectionInfo ClassifyCanonical(const std::wstring& canonical) const;
    Config cfg_;
};

}  // namespace ss
