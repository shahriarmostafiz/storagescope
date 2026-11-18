# StorageScope: update brief for the Android app

Summary of everything changed in the Windows app (and ported to the Mac app) in this chat. Give this file to the Android app's session and ask it to apply what fits Android. Each item says **what**, **why**, and the **exact rule or number** so it can be copied. Nothing here was measured on Android; the Android code was not seen.

Reference code: Windows `D:\personal\idm\src\...`, Mac `D:\personal\Storage Scope Mac\Sources\...`.

## 1. Search (user complaints: exact matches came late, regex/partial first, bugs)

1. **Best match first.** When there is a text query (not regex), order results in tiers, ties by name:
   1. name equals the query (ignore the extension, case-insensitive)
   2. name starts with the query
   3. a whole word in the name equals the query
   4. name contains the query
   5. everything else (wildcard / multi-word matches)
   Clicking a column header switches to that column's order; "Best match" is the default again on a new search.
2. **Type filter always visible** (All, Documents, Images, Video, Audio, Archives, Apps, Code...), next to "Search in" and "Show" (files / folders / both). It was hidden under Filters before.
3. **"Search in" is one picker**: Whole scan / Selected folders (on Windows it also lists drives). Don't show two overlapping pickers.
4. **Regex safety.** Refuse patterns that repeat a repeat, such as `(a+)+$`, `(.*x)*`, `((a|b)+c)+`, `(\w+\s?)*` (they can freeze a search). Allow bounded counts: `(\d{4}-\d{2})+`, `(ab){2,3}c`, escaped `\(a+\)`, and chars inside `[...]`. Show an error message instead of searching. Also cap a regex search at about 3 seconds and say "stopped after 3 seconds, refine".
5. Wait about 200 ms after typing before searching; cancel the previous search when a new one starts.
6. Result cap 50,000 with a "showing the first 50,000" note.
7. **Hide system folders from Search / Large Files lists** with a "system location" test, not the delete-protection test. Hidden: the OS folder, program folders, folders whose name starts with `$`, boot/recovery names. **Never hide the user's own folders** (Documents, Desktop, Downloads). This was a real bug: the user's whole profile disappeared from lists.

## 2. Selection must survive refreshes

- Bug: ticks/highlights vanished a few seconds after selecting, whenever a list rebuilt (scan finished, folder changed, tree changed).
- Fix: remember selection **by path**, not by row index or object, before a rebuild and restore it after. If two "remember" calls happen before the restore, the second must **not** overwrite the first (return early while a restore is pending).
- A list that is hidden or whose data did not change should not rebuild at all. Use a cheap fingerprint (item count, a tree revision number that increments on every change).

## 3. "Select all"

Add a **Select all / Clear** control on every multi-select list: Large Files, Search results, Temp files, Cache files (Windows also Ctrl+A). It selects everything currently listed (after filters), not hidden rows.

## 4. Scan progress, time left, and browsing while scanning

1. **Progress line** above every page during a scan: `Progress: N%     Time left: m:ss`. Remove the "inaccessible files" counter from this line.
2. **View more / View less** button shows: the folder currently being scanned (folder or drive path, never a single file), items scanned so far, total size so far, and a note when the disk is a slow one. Plus a **Stop scan** button.
3. **Percent** = bytes found so far / expected bytes. Expected bytes = the volume's used space times a learned ratio (the share of used space that is real files; default 0.92, clamp 0.30 to 1.10). Save the ratio after each full scan and reuse it next time. For a single-folder scan there is no known total: show a moving bar and no percent.
4. **Time left** = remaining bytes / smoothed rate (exponential moving average of the scan rate). Show nothing for the first few seconds.
5. **Browse while scanning.** The folder tree and the treemap show what has been scanned so far and refresh about once a second. Large Files, Old Files and Search wait for the scan to end and say so.
   - A folder still being listed shows "Scanning..." instead of a size.
   - Folder sizes rise live: when a folder is listed, add its files' sizes to every ancestor.
   - The final totals are written exactly when the scan completes (so live numbers never stay wrong).
6. **Stop keeps the previous results.** A half-scanned tree is discarded, not shown as the answer.

## 5. Remembered scan (open instantly next launch)

