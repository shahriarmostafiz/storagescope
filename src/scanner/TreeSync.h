#pragma once
#include <stop_token>
#include <string>
#include <vector>

#include "scanner/DirectoryEnum.h"
#include "scanner/ScanNode.h"

namespace ss {

// True if applying `listing` to `dir` adds entries or changes an entry's kind, i.e. the directory's child list gets rebuilt and
// every ScanNode* the UI holds into it would dangle. Callers drop their pointers first when this is true.
bool NeedsStructuralChange(const ScanNode* dir, const std::vector<DirEntry>& listing);

struct SyncStats {
    int added = 0, removed = 0, updated = 0;
    std::vector<std::wstring> newDirs;  // sub-folders that appeared: list them next (they are empty placeholders for now)
    bool Changed() const { return added || removed || updated; }
};

// Makes `dir` match what is on disk one level deep (files added/removed/resized, folders added/removed) and corrects the totals of
// every ancestor. Idempotent. A non-Ok `status` only records the status (a listing that failed cannot be trusted).
// `ownModified` (optional) is the folder's own last-write time read BEFORE it was listed; it is stored so the next start-up check
// compares against what was really read.
SyncStats ApplyListing(ScanNode* dir, const std::vector<DirEntry>& listing, ListStatus status, uint64_t ownModified = 0);

// Quick "which folders changed?" check. NTFS updates a folder's modified time when an entry is added/removed/renamed in it, so
// comparing stored times with the disk finds changed folders without re-reading their contents.
struct DirCheck {
    std::wstring path;
    uint64_t modifiedSecs = 0;  // stored FILETIME / 10^7
};
void CollectDirChecks(const ScanNode* root, std::vector<DirCheck>& out);
std::vector<std::wstring> ValidateDirs(const std::vector<DirCheck>& checks, unsigned workers, std::stop_token st);

}  // namespace ss
