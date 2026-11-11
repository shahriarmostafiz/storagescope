# StorageScope

A small native Windows desktop tool that shows where your disk space went and lets you remove
things safely. Plain C++20 and Win32: no frameworks, no runtime, no network access, no telemetry.
Working name; the product name appears only in `CMakeLists.txt`, the window title/class names in
`src/ui/MainWindow.cpp`, `resources/app.manifest`, and the `%LOCALAPPDATA%\StorageScope` folder
name in `src/app/Settings.cpp`.

* Release EXE: **~0.46 MB** (static CRT, one file, icon embedded). Idle RAM after start: ~18 MB.
* Look: light, pure-black dark, or follow Windows (Settings > Appearance); larger UI font; rounded controls; Mica title bar on Windows 11.
* Runs as a normal user (`asInvoker`). It never asks for elevation.

## Running the downloaded exe

The exe is not code-signed yet, so Windows SmartScreen may show "Windows protected your PC". Click **More info**, then
**Run anyway**, or right-click the file, choose Properties and tick **Unblock**. Compare the file with the published
SHA-256 (`certutil -hashfile StorageScope.exe SHA256`) if you want to confirm it is the release build.

## Code signing policy and privacy

Free code signing provided by [SignPath.io](https://signpath.io), certificate by
[SignPath Foundation](https://signpath.org) (once the project is approved; until then releases are unsigned).
Signed releases are built from this repository by the GitHub Actions workflow in `.github/workflows/build.yml`.

Privacy: StorageScope makes no network connections and sends no data anywhere. Everything it stores stays in
`%LOCALAPPDATA%\StorageScope` on your PC (see "Remembered scan and automatic updates" and "Logs and settings").

Licensed under the [MIT License](LICENSE).

## Requirements

* Windows 10 or 11, x64
* Visual Studio 2019 (16.11+) or 2022 with the *Desktop development with C++* workload
  (MSVC v142/v143 + a Windows 10/11 SDK). C++20 `std::jthread` / `std::stop_token` are required.
* CMake 3.20+ (optional, `build.bat` works without it)

## Build

Without CMake (what was used to develop and verify this project):

```bat
build.bat            :: core library + tests + app  -> build\StorageScope.exe, build\StorageScopeTests.exe
build.bat tests      :: core library + tests only
build\StorageScopeTests.exe
```

With CMake (from a *x64 Native Tools* prompt, or any generator that targets MSVC x64):

```bat
mkdir build-cmake
cd build-cmake
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
ctest -C Release --output-on-failure
```

> The CMake files were written to mirror `build.bat`, but the machine this was developed on has
> no CMake installed, so **the CMake path has not been run**. `build.bat` is the verified path.

Visual Studio: *File > Open > CMake...* and pick `CMakeLists.txt`, or open the generated `.sln`.

## What it does

| Page | Purpose |
|---|---|
| Dashboard | Mounted drives as cards (used / free / total, SSD/HDD, type, and how free space changed vs. about a week ago). Scan Selected Drive, Scan Folder, Scan All Local Drives (manual scanning always works; see "Remembered scan and automatic updates"). A **Recycle Bin** row shows its size (emptying it is done on the Recycle Bin page). After a scan: **usage by file type** (stacked bar + legend), the **10 biggest folders** (by the files directly inside them) and the **10 biggest files**; double-click one to jump to it. At launch these come from the saved scan immediately, and they are recomputed after every change (a deleted or auto-detected change removes/updates the entries at once). |
| Storage Analyzer | Browse the scan like a file manager: size, size on disk, % of parent bar, file/folder counts, modified, status. Sort any column, breadcrumb, search, context menu, multi-select, delete. |
| Treemap | Squarified treemap of the current folder (box area = size; files coloured by type). Click selects, double-click enters a folder, right-click for Open in Analyzer / Open File Location / Add to Basket. |
| Large Files | Thresholds 100 MB / 500 MB / 1 GB / 5 GB / custom, type filter (videos, pictures, audio, archives, disk images, installers, documents, other), a **last-modified** filter (older than 6 months / 1 year / 2 years), sorting, Refresh. Extension never implies "safe to delete". |
| Temporary Files | Scans user temp locations, age filter (24 h / 7 d / 30 d / all eligible), checkbox categories, per-file failure handling. |
| Recycle Bin | The Windows Recycle Bin of all drives, read through the shell: name, original location, size, date deleted. Tick items (or highlight them), then **Restore Selected** (Windows asks about name conflicts), **Delete Permanently...** (confirmed; cannot be undone) or **Empty Recycle Bin** (all three run in the background, so the window stays responsive). Select All / Clear Selection, Ctrl+A, Esc, Delete and F5 work too. |
| Settings | Theme, show hidden files, delete confirmations, default temp age, worker count, Open Logs, and an **ignore list** of folders/drives (several can be added at once). Scans skip ignored folders completely, so nothing inside them is counted or shown; adding one hides it from the current results immediately, removing one brings it back after the next scan. An ignored drive is skipped by "Scan All Local Drives" and refused when scanned directly. |

**Selecting several items.** Storage Analyzer, Large Files and the Recycle Bin have a **check box in front of each row**: click the boxes to pick any rows, no Ctrl needed. Ticked rows are what Delete, Recycle, Add to Basket and the other actions use; with nothing ticked they use the highlighted rows (click, Shift+click and Ctrl+click still work). Ctrl+A ticks every row, Esc clears the ticks, Space ticks or unticks the highlighted rows. In the two big (virtual) lists the box is painted by the app, in the accent colour.

**Basket.** "Add to Basket" (Analyzer, Large Files, Treemap) collects items across pages; a row under the progress bar shows the count and total size, and "Delete Basket..." deletes them with the normal protection checks and confirmations. Protected items cannot enter the basket.

**Small comforts.** Success messages appear as a short banner; the window size, position and last page are remembered; dropping a folder or drive onto the window scans it. Free space per drive per day is stored in `%LOCALAPPDATA%\StorageScope\history.csv` (a few numbers, no file names, capped at 1000 lines) for the "vs. a week ago" line; switch it off or clear it under Settings > Privacy.

**Refreshing.** After a delete the list is updated from what is really on disk (items that report an error but are gone are removed; a half-deleted folder is rescanned). Storage Analyzer and Large Files have a **Refresh** button (also F5): Neither does a full rescan: Analyzer re-lists the folder you are viewing (new, removed and changed entries), Large Files checks the listed files and re-lists their folders. Use the Dashboard to scan again from scratch. Temporary Files rescans itself after cleaning and then shows a confirmation with what was freed, what was skipped and what remains.

Search, sorting, navigation and Large Files all run on the in-memory index; they never touch the disk.

### Remembered scan and automatic updates

**This deliberately reverses two earlier rules** ("never cache scan results" and "never scan at start-up"). Both are now
defaults you can turn off under Settings > Scan memory and updates.

* **Saved scan** (`%LOCALAPPDATA%\StorageScope\lastscan.bin`, on by default). The finished scan is written when a full
  scan completes and again on exit if the tree changed (about 1-2 s). At the next launch it loads in the background
  (about 4 s for 1.7 million files; 25 MB on disk, compressed with the Windows Compression API) and the Dashboard shows
  its statistics immediately from the file header. The file is read as untrusted input: counts and lengths are bounded and
  a truncated, corrupt or other-version file is discarded, never crashes the app. **Privacy:** the file contains the
  names, sizes and dates of the scanned files and folders (not their contents). "Clear saved scan" deletes it; turning
  "Remember the last scan" off deletes it too and nothing is written.
* **Automatic updates** (on by default). At launch a worker compares every folder's modified time with the disk and
  re-lists only the folders that changed; while the app runs, a `ReadDirectoryChangesW` watcher (no admin rights) reports
  changed folders and a 3-second timer re-lists them (up to 100 per tick) and updates every page, including the Dashboard
  lists. After you delete something the parent folders are re-listed too. If the watcher loses events (buffer overflow)
  the app re-checks everything instead of guessing. A saved scan whose last full scan is over 7 days old is rescanned
  automatically. Turning automatic updates off disables the watcher, the launch check, the first-launch scan and the old-scan rescan; scanning stays manual.
* **First launch** with no saved scan scans all local drives once in the background (Stop Scan works).
* Manual scans (Scan Drive/Folder/All, Refresh) always work and replace the saved scan.

## Architecture

```
src/
  main.cpp                     entry point (COM, common controls, logger)
  app/Settings                 tiny flat JSON in %LOCALAPPDATA%\StorageScope\settings.json
  filesystem/PathUtils         canonicalisation, component-wise prefix tests, reparse resolution
  filesystem/DriveManager      GetLogicalDrives/GetDriveTypeW/GetDiskFreeSpaceExW, SSD/HDD detection
  safety/PathProtectionService the ONLY place that decides what may be deleted
  scanner/IScanner             interface (a future NTFS MFT/USN scanner implements this)
  scanner/DirectoryScanner     FindFirstFileExW worker-pool implementation
  scanner/ScanNode             compact tree + bottom-up size bookkeeping
  operations/FileOperationService  recycle (IFileOperation) + permanent delete, re-validates every path
  operations/ExplorerService   Explorer / properties / clipboard / Settings app
  cleanup/TempCleanup          temp scan + clean
  utils/FormatUtils, Logger
  ui/                          MainWindow (nav, progress, delete flow) + one View per page
tests/tests.cpp                safety, scanner, deletion, temp cleanup, cancellation
```

The core library has no UI dependency, so everything except `src/ui` and `main.cpp` is covered by
the test executable.

### Scanner

* `FindFirstFileExW(FindExInfoBasic, FIND_FIRST_EX_LARGE_FETCH)` on `\\?\` paths (long paths work).
* A fixed pool of worker threads takes directories from a LIFO queue. Never one thread per directory.
  Auto worker count: SSD 4-8, HDD 2, unknown 4; overridable in Settings.
* Each worker enumerates one directory into a contiguous `std::vector<ScanNode>`, adds its own files to
  the directory's totals and queues sub-directories. A per-directory pending counter finalises a folder
  when its whole subtree is done and adds its totals to the parent. **Parents are never re-scanned.**
* Directory reparse points (junctions, symlinks, mount points) are recorded as "not scanned" and never
  followed, so there are no loops, no double counting, no accidental volume crossing.
* Access denied / vanished directories / disconnected drives are recorded on the node and counted; the
  scan continues. Cancellation is checked between directories and every 256 entries; `Scan()` joins all
  workers before returning.
* Memory: names are stored once per node, full paths are rebuilt from parent pointers. Measured: roughly
  200 bytes per entry including allocator overhead (see Verification).
* Size on disk is the file size rounded up to the volume's cluster size; compressed/sparse files use
  `GetCompressedFileSizeW`; cloud placeholders count as 0.
* The UI is told about progress with a throttled `WM_APP_SCAN_PROGRESS` (about 10/s) and one
  `WM_APP_SCAN_COMPLETE`. Workers never touch controls.

### System files

Files with the Windows System attribute (`pagefile.sys`, `hiberfil.sys`, ...) are never shown in any list, chart or
statistic, and there is no setting to show them (the old `showSystem` settings value no longer exists). They still count
towards the drive's used space on the Dashboard, which comes from Windows. System *locations* (`C:\Windows` etc.) are
protected from deletion as described below.

### Protection rules

`PathProtectionService::Classify()` canonicalises the path (env vars, `\\?\`, `/`, `.`/`..`, duplicate and
trailing separators, trailing dots/spaces, 8.3 short names, case-insensitive), then **also** resolves it
through junctions/symlinks and applies the stricter of the two results. Prefix tests are per path
component (`C:\Windows2` is not under `C:\Windows`). Paths with alternate-data-stream colons, wildcards,
device prefixes, drive-relative forms or relative forms are rejected.

| Level | Where | Result |
|---|---|---|
| Critical | `%SystemRoot%` and everything below, drive roots, `System Volume Information`, `Recovery`, `EFI`, `Boot`, `bootmgr`, `$Recycle.Bin` and any root-level `$...` name, `pagefile/hiberfil/swapfile.sys`, `Config.Msi`, `C:\Users`, every profile folder, `NTUSER.DAT*`, UNC paths, unresolvable targets | never deletable |
| NoDelete | `Program Files`, `Program Files (x86)` | scan/open/manage only; UI points to Windows Settings |
| Critical (data roots) | `C:\ProgramData`, `%USERPROFILE%\AppData`, `...\AppData\Local`, `...\AppData\Roaming` themselves | never deletable (one click would wipe every app's settings) |
| Caution | anything *inside* AppData / ProgramData (including `Local\Temp`) | deletable, with an extra warning |
| None | everything else (Downloads, Documents, Desktop...) | deletable with confirmation |

Protection cannot be disabled in Settings. Four layers are enforced: (1) the UI disables the buttons,
(2) `FileOperationService` re-checks every path itself, (3) paths are canonicalised, (4) the check is repeated
immediately before the file-system call. A tiny window between check and delete cannot be closed completely
on a shared file system (see limitations).

### Deletion

* Default choice is always **Move to Recycle Bin** (`IFileOperation`, shell does the Recycle Bin work).
  `FOF_NOCONFIRMATION` is deliberately *not* set, so if Windows cannot recycle an item it asks you instead of
  silently deleting it. Items in the Recycle Bin keep using disk space; the UI says so.
* **Permanent delete** needs a second, red warning whose default button is Cancel. It never follows reparse
  points (links are unlinked, targets are untouched), clears read-only flags, and continues past items it cannot
  delete.
* Results are structured (`FileOperationError`) and shown in words ("3 items could not be deleted because they
  are currently in use"). Technical codes go only to the log.
* After a successful delete the scan tree is updated in place (the node is hidden and all ancestors' sizes/counts
  are reduced). "Refresh Folder" re-scans one folder and grafts the result in.

### Temporary files

Only existing, validated folders named `Temp` / `Tmp` / `CrashDumps` inside the user profile are offered
(`%TEMP%`, `%LOCALAPPDATA%\Temp`, `%LOCALAPPDATA%\CrashDumps`). `C:\Windows\Temp` is deliberately excluded. A file is
eligible only if both its write and creation time are older than the chosen age (default 24 hours); "all eligible"
still keeps anything touched in the last minute. Locked files are skipped and counted, directories are never followed
through reparse points, and empty old directories are removed. Cleaning is a permanent delete (Recycle Bin makes no
sense for temp files) and the folder is re-validated right before deleting. No registry, WinSxS, driver store, browser
or restore-point cleaning exists.

### Logs and settings

`%LOCALAPPDATA%\StorageScope\logs\storagescope.log`, rotated at 1 MB, 5 files maximum (5 MB total). Logs contain
counts, sizes and error codes, not file names. Settings: `%LOCALAPPDATA%\StorageScope\settings.json`. The last scan is
persisted in `lastscan.bin` (file and folder names; see "Remembered scan and automatic updates") unless you switch that off.

## Verification (Windows 11 x64, MSVC 14.29, Release, this machine)

Automated (`StorageScopeTests.exe`, 277 checks, all passing, including the Recycle Bin listing and purge, the ignore list, settings persistence, file categories, scan statistics, the free-space history, the saved-scan round trip and corrupt-file rejection, incremental tree updates checked against a fresh scan (an ignored folder stays out of re-listings; a re-listed parent does not hide unread changes in a sub-folder), the folder-time re-check and the change watcher; cancellation tests use a generated folder tree, so results do not depend on the machine): path canonicalisation, the refresh-folder subtree swap, every protection rule incl. `..`,
case, trailing slashes, `%SystemRoot%`, 8.3 names, ADS, `\\?\`, long and Unicode paths, junction to `System32`;
scanner totals on a tree with Unicode folders, a >300-char path and a junction (target not counted); inaccessible and
missing roots; cancellation (1 ms before start, ~400 ms mid-scan) with worker threads gone afterwards; permanent delete
of a tree containing a junction (target survives, read-only files handled, locked file reported as in use); refusal of
`C:\Windows`, `System32`, `Program Files`, drive roots; real Recycle Bin delete; temp cleanup on a controlled folder
(old files removed, recent and locked files kept, empty old folder removed, non-temp folders rejected).

Manual, in the real app: start-up without scanning (18 MB); full scan of `C:` (1,088,267 files, 347,028 folders,
250.3 GB in 57 s, 589 inaccessible folders skipped, peak working set 286 MB, UI round-trip latency ~0 ms during the scan);
`D:` scan (636,657 files, 11 s); Stop (124 ms, memory back to 20 MB); Windows and Program Files selected => delete
buttons disabled with the explanation and "Open Installed Apps"; delete dialog default = Recycle Bin; Esc cancels;
recycled folder verified in the Recycle Bin; permanent delete needs the second red dialog, Enter on its default does
nothing; Manage opens Explorer at the folder; Large Files, search, Temporary Files scan and Settings pages render real
data.

Not done: no installer; the CMake build; temp cleanup of the *real* `%TEMP%` from the UI (only the controlled test
folder was cleaned); dark mode; testing on a mechanical disk or in a mixed-DPI setup.

## Known limitations

* **Hard links** are counted once per link. **Alternate data streams** and NTFS metadata are not counted, so the
  scanned total is slightly below "used" space on the drive (about 8 GB of 259 GB on the test machine).
* Size on disk is an estimate (cluster rounding), not an exact `$MFT` allocation.
* Version 1 uses directory enumeration only. The `IScanner` interface is where an MFT/USN scanner would plug in.
* "Skip reparse points" and "Skip inaccessible folders" are always on (shown disabled in Settings). Cloud-placeholder
  directories that are reparse points are not entered.
* Search matches names (or the full path if the query contains `\` or `:`) and shows at most 20,000 hits.
* Automatic updates catch added, removed and renamed entries, and file size changes seen by the watcher while the app is
  open. A file that grew while the app was closed, without any entry being added or removed in its folder, keeps its old
  size until the next full scan (hence the automatic rescan after 7 days) or Refresh. Heavy churn (more events than the
  watcher can hold) triggers a full folder re-check instead of exact updates.
* Dashboard statistics from the saved scan are shown for a few seconds at launch; double-clicking their entries works once the tree has loaded.
* Extremely deep trees (tens of thousands of nested levels) are protected by an 8 MB stack reserve rather than by
  non-recursive destruction.
* DPI: fonts and layout follow monitor DPI (Per-Monitor V2); list column widths are not rescaled when you drag the
  window between monitors with different scaling.
* Drop-downs are a custom rounded control. They expose the "combo box" role, a name and the current value through both MSAA and
  UI Automation (both verified in-process on this machine; not tried with a real screen reader). The popup list is a Windows menu
  (rounded on Windows 11, square on Windows 10).
* Network drives are listed but their free space is never queried (an offline share could freeze the window); scan them explicitly.
* The test suite still recycles one tiny file into your Recycle Bin per run (it must test the real Recycle Bin path).
* There is no installer. The deliverable is a single portable EXE; the Release configuration is `build.bat` / CMake `Release`.
* The CMake files are untested on the development machine (no CMake installed).
