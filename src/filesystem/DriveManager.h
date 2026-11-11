#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ss {

enum class MediaKind { Unknown, Ssd, Hdd };

struct DriveInfo {
    wchar_t letter = 0;
    std::wstring root;     // "C:\"
    std::wstring label;
    unsigned type = 0;     // GetDriveTypeW result
    bool ready = false;
    uint64_t total = 0, free = 0;
    MediaKind media = MediaKind::Unknown;
    uint64_t Used() const { return total >= free ? total - free : 0; }
    bool IsFixed() const;
    std::wstring TypeText() const;  // "Local disk (SSD)", "Removable", "Network", ...
};

class DriveManager {
public:
    static std::vector<DriveInfo> Enumerate();  // all mounted drive letters
    static MediaKind DetectMedia(wchar_t letter);
    // Worker count: SSD 4-8, HDD 2, unknown 4. Mixed roots => most conservative.
    static unsigned AutoWorkers(const std::vector<std::wstring>& roots);
};

}  // namespace ss
