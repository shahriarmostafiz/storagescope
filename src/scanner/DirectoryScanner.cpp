#include "scanner/DirectoryScanner.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "filesystem/PathUtils.h"
#include "scanner/DirectoryEnum.h"

namespace ss {
namespace {

struct Work {
    ScanNode* node;
    uint32_t cluster;
};

class Run {
public:
    explicit Run(std::stop_token st) : st_(st) {}

    std::stop_token st_;
    std::mutex m;
    std::condition_variable_any cv;
    std::vector<Work> q;  // LIFO => depth-first, small queue, better locality on HDDs
    size_t outstanding = 0;
    std::atomic<uint64_t> files{0}, dirs{0}, bytes{0}, errors{0}, reparse{0};
    std::atomic<bool> aborted{false};
    std::vector<std::wstring> ignored;  // canonical; read-only while the workers run
    std::mutex pathM;
    std::wstring curPath;

    void WorkerLoop() {
        for (;;) {
            Work w;
            {
                std::unique_lock lk(m);
                bool ok = cv.wait(lk, st_, [&] { return !q.empty() || outstanding == 0; });
                if (!ok) {
                    if (outstanding) aborted = true;
                    return;
                }
                if (q.empty()) return;  // outstanding == 0: all done
                if (st_.stop_requested()) {  // the wait predicate is already true when work is queued, so check explicitly
                    aborted = true;
                    return;
                }
                w = q.back();
                q.pop_back();
            }
            Process(w);
            {
                std::lock_guard lk(m);
                if (--outstanding == 0) cv.notify_all();
            }
        }
    }

    // Called when a directory (and, transitively, its whole subtree) is complete.
    static void Complete(ScanNode* n) {
        while (n) {
            DirData* d = n->dir.get();
            if (d->pending.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
            n->size = d->size.load();
            n->allocated = d->allocated.load();
            n->files = d->files.load();
            n->dirs = d->dirs.load();
            ScanNode* p = n->parent;
            if (!p) return;
            DirData* pd = p->dir.get();
            pd->size += n->size;
            pd->allocated += n->allocated;
            pd->files += n->files;
            pd->dirs += n->dirs + (n->type == NodeType::Directory ? 1u : 0u);
            n = p;
        }
    }

    void Process(const Work& w) {
        ScanNode* node = w.node;
        std::wstring dirPath = BuildPath(node);
        {
            std::unique_lock lk(pathM, std::try_to_lock);
            if (lk.owns_lock()) curPath = dirPath;
        }
        if (!node->dir) node->dir = std::make_unique<DirData>();
        DirData& dd = *node->dir;
        dd.pending.store(1);  // self token, released by Complete() below

        std::wstring base = path::ToExtended(dirPath);
        if (base.back() != L'\\') base += L'\\';
        auto fd = std::make_unique<WIN32_FIND_DATAW>();
        HANDLE h = FindFirstFileExW((base + L"*").c_str(), FindExInfoBasic, fd.get(), FindExSearchNameMatch, nullptr,
                                    FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) {
            DWORD e = GetLastError();
            if (e != ERROR_FILE_NOT_FOUND) {  // FILE_NOT_FOUND == empty volume root
                node->status = (e == ERROR_ACCESS_DENIED || e == ERROR_PRIVILEGE_NOT_HELD || e == ERROR_SHARING_VIOLATION)
                                   ? NodeStatus::AccessDenied
                                   : NodeStatus::Unavailable;
                errors++;
            }
            dirs++;
            Complete(node);
            return;
        }

        auto& kids = dd.children;
        uint64_t ownSize = 0, ownAlloc = 0;
        uint32_t ownFiles = 0, sub = 0, counter = 0, skipped = 0;
        bool stopped = false;
        do {
            const wchar_t* nm = fd->cFileName;
            if (nm[0] == L'.' && (nm[1] == 0 || (nm[1] == L'.' && nm[2] == 0))) continue;
            if ((++counter & 255) == 0 && st_.stop_requested()) {
                stopped = true;
                break;
            }
            if (!ignored.empty() && (fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {  // user's ignore list
                std::wstring cp = dirPath;
                if (cp.back() != L'\\') cp += L'\\';
                cp += nm;
                bool skip = false;
                for (const auto& ig : ignored)
                    if (path::EqualsNoCase(cp, ig)) {
                        skip = true;
                        break;
                    }
                if (skip) continue;
            }
            kids.emplace_back();
            ScanNode& n = kids.back();
            n.name.assign(nm);
            n.parent = node;
            n.attrs = fd->dwFileAttributes;
            n.modified = (static_cast<uint64_t>(fd->ftLastWriteTime.dwHighDateTime) << 32) | fd->ftLastWriteTime.dwLowDateTime;
            if (n.attrs & FILE_ATTRIBUTE_DIRECTORY) {
                if (n.attrs & FILE_ATTRIBUTE_REPARSE_POINT) {  // never followed: avoids loops / double counting
                    n.type = NodeType::Reparse;
                    DWORD tag = fd->dwReserved0;
                    n.status = tag == IO_REPARSE_TAG_MOUNT_POINT ? NodeStatus::Junction
                               : tag == IO_REPARSE_TAG_SYMLINK   ? NodeStatus::Symlink
                                                                 : NodeStatus::ReparseOther;
                    ++skipped;
                } else {
                    n.type = NodeType::Directory;
                    ++sub;
                }
            } else {
                n.type = NodeType::File;
                n.size = (static_cast<uint64_t>(fd->nFileSizeHigh) << 32) | fd->nFileSizeLow;
                n.allocated = AllocatedSizeOf(n.attrs, n.size, w.cluster, base, nm);
                ownSize += n.size;
                ownAlloc += n.allocated;
                ++ownFiles;
            }
        } while (FindNextFileW(h, fd.get()));
        DWORD le = GetLastError();
        FindClose(h);

        if (stopped) {
            aborted = true;
            return;  // partial tree is discarded by the caller
        }
        if (le != ERROR_NO_MORE_FILES) {  // device removed, network drop, ...
            node->status = NodeStatus::Unavailable;
            errors++;
        }
        if (kids.capacity() > kids.size() + kids.size() / 4) kids.shrink_to_fit();

        dd.size += ownSize;
        dd.allocated += ownAlloc;
        dd.files += ownFiles;
        files += ownFiles;
        bytes += ownSize;
        reparse += skipped;
        dirs++;

        dd.pending.fetch_add(sub);
        if (sub) {
            {
                std::lock_guard lk(m);
                for (auto& k : kids)
                    if (k.type == NodeType::Directory) q.push_back({&k, w.cluster});
                outstanding += sub;
            }
            cv.notify_all();
        }
        Complete(node);
    }
};

}  // namespace

void DirectoryScanner::Scan(const std::vector<std::wstring>& rootsIn, const ScanOptions& opt, ScanCallbacks cb,
                            std::stop_token st) {
    using clock = std::chrono::steady_clock;
    auto t0 = clock::now();
    auto elapsed = [&] {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count());
    };

    std::vector<std::wstring> roots;
    for (auto& r : rootsIn) {
        std::wstring c = path::Canonicalize(r);
        if (!c.empty() && std::find(roots.begin(), roots.end(), c) == roots.end()) roots.push_back(c);
    }

    auto res = std::make_unique<ScanResult>();
    Run run(st);
    for (const auto& s : opt.ignored) {
        std::wstring c = path::Canonicalize(s);
        if (!c.empty()) run.ignored.push_back(c);
    }
    auto stamp = [](ScanNode& n, const std::wstring& p) {  // a root is not part of any listing: read its own folder time
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExW(path::ToExtended(p).c_str(), GetFileExInfoStandard, &fa))
            n.modified = (static_cast<uint64_t>(fa.ftLastWriteTime.dwHighDateTime) << 32) | fa.ftLastWriteTime.dwLowDateTime;
    };
    auto newRoot = [&](const std::wstring& p) {
        auto n = std::make_unique<ScanNode>();
        stamp(*n, p);
        n->name = p;
        n->type = NodeType::Directory;
        n->scanRoot = true;
        n->dir = std::make_unique<DirData>();
        return n;
    };

