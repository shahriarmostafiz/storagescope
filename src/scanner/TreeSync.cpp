#include "scanner/TreeSync.h"

#include <windows.h>

#include <algorithm>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "filesystem/PathUtils.h"

namespace ss {
namespace {

std::wstring Lower(const std::wstring& s) {
    std::wstring o = s;
    for (auto& c : o) c = c < 128 ? (c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c + 32) : c) : static_cast<wchar_t>(towlower(c));
    return o;
}

NodeType KindOf(const DirEntry& e) { return !e.IsDir() ? NodeType::File : (e.IsReparseDir() ? NodeType::Reparse : NodeType::Directory); }

NodeStatus ReparseStatus(uint32_t tag) {
    return tag == IO_REPARSE_TAG_MOUNT_POINT ? NodeStatus::Junction : tag == IO_REPARSE_TAG_SYMLINK ? NodeStatus::Symlink : NodeStatus::ReparseOther;
}

}  // namespace

bool NeedsStructuralChange(const ScanNode* dir, const std::vector<DirEntry>& listing) {
    if (!dir->dir) return !listing.empty();
    std::unordered_map<std::wstring, NodeType> existing;
    existing.reserve(dir->dir->children.size());
    for (const auto& c : dir->dir->children)
        if (!c.removed) existing.emplace(Lower(c.name), c.type);
    for (const auto& e : listing) {
        auto it = existing.find(Lower(e.name));
        if (it == existing.end() || it->second != KindOf(e)) return true;
    }
    return false;
}

SyncStats ApplyListing(ScanNode* dir, const std::vector<DirEntry>& listing, ListStatus status, uint64_t ownModified) {
    SyncStats s;
    if (!dir->dir) dir->dir = std::make_unique<DirData>();
    if (status != ListStatus::Ok) {
        dir->status = status == ListStatus::AccessDenied ? NodeStatus::AccessDenied : NodeStatus::Unavailable;
        return s;
    }
    dir->status = NodeStatus::Ok;
    if (ownModified) dir->modified = ownModified;  // this folder's contents are now known to match that time

    std::unordered_map<std::wstring, const DirEntry*> byName;
    byName.reserve(listing.size());
    for (const auto& e : listing) byName[Lower(e.name)] = &e;

    std::unordered_set<std::wstring> kept;  // names that exist both in the tree and on disk
    for (auto& c : dir->dir->children) {
        if (c.removed) continue;
        std::wstring key = Lower(c.name);
        auto it = byName.find(key);
        if (it == byName.end()) {  // gone from disk
            DetachFromTree(&c);
            ++s.removed;
            continue;
        }
        const DirEntry& e = *it->second;
        if (KindOf(e) != c.type) {  // file became folder or the reverse: remove now, re-add below
            DetachFromTree(&c);
            ++s.removed;
            continue;
        }
        kept.insert(key);
        if (c.type == NodeType::File && (e.size != c.size || e.allocated != c.allocated)) {
            AdjustAncestors(c.parent, static_cast<int64_t>(e.size) - static_cast<int64_t>(c.size),
                            static_cast<int64_t>(e.allocated) - static_cast<int64_t>(c.allocated), 0, 0);
            c.size = e.size;
            c.allocated = e.allocated;
            ++s.updated;
        }
        // A sub-folder's time is only taken when that folder itself is listed; copying it here would hide unread changes inside it.
        if (c.type != NodeType::Directory) c.modified = e.modified;
        c.attrs = e.attrs;
    }

    std::vector<const DirEntry*> adds;
    for (const auto& e : listing)
        if (!kept.count(Lower(e.name))) adds.push_back(&e);
    if (adds.empty()) return s;

    // New entries: rebuild the child list (this moves the surviving nodes, so grandchildren get their parent re-pointed).
    std::wstring dirPath = BuildPath(dir);
    std::vector<ScanNode> fresh;
    fresh.reserve(dir->dir->children.size() + adds.size());
    for (auto& c : dir->dir->children)
        if (!c.removed) fresh.push_back(std::move(c));
    int64_t dSize = 0, dAlloc = 0, dFiles = 0, dDirs = 0;
    for (const DirEntry* e : adds) {
        ScanNode n;
        n.name = e->name;
        n.attrs = e->attrs;
        n.type = KindOf(*e);
        n.modified = n.type == NodeType::Directory ? 0 : e->modified;  // 0 = unknown: a new folder is empty until its own listing arrives
        if (n.type == NodeType::File) {
            n.size = e->size;
            n.allocated = e->allocated;
            dSize += static_cast<int64_t>(e->size);
            dAlloc += static_cast<int64_t>(e->allocated);
            ++dFiles;
        } else if (n.type == NodeType::Directory) {
            n.dir = std::make_unique<DirData>();  // filled by the follow-up listing of newDirs
            ++dDirs;
            s.newDirs.push_back(path::Join(dirPath, e->name));
        } else {
            n.status = ReparseStatus(e->reparseTag);
        }
        fresh.push_back(std::move(n));
        ++s.added;
    }
    for (auto& c : fresh) {
        c.parent = dir;
        if (c.dir)
            for (auto& g : c.dir->children) g.parent = &c;
    }
    dir->dir->children = std::move(fresh);
    AdjustAncestors(dir, dSize, dAlloc, dFiles, dDirs);
    return s;
}

void CollectDirChecks(const ScanNode* root, std::vector<DirCheck>& out) {
    std::vector<std::pair<const ScanNode*, std::wstring>> stack;
    stack.emplace_back(root, BuildPath(root));
    while (!stack.empty()) {
        auto [n, p] = std::move(stack.back());
        stack.pop_back();
        if (n->type == NodeType::Directory && !p.empty()) out.push_back({p, n->modified / 10000000ull});
        if (!n->dir) continue;
        for (const auto& c : n->dir->children)
            if (!c.removed && c.type == NodeType::Directory) stack.emplace_back(&c, path::Join(p, c.name));
    }
}

std::vector<std::wstring> ValidateDirs(const std::vector<DirCheck>& checks, unsigned workers, std::stop_token st) {
    workers = std::max(1u, workers);
    std::vector<std::vector<std::wstring>> parts(workers);
    {
        std::vector<std::jthread> pool;
        for (unsigned w = 0; w < workers; ++w)
            pool.emplace_back([&, w] {
                for (size_t i = w; i < checks.size(); i += workers) {
                    if ((i & 1023) == w && st.stop_requested()) return;
                    WIN32_FILE_ATTRIBUTE_DATA fa;
                    if (!GetFileAttributesExW(path::ToExtended(checks[i].path).c_str(), GetFileExInfoStandard, &fa)) continue;  // gone: its parent shows it
                    uint64_t ft = (static_cast<uint64_t>(fa.ftLastWriteTime.dwHighDateTime) << 32) | fa.ftLastWriteTime.dwLowDateTime;
                    // stored time 0 = unknown (e.g. a drive root): cheaper to re-list that one folder than to guess
                    if (checks[i].modifiedSecs == 0 || ft / 10000000ull != checks[i].modifiedSecs) parts[w].push_back(checks[i].path);
                }
            });
    }  // joined here
    std::vector<std::wstring> changed;
    for (auto& p : parts) changed.insert(changed.end(), std::make_move_iterator(p.begin()), std::make_move_iterator(p.end()));
    return changed;
}

}  // namespace ss
