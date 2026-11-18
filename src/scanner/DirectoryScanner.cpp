#include "scanner/DirectoryScanner.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "filesystem/DriveManager.h"
#include "filesystem/PathUtils.h"
#include "scanner/DirectoryEnum.h"

namespace ss {
namespace {

struct Work {
    ScanNode* node;
    uint32_t cluster;
    uint32_t root;  // index of the scan root this folder belongs to (each root has its own queue and thread limit)
};

// Adds to a counter that other threads may read or add to at the same time.
template <class T>
void AtomicAdd(T& field, T v) {
    std::atomic_ref<T>(field).fetch_add(v, std::memory_order_relaxed);
}

class Run {
public:
    explicit Run(std::stop_token st) : st_(st) {}

    std::stop_token st_;
    std::mutex m;
    std::condition_variable cv;  // (not the stop_token overloads: the VS2019 STL can lose a wake-up there; waits below poll instead)
    // One LIFO queue per scan root (depth-first: small queue, better locality on HDDs) and a cap on how many threads may work
    // inside one root at the same time: a spinning disk is read by ONE thread (parallel readers make its head jump around),
    // an SSD by several. Different roots (different disks) still run side by side.
    std::vector<std::vector<Work>> q;
    std::vector<unsigned> limit, active, maxLimit;  // maxLimit: what a root may be raised to once its listings prove fast
    uint32_t testDelayUs = 0;
    std::function<void(int, uint32_t)> testHook;  // see ScanOptions::testDiskHook
    bool guardsOff = false;
    size_t outstanding = 0;
    std::atomic<uint64_t> files{0}, dirs{0}, bytes{0}, allocated{0}, errors{0}, reparse{0};
    // Slow-disk protection. (1) The exact size of a compressed/sparse file needs the file to be OPENED: on a hard drive that is one
    // seek per file (a fully compressed drive = hours). After a few seconds of such lookups the rest is estimated from the logical size.
    // (2) If listing folders turns out slow (a disk that does not say it is a hard drive, e.g. in a USB case), the root drops to one thread.
    std::atomic<uint64_t> lookupNs{0};
    std::atomic<bool> lookupOff{false};
    std::atomic<uint64_t> listNs[32]{}, listCount[32]{};
    std::atomic<bool> slowDisk{false};
    uint64_t lookupBudgetNs = 3000000000ull;  // 3 s of file opens, then estimate (60 s when every disk is an SSD: there it is cheap)
    static constexpr uint64_t kSlowFolderNs = 5000000ull;       // an average folder listing slower than 5 ms means a spinning disk
    static constexpr uint64_t kFastFolderNs = 1000000ull;       // faster than 1 ms: an SSD, so a root that started gentle may use all its threads

    // Needs `m`. Takes the newest folder of the root that has work, a free thread slot and the fewest threads busy right now: with
    // several drives they progress side by side (and all show growth while browsing the live scan) instead of one after the other.
    bool Pick(Work& w) {
        size_t best = q.size();
        for (size_t r = 0; r < q.size(); ++r) {
            if (q[r].empty() || active[r] >= limit[r]) continue;
            if (best == q.size() || active[r] < active[best]) best = r;
        }
        if (best == q.size()) return false;
        w = q[best].back();
        q[best].pop_back();
        ++active[best];
        return true;
    }
    bool Pickable() const {
        for (size_t r = 0; r < q.size(); ++r)
            if (!q[r].empty() && active[r] < limit[r]) return true;
        return false;
    }
    std::atomic<bool> aborted{false};
    std::vector<std::wstring> ignored;  // canonical; read-only while the workers run
    std::mutex pathM;
    std::wstring curPath;

    void WorkerLoop() {
        for (;;) {
            Work w;
            {
                std::unique_lock lk(m);
                while (!Pickable() && outstanding != 0 && !st_.stop_requested()) cv.wait_for(lk, std::chrono::milliseconds(50));
                if (st_.stop_requested()) {
                    if (outstanding) aborted = true;
                    return;
                }
                if (!Pickable()) return;  // outstanding == 0: all done
                Pick(w);
            }
            Process(w);
            {
                std::lock_guard lk(m);
                --active[w.root];
                --outstanding;
                cv.notify_all();  // a thread slot is free again (and maybe everything is done)
            }
        }
    }

