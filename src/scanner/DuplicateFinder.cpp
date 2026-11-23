#include "scanner/DuplicateFinder.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <set>
#include <thread>
#include <unordered_map>

#include "filesystem/PathUtils.h"

namespace ss {
namespace {

constexpr uint64_t kEdge = 64 * 1024;  // the quick check reads this much from the start and from the end of a file
constexpr size_t kBuf = 1 << 20;
// Directories, links, and cloud placeholders (reading one would download it).
constexpr DWORD kNotPlain = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE | 0x00040000 /*RECALL_ON_OPEN*/ |
                            0x00400000 /*RECALL_ON_DATA_ACCESS*/;

// SHA-256 of a file (Windows CNG). One per worker thread: it owns a read buffer.
class Hasher {
public:
    Hasher() {
        if (BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) alg_ = nullptr;
        buf_.resize(kBuf);
    }
    ~Hasher() {
        if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
    }
    Hasher(const Hasher&) = delete;
    Hasher& operator=(const Hasher&) = delete;

    // edges=false: the whole file. edges=true: its first and last 64 KB (the whole file when it is at most 128 KB).
    // False when the file cannot be read, is not a plain local file, is not the size the scan recorded (it changed since),
    // or changes while it is being read. `stamp` receives what was seen of the file.
    bool Digest(const std::wstring& path, uint64_t size, bool edges, std::stop_token st, std::string& out, DupStamp& stamp) {
        if (!alg_) return false;
        const std::wstring ext = path::ToExtended(path);
        // Before opening: opening a cloud placeholder (OneDrive "online only") downloads it, and a link would be followed.
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (ext.empty() || !GetFileAttributesExW(ext.c_str(), GetFileExInfoStandard, &fa) || (fa.dwFileAttributes & kNotPlain)) return false;
        HANDLE f = CreateFileW(ext.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                               edges ? 0 : FILE_FLAG_SEQUENTIAL_SCAN, nullptr);  // the quick check reads two small pieces: no read-ahead
        if (f == INVALID_HANDLE_VALUE) return false;
        BCRYPT_HASH_HANDLE h = nullptr;
        bool ok = BCryptCreateHash(alg_, &h, nullptr, 0, nullptr, 0, 0) == 0;
        BY_HANDLE_FILE_INFORMATION before{}, after{};
        if (ok)
            ok = GetFileInformationByHandle(f, &before) && !(before.dwFileAttributes & kNotPlain) &&
                 ((static_cast<uint64_t>(before.nFileSizeHigh) << 32) | before.nFileSizeLow) == size;
        if (ok) {
            if (!edges) ok = Feed(f, h, size, st);
            else if (ok) {
                ok = Feed(f, h, std::min(size, kEdge), st);
                if (ok && size > kEdge) {
                    uint64_t tail = std::min(kEdge, size - kEdge);
                    LARGE_INTEGER p;
                    p.QuadPart = static_cast<LONGLONG>(size - tail);
                    ok = SetFilePointerEx(f, p, nullptr, FILE_BEGIN) && Feed(f, h, tail, st);
                }
            }
            // a write during the read gives a hash of a mix of old and new content
            ok = ok && GetFileInformationByHandle(f, &after) && CompareFileTime(&before.ftLastWriteTime, &after.ftLastWriteTime) == 0 &&
                 before.nFileSizeLow == after.nFileSizeLow && before.nFileSizeHigh == after.nFileSizeHigh;
            uint8_t d[32];
            if (ok && BCryptFinishHash(h, d, sizeof d, 0) == 0) out.assign(reinterpret_cast<const char*>(d), sizeof d);
            else ok = false;
        }
        if (h) BCryptDestroyHash(h);
        CloseHandle(f);
        if (ok) {
            stamp.writeTime = (static_cast<uint64_t>(before.ftLastWriteTime.dwHighDateTime) << 32) | before.ftLastWriteTime.dwLowDateTime;
            stamp.fileId = (static_cast<uint64_t>(before.nFileIndexHigh) << 32) | before.nFileIndexLow;
            stamp.volume = before.dwVolumeSerialNumber;
        }
        return ok;
    }

private:
    bool Feed(HANDLE f, BCRYPT_HASH_HANDLE h, uint64_t n, std::stop_token st) {
        while (n > 0) {
            if (st.stop_requested()) return false;
            DWORD want = static_cast<DWORD>(std::min<uint64_t>(n, buf_.size())), got = 0;
            if (!ReadFile(f, buf_.data(), want, &got, nullptr) || got != want) return false;  // short read: the file shrank
            if (BCryptHashData(h, buf_.data(), got, 0) != 0) return false;
            n -= got;
        }
        return true;
    }
    BCRYPT_ALG_HANDLE alg_ = nullptr;
    std::vector<uint8_t> buf_;
};

using Groups = std::vector<std::vector<uint32_t>>;

// Splits every group into the files whose digests are equal; only subgroups of two or more survive.
// Files are read in the order of `in` (the order of the scan tree, which follows the folders on the disk), not group by group:
// on a hard disk jumping between far-apart groups costs a head movement per file.
Groups Refine(const std::vector<DupInput>& in, Groups& groups, bool edges, int stage, int threads, std::stop_token st, DupProgress* prog,
              std::vector<DupStamp>& stamps) {
    struct Task {
        uint32_t idx, g;
    };
    std::vector<Task> tasks;
    for (uint32_t g = 0; g < groups.size(); ++g)
        for (uint32_t i : groups[g]) tasks.push_back({i, g});
    std::sort(tasks.begin(), tasks.end(), [](const Task& a, const Task& b) { return a.idx < b.idx; });
    if (prog) {
        prog->done = 0;
        prog->total = tasks.size();
        prog->stage = stage;
    }
    std::vector<std::string> digest(tasks.size());
    std::vector<char> ok(tasks.size(), 0);
    std::atomic<size_t> next{0};
    auto work = [&] {
        try {
            Hasher hs;
            while (!st.stop_requested()) {
                size_t t = next.fetch_add(1);
                if (t >= tasks.size()) return;
                const DupInput& f = in[tasks[t].idx];
                ok[t] = hs.Digest(f.path, f.size, edges, st, digest[t], stamps[tasks[t].idx]) ? 1 : 0;  // one task per file: no two threads share a slot
                if (prog) ++prog->done;
            }
        } catch (...) {  // out of memory: this thread stops, its files count as not checked (below) and the others carry on
        }
    };
    int n = std::clamp(threads, 1, 8);
    {
        std::vector<std::jthread> pool;
        for (int i = 1; i < n; ++i) pool.emplace_back(work);
        work();
    }  // the pool joins here
    if (prog && !st.stop_requested()) prog->skipped += static_cast<uint64_t>(std::count(ok.begin(), ok.end(), 0));
    std::vector<std::unordered_map<std::string, std::vector<uint32_t>>> by(groups.size());
    for (size_t t = 0; t < tasks.size(); ++t)
        if (ok[t]) by[tasks[t].g][digest[t]].push_back(tasks[t].idx);
    Groups out;
    for (auto& m : by)
        for (auto& kv : m)
            if (kv.second.size() > 1) out.push_back(std::move(kv.second));
    return out;
}

}  // namespace

std::vector<std::vector<uint32_t>> FindDuplicateGroups(const std::vector<DupInput>& in, int threads, std::stop_token st, DupProgress* prog,
                                                      std::vector<DupStamp>* stampsOut) {
    std::vector<DupStamp> stamps(in.size());
    std::unordered_map<uint64_t, std::vector<uint32_t>> bySize;
    for (uint32_t i = 0; i < in.size(); ++i)
        if (in[i].size > 0) bySize[in[i].size].push_back(i);
    Groups same;  // files that share a size: the only ones that can be identical
    for (auto& kv : bySize)
        if (kv.second.size() > 1) same.push_back(std::move(kv.second));
    bySize.clear();

    Groups quick = Refine(in, same, true, 1, threads, st, prog, stamps);
    Groups finals, big;  // a file of at most 128 KB was read completely by the quick check, so its group is already final
    for (auto& g : quick) (in[g[0]].size <= 2 * kEdge ? finals : big).push_back(std::move(g));
    if (!big.empty() && !st.stop_requested()) {
        Groups full = Refine(in, big, false, 2, threads, st, prog, stamps);
        for (auto& g : full) finals.push_back(std::move(g));
    }
    if (st.stop_requested()) return {};

    for (auto& g : finals)  // the file to keep comes first: the oldest, then the one with the shortest path
        std::sort(g.begin(), g.end(), [&](uint32_t a, uint32_t b) {
            if (in[a].modified != in[b].modified) return in[a].modified < in[b].modified;
            if (in[a].path.size() != in[b].path.size()) return in[a].path.size() < in[b].path.size();
            return in[a].path < in[b].path;
        });
    for (auto& g : finals) {  // two names of one file are not copies: keep the first name only
        std::vector<uint32_t> uniq;
        std::set<std::pair<uint32_t, uint64_t>> seen;
        for (uint32_t i : g)
            if (seen.insert({stamps[i].volume, stamps[i].fileId}).second) uniq.push_back(i);
        g = std::move(uniq);
    }
    finals.erase(std::remove_if(finals.begin(), finals.end(), [](const auto& g) { return g.size() < 2; }), finals.end());
    std::sort(finals.begin(), finals.end(), [&](const auto& a, const auto& b) {  // most reclaimable space first
        uint64_t wa = in[a[0]].size * (a.size() - 1), wb = in[b[0]].size * (b.size() - 1);
        return wa != wb ? wa > wb : in[a[0]].path < in[b[0]].path;
    });
    if (stampsOut) *stampsOut = std::move(stamps);
    return finals;
}

}  // namespace ss