- Save the last scan to app storage in a compact binary format (names, sizes, times, per-folder counts; **don't store folder totals, recompute them on load**), block-compressed. Write to a temp file then rename.
- Save in the **background** after a scan ends and after deleting items (never block the UI); mark the app "busy" while saving so a second save/delete can't overlap.
- Load at launch in the background. Treat the file as untrusted: bounds on counts, lengths, depth; reject truncated, damaged or wrong-version files and fall back to "no scan".
- Setting "Remember last scan" (default on) and a "Clear saved scan" button. Say in the README that scan names are stored locally.

## 6. Slow storage (HDD, SD card, USB, network): the 30+ minute scan

A user reported a 30+ minute scan on a slow disk. Cause: many threads fighting over a spinning disk's single head, and one slow system call per compressed file.

Rules used on Windows (adapt thread counts to Android storage; internal flash is fast, **SD cards / USB OTG / network shares are the slow case**):
- Detect the medium. Slow media: **1 reader thread**; unknown: start with 2 and raise to the CPU count if listings are fast; fast flash: all CPU threads.
- Per storage root keep its own work queue (depth-first, last-in-first-out so the head stays near its last position) and a per-root thread limit. Pick the next job from the root with the **fewest active threads** (round-robin) so one volume cannot starve another.
- Adapt: after the first 200 / 2000 folders, if the average folder listing takes over 5 ms drop to 1 thread; if under 1 ms raise.
- Exact on-disk size lookups (compressed/sparse files) cost one extra call per file. Give them a time budget (3 s; 60 s when all storage is fast) and after that **estimate** (allocated size is close to file size) and note that sizes are estimated.
- On slow media skip automatic work at start-up (recheck of the saved scan, weekly rescan, first-run auto-scan): wait for the user to press Scan.
- Results of the simulation (modelled, not measured on a real disk): 3,455-folder tree, old 4 threads 41.6 s vs new HDD mode 10.7 s; 3,000 compressed files 39.8 s vs 4.0 s; a 1.66 million file C: + D: scan modelled at about 63 min old vs about 19 min new on typical HDD timings.

## 7. Other fixes worth copying

- Text boxes: Ctrl+Backspace (delete word) and arrow keys must work inside search/filter fields; a custom key handler was swallowing them.
- Large Files / Old Files: Old Files lists a folder as **one row** when all its files are old (months: 1/3/6/12/24, default 3).
- Cache cleaning is a separate page: app caches, logs, browser caches per profile; only items found by discovery, never follow links, and deletion goes to the recycle bin / trash where the OS has one.
- Never delete anything protected (system, app, other users' data); confirm before every delete; delete only what was ticked.
- Landing page download counter (website only): hidden until the real count is 100 or more, then shown and refreshed every 2 minutes. Not an app change.

## 8. Tests to add on Android (same idea as the Windows/Mac tests)

- Ranking order: for query `readme` over `readme.md.bak, README, my readme file.txt, readme.txt, xreadmex.txt, notes.txt` the order is `README, readme.txt, readme.md.bak, my readme file.txt, xreadmex.txt, notes.txt`.
- Regex guard: bad list and good list from section 1.4.
- Saved scan: round-trip equals the original totals; a file cut in half is rejected without a crash; a missing file returns nothing.
- Selection: tick two items, rebuild twice in a row, both still ticked.
- Live scan: during the scan the root is readable and "building"; at the end no folder is still "building" and the total equals the exact total.
- Time format: 75 s is `1:15`, 3725 s is `1:02:05`, negative is `0:00`.
- Hiding rule: user folders stay visible; OS folders stay hidden.

## 9. Not done / caveats to pass along

- Mac port was written without a Swift compiler; nothing there is compiled yet.
- HDD numbers are a simulation, not a measurement on a real slow disk.
- Memory is about 185 bytes per scanned item (about 190 MB per million files). Left as is on purpose: target users have more than 4 GB RAM. Lower-RAM Android phones may need a cap or a lighter node.
- Windows also has a live change watcher and a startup recheck of the saved scan; Mac and this brief leave them out. On Android use a FileObserver-style watcher only if it is cheap; otherwise rescan on demand.
