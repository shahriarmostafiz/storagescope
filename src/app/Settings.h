#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ss {

// Tiny flat JSON file at %LOCALAPPDATA%\StorageScope\settings.json.
// System path protection is not a setting: it cannot be disabled.
struct Settings {
    bool showHidden = true;
    bool showAppData = false;    // list AppData / ProgramData (advanced); off = hidden everywhere, sizes still counted
    bool askBeforeRecycle = true;
    bool requireSecondPermanentConfirm = true;
    uint32_t tempAgeHours = 24;  // 24 / 168 / 720
    uint32_t workers = 0;        // 0 = auto
    uint32_t theme = 0;          // 0 = follow Windows, 1 = light, 2 = dark
    uint32_t accent = 0;         // accent colour preset (index into the table in View.cpp; 0 = Emerald)
    uint32_t oldAge = 1;                // Old Files page: index into its age list (1 = 3 months)
    uint32_t oldSize = 1;               // Old Files page: index into its size list (1 = 10 MB)
    // Search page (remembered between sessions; the query text itself is not)
    uint32_t searchFlags = 0;  // bit0 filters panel open, bit1 match case, bit2 whole word, bit3 regex
    uint32_t searchWhere = 0;  // 0 = whole system, 1 = selected folders
    uint32_t searchKind = 0;   // 0 = files and folders, 1 = files, 2 = folders
    uint32_t searchType = 0;   // type list index (0 = all types)
    uint32_t searchDate = 0;   // modified list index (0 = any time)
    std::wstring searchInclude, searchExclude;
    std::vector<std::wstring> searchScope, searchSkip;  // folders to search in / folders to skip
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
