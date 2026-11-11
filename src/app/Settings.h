#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ss {

// Tiny flat JSON file at %LOCALAPPDATA%\StorageScope\settings.json.
// System path protection is not a setting: it cannot be disabled.
struct Settings {
    bool showHidden = true;
    bool showSystem = false;  // always false: system files (pagefile.sys, hiberfil.sys, ...) are never shown; not user-changeable
    bool askBeforeRecycle = true;
    bool requireSecondPermanentConfirm = true;
    uint32_t tempAgeHours = 24;  // 24 / 168 / 720
    uint32_t workers = 0;        // 0 = auto
    uint32_t theme = 0;          // 0 = follow Windows, 1 = light, 2 = dark
    std::vector<std::wstring> ignored;  // canonical folders/drives that scans skip entirely
    bool keepHistory = true;            // remember free space per drive per day (History) for the Dashboard trend
    bool rememberScan = true;           // save the last scan to disk and load it at start-up (stores file/folder names locally)
    bool autoUpdate = true;             // keep results current: re-check at launch, watch for changes, scan once on first launch
    bool firstRunDone = false;          // the automatic first scan has been attempted
    uint32_t lastFullScanDay = 0;       // days since 1601 of the last complete scan (0 = unknown); old data triggers a rescan
    // window state, restored at start: size 0 = never saved
    int winX = 0, winY = 0, winW = 0, winH = 0, lastPage = 0;
    bool maximized = false;

    static std::wstring AppDataDir();  // %LOCALAPPDATA%\StorageScope (created on demand)
    void Load();
    void Save() const;
};

}  // namespace ss
