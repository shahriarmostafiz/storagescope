#include "filesystem/DriveManager.h"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <thread>

namespace ss {

bool DriveInfo::IsFixed() const { return type == DRIVE_FIXED; }

std::wstring DriveInfo::TypeText() const {
    switch (type) {
        case DRIVE_FIXED:
            return media == MediaKind::Ssd ? L"Local disk (SSD)" : media == MediaKind::Hdd ? L"Local disk (HDD)" : L"Local disk";
        case DRIVE_REMOVABLE: return L"Removable";
        case DRIVE_REMOTE: return L"Network";
        case DRIVE_CDROM: return L"Optical";
        case DRIVE_RAMDISK: return L"RAM disk";
        default: return L"Unknown";
    }
}

MediaKind DriveManager::DetectMedia(wchar_t letter) {
    wchar_t dev[] = {L'\\', L'\\', L'.', L'\\', letter, L':', 0};
    HANDLE h = CreateFileW(dev, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return MediaKind::Unknown;
    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceSeekPenaltyProperty;
    q.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR d{};
    DWORD ret = 0;
    MediaKind k = MediaKind::Unknown;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, &d, sizeof d, &ret, nullptr) && ret >= sizeof d)
        k = d.IncursSeekPenalty ? MediaKind::Hdd : MediaKind::Ssd;
    CloseHandle(h);
    return k;
}

std::vector<DriveInfo> DriveManager::Enumerate() {
    std::vector<DriveInfo> out;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        DriveInfo d;
        d.letter = static_cast<wchar_t>(L'A' + i);
        d.root = std::wstring(1, d.letter) + L":\\";
        d.type = GetDriveTypeW(d.root.c_str());
        if (d.type == DRIVE_NO_ROOT_DIR || d.type == DRIVE_UNKNOWN) continue;
        if (d.type == DRIVE_REMOTE) {  // an offline share can block this call for 30+ s: list it, never query it
            d.ready = true;
            out.push_back(std::move(d));
            continue;
        }
        ULARGE_INTEGER avail{}, total{}, freeb{};
        d.ready = GetDiskFreeSpaceExW(d.root.c_str(), &avail, &total, &freeb) != 0;
        if (d.ready) {
            d.total = total.QuadPart;
            d.free = freeb.QuadPart;
            wchar_t label[MAX_PATH + 1] = L"";
            if (GetVolumeInformationW(d.root.c_str(), label, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0)) d.label = label;
        }
        if (d.type == DRIVE_FIXED) d.media = DetectMedia(d.letter);
        out.push_back(std::move(d));
    }
    return out;
}

unsigned DriveManager::AutoWorkers(const std::vector<std::wstring>& roots) {
    unsigned hw = std::max(2u, std::thread::hardware_concurrency());
    unsigned ssd = std::min(8u, std::max(4u, hw / 2));
    unsigned result = ssd;
    for (const auto& r : roots) {
        if (r.size() < 2 || r[1] != L':') {
            result = std::min(result, 4u);
            continue;
        }
        MediaKind k = DetectMedia(static_cast<wchar_t>(towupper(r[0])));
        if (k == MediaKind::Hdd) result = std::min(result, 2u);
        else if (k == MediaKind::Unknown) result = std::min(result, 4u);
    }
    return result;
}

}  // namespace ss
