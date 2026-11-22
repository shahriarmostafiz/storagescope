# StorageScope for Mac

A native macOS (SwiftUI) version of StorageScope: see what uses your disk, find big and forgotten files, search, clean caches, and move things to the Trash safely.

> **Status: first version, written on Windows and not yet compiled or run.** Open it on a Mac, build it, and send the first compiler errors or crashes back. See "First build" below.

## What it does

| Page | What it does |
|---|---|
| Overview | Disk usage, **Scan Folder...**, **Scan Whole Disk**, last-scan summary. While a scan runs, a bar above every page shows **Progress: N%** and **Time left** (whole-disk scans), a **Stop Scan** button and **View more** (the folder being scanned and the running totals) |
| Storage Analyzer | Folder tree, biggest first, with a share bar per item |
| Search | Words, `"exact phrase"`, `*.pdf` wildcards, regex, Match case, Whole word; the **Type** list is always visible, more filters (extensions, leave-out list, size, modified date) under Filters; "Search in: whole scan / selected folders"; "Skip folders". **Best match first**: a name equal to the query, then names starting with it, whole-word hits, names containing it (click a column header to sort by that column). Regex patterns that repeat a repeat, like `(a+)+`, are refused because they can freeze a search |
| Treemap | Click a folder to go into it, path bar to go up, hover for details |
| Large Files | Files above a size, by type |
| Old Files | Files and whole folders not modified for 1 / 3 / 6 / 12 / 24 months (default 3); a folder whose files are all old is one row |
| Duplicate Files | Files with exactly the same content (same size, then a hash of the start and end, then of the whole file), grouped. The oldest of each group is the **original** and is kept; every copy is selected and says which file it is a copy of (**Duplicate of** column). **Keep This One** picks another original. An original can never be selected, so one copy always stays. System software and the insides of `.git`, `.svn` and `.hg` folders are never listed |
| Cache & Logs | `~/Library/Caches`, logs, browser caches (Chrome, Edge, Brave, Arc, Vivaldi, Opera, Safari, Firefox, per profile), Xcode DerivedData, npm cache |

Every list has **Select all** and multi-select, **Show in Finder** and **Move to Trash**.

## Safety rules

- Nothing is deleted for good. Everything goes to the **Trash** (`FileManager.trashItem`), so it can be put back.
- Never offered or allowed: `/`, `/System`, `/Library`, `/Applications`, `/usr`, `/bin`, `/sbin`, `/private`, `/etc`, `/var`, `/opt`, your home folder, `~/Library`, Desktop, Documents, Downloads, Movies, Music, Pictures, `~/.ssh`, `~/.gnupg`.
- Allowed with an extra warning: anything inside `~/Library`, dot-folders in your home (`.config`, `.npm`, ...), `.app` bundles and photo/music libraries.
- Large Files, Old Files, Search and the Treemap leave out system software. App data (`~/Library` and home dot-folders) is hidden unless **Show app data** is on.
- Cache cleaning only touches items inside the cache folders it discovered, and never follows links.
- Scans read file information only; they never write.

## Requirements

- macOS 13 (Ventura) or newer
- Xcode 15 or newer, or the Command Line Tools (`xcode-select --install`)

## First build

```bash
cd "Storage Scope Mac"
swift build          # compile
swift test           # run the core tests
swift run            # run the app straight from Terminal
```

Or open `Package.swift` in Xcode and press Run.

To get a double-clickable app (universal: Apple Silicon and Intel):

```bash
bash build-app.sh
open build/StorageScope.app
```

## Full Disk Access

macOS refuses to list some folders (Mail, Messages, Safari data, other apps' containers) unless the app has **Full Disk Access**. Without it the scan still works, but totals are lower and a banner shows how many folders were skipped. Give access in System Settings > Privacy & Security > Full Disk Access (the banner has a button), then scan again.

The app is not sandboxed on purpose: a sandboxed app cannot scan the whole disk. That means it cannot go in the Mac App Store; distribute it directly.

## Sharing the app with other people

`build-app.sh` signs ad-hoc, which is enough to run on your own Mac. For other people:

1. An Apple Developer account (USD 99 per year).
2. Sign with a Developer ID Application certificate and the hardened runtime:
   `codesign --force --deep --options runtime --sign "Developer ID Application: Your Name (TEAMID)" build/StorageScope.app`
3. Notarize with `xcrun notarytool submit` and staple with `xcrun stapler staple`.

Without that, macOS shows an "unidentified developer" warning (right-click > Open gets past it).

## Layout

```
Package.swift
Sources/StorageScopeCore/   scanning, search index, protection rules, cache discovery, trashing (no UI)
Sources/StorageScopeMac/    the SwiftUI app
Tests/StorageScopeCoreTests/
build-app.sh                builds build/StorageScope.app
```

## How it works

- **Scanner**: `readdir` + `lstat` directly, top levels in parallel. Sizes are allocated bytes. Symbolic links are not followed or counted. For a whole-disk scan, `/Volumes`, `/System/Volumes` (a duplicate view of the same files), `/dev` and swap are skipped.
- **Search**: a flat copy of the scan in tree order with every name lowercased in one byte buffer. A folder's contents are one index range, so "search only in this folder" is a range scan and "skip this folder" is one jump. Matching uses `memmem`. A regex search stops after 3 seconds.
- **Large/Old Files, Search, Treemap** read the scan in memory. They do not touch the disk. **Duplicate Files** is the one page that reads file contents (SHA-256, only for files that already match in size and in their first and last 64 KB).

## Updates (matching the Windows version)

- **Browse while scanning:** Storage Analyzer and Treemap show the tree as far as it has been scanned and follow it about once a second. Search, Large Files, Old Files and Duplicate Files wait until the scan has finished.
- **Remembered scan:** the last scan is saved (compressed, in `~/Library/Application Support/StorageScope/lastscan.bin`) after a scan and after moving items to the Trash, and loaded at launch. Switch it off or clear it at the bottom of the sidebar.
- **Stop** keeps the previous results; the half-scanned tree is dropped.

## Known limits (first version)

- A saved scan is not re-checked at launch and there is no change watcher: use Scan to refresh it. (The Windows version also watches for changes.)
- The percentage and time left are estimates from the volume's used space; they get better after the first whole-disk scan (the app learns how much of the used space is real files).
- No special handling for external hard drives yet (the Windows version reads a spinning disk with one thread).
- Not compared against Finder's sizes: hard links are counted once per link, APFS clones and purgeable space are not special-cased.
- Treemap shows the 300 biggest items of a folder; the tree view shows the 1,000 biggest.
- Searches run on a background task and the results list is replaced when they finish. Typing pauses 200 ms before a search starts.
- Time Machine local snapshots, iCloud-only (not downloaded) files and other volumes are not scanned.
- No app icon yet.
