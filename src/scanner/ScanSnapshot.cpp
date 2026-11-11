#include "scanner/ScanSnapshot.h"

#include <windows.h>
#include <compressapi.h>

#include <algorithm>
#include <cstring>

#include "filesystem/PathUtils.h"

namespace ss {
namespace {

constexpr char kMagic[4] = {'S', 'S', 'C', 'N'};
constexpr uint32_t kVersion = 1;
constexpr size_t kBlockRaw = 1u << 20;             // 1 MB of records per compressed block
constexpr uint32_t kMaxRaw = 16u << 20;            // reader limits (untrusted file)
constexpr uint64_t kMaxNodes = 60'000'000;
constexpr size_t kMaxName = 65535;
constexpr size_t kMaxDepth = 200'000;
constexpr uint64_t kMaxSummaryStrings = 100;

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring FromUtf8(const char* p, size_t len) {
    if (!len) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, p, static_cast<int>(len), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, p, static_cast<int>(len), w.data(), n);
    return w;
}

// ------------------------------------------------------------ writing
class Writer {
public:
    explicit Writer(HANDLE f) : f_(f) { ok_ = CreateCompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &comp_) != 0; }
    ~Writer() {
        if (comp_) CloseCompressor(comp_);
    }
    bool ok() const { return ok_; }
    void Raw(const void* p, size_t n) {  // uncompressed header bytes
        DWORD w = 0;
        if (ok_ && (!WriteFile(f_, p, static_cast<DWORD>(n), &w, nullptr) || w != n)) ok_ = false;
    }
    void U32(uint32_t v) { Raw(&v, 4); }
    void U64(uint64_t v) { Raw(&v, 8); }
    void Str(const std::wstring& s) {
        std::string u = ToUtf8(s);
        U32(static_cast<uint32_t>(u.size()));
        Raw(u.data(), u.size());
    }
    // body bytes (compressed in blocks)
    void Byte(uint8_t b) {
        buf_.push_back(static_cast<char>(b));
        if (buf_.size() >= kBlockRaw) Flush();
    }
    void Var(uint64_t v) {
        while (v >= 0x80) {
            Byte(static_cast<uint8_t>(v | 0x80));
            v >>= 7;
        }
        Byte(static_cast<uint8_t>(v));
    }
    void Name(const std::wstring& s) {
        std::string u = ToUtf8(s);
        Var(u.size());
        for (char c : u) Byte(static_cast<uint8_t>(c));
    }
    void Flush() {
        if (buf_.empty() || !ok_) return;
        SIZE_T need = 0;
        Compress(comp_, buf_.data(), buf_.size(), nullptr, 0, &need);  // asks for the output size
        if (!need) {
            ok_ = false;
            return;
        }
        std::vector<char> out(need);
        SIZE_T used = 0;
        if (!Compress(comp_, buf_.data(), buf_.size(), out.data(), need, &used)) {
            ok_ = false;
            return;
        }
        U32(static_cast<uint32_t>(buf_.size()));
        U32(static_cast<uint32_t>(used));
        Raw(out.data(), used);
        buf_.clear();
    }
    void Finish() {
        Flush();
        U32(0);
        U32(0);  // terminator
    }

private:
    HANDLE f_;
    COMPRESSOR_HANDLE comp_ = nullptr;
    std::string buf_;
    bool ok_ = false;
};

uint64_t CountNodes(const ScanNode* root) {
    uint64_t n = 0;
    std::vector<const ScanNode*> st{root};
    while (!st.empty()) {
        const ScanNode* x = st.back();
        st.pop_back();
        ++n;
        if (x->dir)
            for (const auto& c : x->dir->children)
                if (!c.removed) st.push_back(&c);
    }
    return n;
}

void WriteSummaryTop(Writer& w, const std::vector<TopItem>& items) {
    w.U32(static_cast<uint32_t>(items.size()));
    for (const auto& t : items) {
        w.U64(t.size);
        w.Str(DisplayName(t.node));
        w.Str(path::Parent(BuildPath(t.node)));
    }
}