    // Called when a directory (and, transitively, its whole subtree) is complete.
    static void Complete(ScanNode* n) {
        while (n) {
            DirData* d = n->dir.get();
            if (d->pending.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
            // atomic stores: a page browsing the live scan reads these with atomic loads (LiveSize & co.)
            std::atomic_ref<uint64_t>(n->size).store(d->size.load(), std::memory_order_relaxed);
            std::atomic_ref<uint64_t>(n->allocated).store(d->allocated.load(), std::memory_order_relaxed);
            std::atomic_ref<uint32_t>(n->files).store(d->files.load(), std::memory_order_relaxed);
            std::atomic_ref<uint32_t>(n->dirs).store(d->dirs.load(), std::memory_order_relaxed);
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
        dd.building.store(true, std::memory_order_relaxed);
        dd.pending.store(1);  // self token, released by Complete() below

        std::wstring base = path::ToExtended(dirPath);
        if (base.back() != L'\\') base += L'\\';
        auto fd = std::make_unique<WIN32_FIND_DATAW>();
        const auto t0 = std::chrono::steady_clock::now();
        uint64_t ioNs = 0;  // time spent opening files for exact sizes (not part of the listing speed)
        if (testHook) testHook(0, w.root);
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
            dd.building.store(false, std::memory_order_release);  // nothing inside: the (empty) listing is final
            Complete(node);
            return;
        }

        std::vector<ScanNode> kids;  // built privately and published in one step, so a browsing UI never sees half a folder
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
                    n.dir = std::make_unique<DirData>();  // exists before the node is visible; `building` stays true until it is listed
                    n.dir->building.store(true, std::memory_order_relaxed);
                    ++sub;
                }
            } else {
                n.type = NodeType::File;
                n.size = (static_cast<uint64_t>(fd->nFileSizeHigh) << 32) | fd->nFileSizeLow;
                const bool needsOpen = n.size && (n.attrs & (FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_SPARSE_FILE)) &&
                                       !(n.attrs & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS));
                if (needsOpen && !lookupOff.load(std::memory_order_relaxed)) {
                    auto a0 = std::chrono::steady_clock::now();
                    if (testHook) testHook(1, w.root);
                    n.allocated = AllocatedSizeOf(n.attrs, n.size, w.cluster, base, nm, true);
                    uint64_t d = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - a0).count());
                    ioNs += d;
                    if (lookupNs.fetch_add(d, std::memory_order_relaxed) + d > lookupBudgetNs) lookupOff = true;
                } else {
                    n.allocated = AllocatedSizeOf(n.attrs, n.size, w.cluster, base, nm, false);
                }
                ownSize += n.size;
                ownAlloc += n.allocated;
                ++ownFiles;
            }
        } while (FindNextFileW(h, fd.get()));
        DWORD le = GetLastError();
        FindClose(h);

        if (testDelayUs) std::this_thread::sleep_for(std::chrono::microseconds(testDelayUs));
        {  // how fast do folders list on this disk? (only the first few hundred / thousand folders are looked at)
            uint64_t total = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
            uint64_t listing = total > ioNs ? total - ioNs : 0;
            uint32_t r = w.root & 31;
            uint64_t ns = listNs[r].fetch_add(listing, std::memory_order_relaxed) + listing;
            uint64_t cnt = listCount[r].fetch_add(1, std::memory_order_relaxed) + 1;
            if (!guardsOff && (cnt == 200 || cnt == 2000) && w.root < limit.size()) {
                uint64_t avg = ns / cnt;
                std::lock_guard lk(m);
                if (avg > kSlowFolderNs && limit[w.root] > 1) {
                    limit[w.root] = maxLimit[w.root] = 1;
                    slowDisk = true;
                } else if (avg < kFastFolderNs && limit[w.root] < maxLimit[w.root]) {
                    limit[w.root] = maxLimit[w.root];  // it was only unknown, not slow
                    cv.notify_all();
                }
            }
        }

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
        allocated += ownAlloc;
        reparse += skipped;
        dirs++;

        // What was just listed counts for this folder and every folder above it right away, so a UI that browses the tree
        // during the scan sees sizes grow. (Complete() later writes the exact totals, which are the same numbers.)
        for (ScanNode* a = node; a; a = a->parent) {
            AtomicAdd<uint64_t>(a->size, ownSize);
            AtomicAdd<uint64_t>(a->allocated, ownAlloc);
            AtomicAdd<uint32_t>(a->files, ownFiles);
            AtomicAdd<uint32_t>(a->dirs, sub);
        }

        dd.children = std::move(kids);  // the vector's buffer (and so every child's address) does not change from here on
        for (auto& k : dd.children) k.parent = node;  // (already set while listing; the move keeps it valid)
        dd.building.store(false, std::memory_order_release);

        dd.pending.fetch_add(sub);
        if (sub) {
            {
                std::lock_guard lk(m);
                for (auto& k : dd.children)
                    if (k.type == NodeType::Directory) q[w.root].push_back({&k, w.cluster, w.root});
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
        n->dir->building.store(true, std::memory_order_relaxed);
        return n;
    };

    run.q.resize(roots.size());
    if (roots.size() == 1) {
        res->root = newRoot(roots[0]);
        run.q[0].push_back({res->root.get(), ClusterSizeOf(roots[0]), 0});
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
            k.dir = std::make_unique<DirData>();
            k.dir->building.store(true, std::memory_order_relaxed);
        }
        for (size_t i = 0; i < roots.size(); ++i) run.q[i].push_back({&kids[i], ClusterSizeOf(roots[i]), static_cast<uint32_t>(i)});
        res->root = std::move(v);
        if (roots.empty()) res->root->dir->pending.store(0);
    }
    for (auto& q : run.q) run.outstanding += q.size();

    // Threads per disk: a spinning disk is read by one thread (unless the user fixed a worker count), an SSD by several.
    unsigned hw = std::max(2u, std::thread::hardware_concurrency());
    unsigned n = std::max(1u, opt.workers ? opt.workers : std::min(8u, std::max(4u, hw / 2)));
    run.limit.assign(roots.size(), n);
    run.testDelayUs = opt.testListDelayUs;
    run.testHook = opt.testDiskHook;
    run.guardsOff = opt.testDisableGuards;
    run.active.assign(roots.size(), 0);
    bool anyHdd = false, expectedKnown = !roots.empty(), allSsd = !roots.empty();
    uint64_t used = 0;
    unsigned hddRoots = 0, sumLimit = 0;
    for (size_t i = 0; i < roots.size(); ++i) {
        const std::wstring& r = roots[i];
        if (path::IsDriveRoot(r)) {
            MediaKind mk = DriveManager::DetectMedia(static_cast<wchar_t>(towupper(r[0])));
            if (mk != MediaKind::Ssd) allSsd = false;
            if (mk == MediaKind::Hdd) {
                anyHdd = true;
                ++hddRoots;
                run.limit[i] = opt.workers ? n : 1;
            } else if (mk == MediaKind::Unknown) {
                run.limit[i] = std::min(n, 2u);  // USB cases and some RAID/virtual disks do not say: be gentle until the listing speed shows it
            }
            ULARGE_INTEGER freeToCaller{}, total{}, freeAll{};
            if (GetDiskFreeSpaceExW(r.c_str(), &freeToCaller, &total, &freeAll) && total.QuadPart >= freeAll.QuadPart)
                used += total.QuadPart - freeAll.QuadPart;
            else
                expectedKnown = false;
        } else {
            allSsd = false;
            expectedKnown = false;  // a folder scan: nothing tells how big it will be
            run.limit[i] = std::min(n, 4u);
        }
        sumLimit += run.limit[i];
    }
    if (allSsd) run.lookupBudgetNs = 60000000000ull;
    if (opt.testDisableGuards) run.lookupBudgetNs = ~0ull;
    if (opt.testAssumeHdd) {
        for (auto& l : run.limit) l = 1;
        anyHdd = true;
        sumLimit = static_cast<unsigned>(roots.size());
    }
    run.maxLimit = run.limit;
    for (size_t i = 0; i < roots.size(); ++i) {  // a root that only started gentle (type unknown) may be raised later
        if (path::IsDriveRoot(roots[i]) && DriveManager::DetectMedia(static_cast<wchar_t>(towupper(roots[i][0]))) == MediaKind::Unknown) run.maxLimit[i] = n;
        sumLimit += run.maxLimit[i] - run.limit[i];
    }
    uint64_t expected = expectedKnown ? static_cast<uint64_t>(used * (opt.expectedScale > 0.1 ? opt.expectedScale : 1.0)) : 0;
    res->expected = expected;
    if (cb.onStarted) cb.onStarted(res.get());  // the roots exist and nothing runs yet: the UI may start browsing

    unsigned threads = std::max(1u, std::min(sumLimit, std::max(n, hddRoots)));
    std::vector<std::jthread> pool;
    if (run.outstanding)
        for (unsigned i = 0; i < threads; ++i) pool.emplace_back([&run] { run.WorkerLoop(); });

    for (;;) {
        {
            std::unique_lock lk(run.m);
            run.cv.wait_for(lk, std::chrono::milliseconds(100), [&] { return run.outstanding == 0 || st.stop_requested(); });
            if (run.outstanding == 0 || st.stop_requested()) break;
        }
        if (cb.onProgress) {
            ScanProgress p;
            p.files = run.files;
            p.dirs = run.dirs;
            p.bytes = run.bytes;
            p.allocated = run.allocated;
            p.expected = expected;
            p.hdd = anyHdd || run.slowDisk.load();
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
    res->allocated = run.allocated;
    res->slowDisk = anyHdd || run.slowDisk.load();
    res->estimatedSizes = run.lookupOff.load();
    res->errors = run.errors;
    res->skippedReparse = run.reparse;
    res->elapsedMs = elapsed();
    res->canceled = run.aborted || (st.stop_requested() && run.outstanding != 0);
    if (cb.onFinished) cb.onFinished(std::move(res));
}

}  // namespace ss