    if (roots.size() == 1) {
        res->root = newRoot(roots[0]);
        run.q.push_back({res->root.get(), ClusterSizeOf(roots[0])});
    } else {
        auto v = std::make_unique<ScanNode>();
        v->name = L"This PC";
        v->type = NodeType::Virtual;
        v->dir = std::make_unique<DirData>();
        v->dir->pending.store(static_cast<uint32_t>(roots.size()));
        auto& kids = v->dir->children;
        for (auto& r : roots) {
            kids.emplace_back();
            ScanNode& k = kids.back();
            k.name = r;
            stamp(k, r);
            k.type = NodeType::Directory;
            k.scanRoot = true;
            k.parent = v.get();
        }
        for (size_t i = 0; i < roots.size(); ++i) run.q.push_back({&kids[i], ClusterSizeOf(roots[i])});
        res->root = std::move(v);
        if (roots.empty()) res->root->dir->pending.store(0);
    }
    run.outstanding = run.q.size();

    unsigned n = std::max(1u, opt.workers ? opt.workers : 4u);
    std::vector<std::jthread> pool;
    if (run.outstanding)
        for (unsigned i = 0; i < n; ++i) pool.emplace_back([&run] { run.WorkerLoop(); });

    for (;;) {
        {
            std::unique_lock lk(run.m);
            run.cv.wait_for(lk, st, std::chrono::milliseconds(100), [&] { return run.outstanding == 0; });
            if (run.outstanding == 0 || st.stop_requested()) break;
        }
        if (cb.onProgress) {
            ScanProgress p;
            p.files = run.files;
            p.dirs = run.dirs;
            p.bytes = run.bytes;
            p.errors = run.errors;
            p.elapsedMs = elapsed();
            {
                std::lock_guard lk(run.pathM);
                p.currentPath = run.curPath;
            }
            cb.onProgress(p);
        }
    }
    pool.clear();  // joins every worker before we touch the tree

    res->files = run.files;
    res->dirs = run.dirs;
    res->bytes = run.bytes;
    res->errors = run.errors;
    res->skippedReparse = run.reparse;
    res->elapsedMs = elapsed();
    res->canceled = run.aborted || (st.stop_requested() && run.outstanding != 0);
    if (cb.onFinished) cb.onFinished(std::move(res));
}

}  // namespace ss