void WriteNode(Writer& w, const ScanNode& n) {
    w.Byte(static_cast<uint8_t>(n.type));
    w.Byte(static_cast<uint8_t>(static_cast<uint8_t>(n.status) | (n.scanRoot ? 0x80 : 0)));
    w.Var(n.attrs);
    w.Name(n.name);
    w.Var(n.modified / 10000000ull);  // seconds since 1601: plenty for change detection, 4-5 bytes instead of 8
    if (n.type == NodeType::File) {
        w.Var(n.size);
        w.Var(n.allocated);
    } else if (n.IsDir()) {
        uint64_t cnt = 0;
        if (n.dir)
            for (const auto& c : n.dir->children)
                if (!c.removed) ++cnt;
        w.Var(cnt);
    }
}

// ------------------------------------------------------------ reading
class Reader {
public:
    explicit Reader(HANDLE f) : f_(f) {}
    ~Reader() {
        if (dec_) CloseDecompressor(dec_);
    }
    bool Init() { return CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &dec_) != 0; }
    bool Raw(void* p, size_t n) {
        DWORD got = 0;
        return ReadFile(f_, p, static_cast<DWORD>(n), &got, nullptr) && got == n;
    }
    bool U32(uint32_t& v) { return Raw(&v, 4); }
    bool U64(uint64_t& v) { return Raw(&v, 8); }
    bool Str(std::wstring& s) {
        uint32_t n = 0;
        if (!U32(n) || n > kMaxName) return false;
        std::string u(n, '\0');
        if (n && !Raw(u.data(), n)) return false;
        s = FromUtf8(u.data(), u.size());
        return true;
    }
    bool Byte(uint8_t& b) {
        if (pos_ >= cur_.size() && !NextBlock()) return false;
        b = static_cast<uint8_t>(cur_[pos_++]);
        return true;
    }
    bool Var(uint64_t& v) {
        v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            uint8_t b;
            if (!Byte(b)) return false;
            v |= static_cast<uint64_t>(b & 0x7F) << shift;
            if (!(b & 0x80)) return true;
        }
        return false;  // over-long varint
    }
    bool Name(std::wstring& s) {
        uint64_t n = 0;
        if (!Var(n) || n > kMaxName) return false;
        std::string u(static_cast<size_t>(n), '\0');
        for (size_t i = 0; i < u.size(); ++i) {
            uint8_t b;
            if (!Byte(b)) return false;
            u[i] = static_cast<char>(b);
        }
        s = FromUtf8(u.data(), u.size());
        return true;
    }

private:
    bool NextBlock() {
        uint32_t raw = 0, comp = 0;
        if (!U32(raw) || !U32(comp) || raw == 0 || raw > kMaxRaw || comp == 0 || comp > kMaxRaw + (kMaxRaw >> 3) + 4096) return false;
        std::vector<char> in(comp);
        if (!Raw(in.data(), comp)) return false;
        cur_.resize(raw);
        SIZE_T out = 0;
        if (!Decompress(dec_, in.data(), comp, cur_.data(), raw, &out) || out != raw) return false;
        pos_ = 0;
        return true;
    }
    HANDLE f_;
    DECOMPRESSOR_HANDLE dec_ = nullptr;
    std::vector<char> cur_;
    size_t pos_ = 0;
};

bool ReadTop(Reader& r, std::vector<SavedTop>& out) {
    uint32_t n = 0;
    if (!r.U32(n) || n > kMaxSummaryStrings) return false;
    out.resize(n);
    for (auto& t : out)
        if (!r.U64(t.size) || !r.Str(t.name) || !r.Str(t.location)) return false;
    return true;
}

bool ReadHeader(Reader& r, SnapshotSummary& s) {
    char magic[4];
    uint32_t version = 0, nroots = 0;
    if (!r.Raw(magic, 4) || memcmp(magic, kMagic, 4) != 0 || !r.U32(version) || version != kVersion) return false;
    if (!r.U64(s.created) || !r.U64(s.files) || !r.U64(s.dirs) || !r.U64(s.bytes) || !r.U64(s.elapsedMs) || !r.U64(s.nodes) ||
        !r.U64(s.ignoredHash) || !r.U64(s.fileBytes) || !r.U32(nroots))
        return false;
    if (nroots > 64 || s.nodes == 0 || s.nodes > kMaxNodes) return false;
    s.roots.resize(nroots);
    for (auto& p : s.roots)
        if (!r.Str(p)) return false;
    for (int i = 0; i < kCategoryCount; ++i)
        if (!r.U64(s.catBytes[i]) || !r.U64(s.catFiles[i])) return false;
    return ReadTop(r, s.topFolders) && ReadTop(r, s.topFiles);
}

HANDLE OpenForRead(const std::wstring& p) {
    return CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
}

}  // namespace

uint64_t HashIgnored(const std::vector<std::wstring>& ignored) {
    uint64_t h = 1469598103934665603ull;  // FNV-1a over the lower-cased entries
    for (const auto& s : ignored) {
        for (wchar_t c : s) {
            h ^= static_cast<uint64_t>(towlower(c));
            h *= 1099511628211ull;
        }
        h ^= 0xFF;
        h *= 1099511628211ull;
    }
    return h;
}

bool SaveSnapshot(const ScanResult& scan, const std::vector<std::wstring>& roots, uint64_t ignoredHash, const ScanStats& stats,
                  const std::wstring& path) {
    if (!scan.root || scan.canceled) return false;
    std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    bool ok;
    {
        Writer w(f);
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        w.Raw(kMagic, 4);
        w.U32(kVersion);
        w.U64((static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime);
        w.U64(scan.root->files);
        w.U64(scan.root->dirs);
        w.U64(scan.root->size);
        w.U64(scan.elapsedMs);
        w.U64(CountNodes(scan.root.get()));
        w.U64(ignoredHash);
        w.U64(stats.TotalBytes());
        w.U32(static_cast<uint32_t>(roots.size()));
        for (const auto& r : roots) w.Str(r);
        for (int i = 0; i < kCategoryCount; ++i) {
            w.U64(stats.bytes[i]);
            w.U64(stats.files[i]);
        }
        WriteSummaryTop(w, stats.topFolders);
        WriteSummaryTop(w, stats.topFiles);

        struct Frame {
            const ScanNode* n;
            size_t i;
        };
        WriteNode(w, *scan.root);
        std::vector<Frame> st;
        if (scan.root->IsDir()) st.push_back({scan.root.get(), 0});
        while (!st.empty() && w.ok()) {
            const ScanNode* parent = st.back().n;
            size_t idx = st.back().i++;
            if (!parent->dir || idx >= parent->dir->children.size()) {
                st.pop_back();
                continue;
            }
            const ScanNode& c = parent->dir->children[idx];
            if (c.removed) continue;
            WriteNode(w, c);
            if (c.IsDir() && c.dir && !c.dir->children.empty()) st.push_back({&c, 0});
        }
        w.Finish();
        ok = w.ok();
    }
    CloseHandle(f);
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

bool ReadSnapshotSummary(const std::wstring& path, SnapshotSummary& out) {
    HANDLE f = OpenForRead(path);
    if (f == INVALID_HANDLE_VALUE) return false;
    Reader r(f);
    bool ok = r.Init() && ReadHeader(r, out);
    CloseHandle(f);
    return ok;
}

std::unique_ptr<ScanResult> LoadSnapshot(const std::wstring& path) {
    HANDLE f = OpenForRead(path);
    if (f == INVALID_HANDLE_VALUE) return nullptr;
    Reader r(f);
    SnapshotSummary s;
    auto res = std::make_unique<ScanResult>();
    bool ok = r.Init() && ReadHeader(r, s);

    std::vector<ScanNode*> dirsInOrder;  // pre-order; totals are summed over this list in reverse
    struct Frame {
        ScanNode* node;
        uint64_t remaining;
    };
    std::vector<Frame> st;
    uint64_t remainingNodes = ok ? s.nodes : 0;

    auto readNode = [&](ScanNode& n, ScanNode* parent, uint64_t& childCount) -> bool {
        uint8_t type = 0, status = 0;
        uint64_t attrs = 0, secs = 0;
        if (!r.Byte(type) || !r.Byte(status) || type > static_cast<uint8_t>(NodeType::Virtual) || (status & 0x7F) > static_cast<uint8_t>(NodeStatus::ReparseOther))
            return false;
        if (!r.Var(attrs) || attrs > 0xFFFFFFFFull || !r.Name(n.name) || !r.Var(secs) || secs > (1ull << 40)) return false;
        n.type = static_cast<NodeType>(type);
        n.status = static_cast<NodeStatus>(status & 0x7F);
        n.scanRoot = (status & 0x80) != 0;
        n.attrs = static_cast<uint32_t>(attrs);
        n.modified = secs * 10000000ull;
        n.parent = parent;
        childCount = 0;
        if (n.type == NodeType::File) {
            if (!r.Var(n.size) || !r.Var(n.allocated)) return false;
        } else if (n.IsDir()) {
            if (!r.Var(childCount) || childCount > remainingNodes) return false;
            n.dir = std::make_unique<DirData>();
        }
        return true;
    };

    if (ok) {
        res->root = std::make_unique<ScanNode>();
        uint64_t cc = 0;
        if (remainingNodes == 0 || !readNode(*res->root, nullptr, cc)) ok = false;
        else {
            --remainingNodes;
            if (res->root->IsDir()) {
                res->root->dir->children.reserve(static_cast<size_t>(cc));
                dirsInOrder.push_back(res->root.get());
                if (cc) st.push_back({res->root.get(), cc});
            }
        }
    }
    while (ok && !st.empty()) {
        if (st.size() > kMaxDepth || remainingNodes == 0) {
            ok = false;
            break;
        }
        Frame& top = st.back();
        ScanNode* parent = top.node;
        if (--top.remaining == 0) st.pop_back();  // `top` is dead after this line
        parent->dir->children.emplace_back();
        ScanNode& n = parent->dir->children.back();
        uint64_t cc = 0;
        if (!readNode(n, parent, cc)) {
            ok = false;
            break;
        }
        --remainingNodes;
        if (n.IsDir()) {
            dirsInOrder.push_back(&n);
            n.dir->children.reserve(static_cast<size_t>(cc));
            if (cc) st.push_back({&n, cc});
        }
    }
    if (ok && (remainingNodes != 0 || !st.empty())) ok = false;  // header and body disagree
    CloseHandle(f);
    if (!ok) return nullptr;

    for (auto it = dirsInOrder.rbegin(); it != dirsInOrder.rend(); ++it) {  // children before parents
        ScanNode* d = *it;
        uint64_t size = 0, alloc = 0;
        uint32_t files = 0, dirs = 0;
        for (const auto& c : d->dir->children) {
            if (c.type == NodeType::File) {
                size += c.size;
                alloc += c.allocated;
                ++files;
            } else if (c.type == NodeType::Directory) {
                size += c.size;
                alloc += c.allocated;
                files += c.files;
                dirs += c.dirs + 1;
            }
        }
        d->size = size;
        d->allocated = alloc;
        d->files = files;
        d->dirs = dirs;
    }
    res->files = res->root->files;
    res->dirs = res->root->dirs;
    res->bytes = res->root->size;
    res->elapsedMs = s.elapsedMs;
    res->canceled = false;
    return res;
}

}  // namespace ss
