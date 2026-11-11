#include "filesystem/ChangeWatcher.h"

#include <algorithm>
#include <memory>

#include "filesystem/PathUtils.h"

namespace ss {
namespace {
constexpr size_t kMaxDirty = 200000;
constexpr DWORD kFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE |
                          FILE_NOTIFY_CHANGE_CREATION;

struct Watch {
    std::wstring root;
    HANDLE dir = INVALID_HANDLE_VALUE;
    HANDLE ev = nullptr;
    OVERLAPPED ov{};
    std::vector<DWORD> buf;  // DWORD-aligned as ReadDirectoryChangesW requires
    bool armed = false;
};

std::wstring LowerKey(const std::wstring& s) {
    std::wstring o = s;
    for (auto& c : o) c = c < 128 ? (c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c + 32) : c) : static_cast<wchar_t>(towlower(c));
    return o;
}
}  // namespace

void ChangeWatcher::Start(const std::vector<std::wstring>& roots, const std::vector<std::wstring>& ignored, Notify notify, DWORD bufferBytes) {
    Stop();
    {
        std::lock_guard lk(m_);
        dirty_.clear();
    }
    overflow_ = false;
    notified_ = false;
    notify_ = std::move(notify);
    ignored_.clear();
    for (const auto& s : ignored) {
        std::wstring c = path::Canonicalize(s);
        if (!c.empty()) ignored_.push_back(c);
    }
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    running_ = true;
    thread_ = std::jthread([this, roots, bufferBytes](std::stop_token) { Loop(roots, bufferBytes); });
}

void ChangeWatcher::Stop() {
    if (!stop_) return;
    SetEvent(stop_);
    if (thread_.joinable()) thread_.join();
    CloseHandle(stop_);
    stop_ = nullptr;
    running_ = false;
}

void ChangeWatcher::Add(const std::wstring& p) {
    for (const auto& ig : ignored_)
        if (path::IsSameOrUnder(p, ig)) return;
    bool first = false;
    {
        std::lock_guard lk(m_);
        if (dirty_.size() >= kMaxDirty) {
            dirty_.clear();
            overflow_ = true;  // too much to track one by one: the caller re-checks everything
            return;
        }
        first = dirty_.emplace(LowerKey(p), p).second;
    }
    if (first && !notified_.exchange(true) && notify_) notify_();
}

std::vector<std::wstring> ChangeWatcher::TakeDirty(size_t max) {
    std::vector<std::wstring> out;
    std::lock_guard lk(m_);
    for (auto it = dirty_.begin(); it != dirty_.end() && out.size() < max;) {
        out.push_back(it->second);
        it = dirty_.erase(it);
    }
    if (dirty_.empty()) notified_ = false;  // the next new change notifies again
    return out;
}

bool ChangeWatcher::TakeOverflow() { return overflow_.exchange(false); }

size_t ChangeWatcher::Pending() const {
    std::lock_guard lk(m_);
    return dirty_.size();
}

void ChangeWatcher::Loop(std::vector<std::wstring> roots, DWORD bufferBytes) {
    std::vector<std::unique_ptr<Watch>> watches;
    for (const auto& r : roots) {
        auto w = std::make_unique<Watch>();
        w->root = r;
        w->dir = CreateFileW(path::ToExtended(r).c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (w->dir == INVALID_HANDLE_VALUE) {
            overflow_ = true;  // a root we cannot watch means we cannot promise the results are current
            continue;
        }
        w->ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        w->ov.hEvent = w->ev;
        w->buf.resize(std::max<DWORD>(bufferBytes, 4096) / sizeof(DWORD));
        watches.push_back(std::move(w));
    }
    auto arm = [&](Watch& w) {
        ResetEvent(w.ev);
        w.armed = ReadDirectoryChangesW(w.dir, w.buf.data(), static_cast<DWORD>(w.buf.size() * sizeof(DWORD)), TRUE, kFilter, nullptr, &w.ov,
                                        nullptr) != 0;
        if (!w.armed) overflow_ = true;
    };
    for (auto& w : watches) arm(*w);

    std::vector<HANDLE> waits;
    for (auto& w : watches) waits.push_back(w->ev);
    waits.push_back(stop_);
    while (watches.size() > 0) {
        DWORD r = WaitForMultipleObjects(static_cast<DWORD>(waits.size()), waits.data(), FALSE, INFINITE);
        if (r < WAIT_OBJECT_0 || r >= WAIT_OBJECT_0 + waits.size() - 1) break;  // stop event (or failure)
        Watch& w = *watches[r - WAIT_OBJECT_0];
        DWORD bytes = 0;
        if (!GetOverlappedResult(w.dir, &w.ov, &bytes, FALSE) || bytes == 0) {
            overflow_ = true;  // ERROR_NOTIFY_ENUM_DIR / zero bytes = the kernel buffer overflowed and events were lost
        } else {
            auto* rec = reinterpret_cast<const BYTE*>(w.buf.data());
            for (;;) {
                auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(rec);
                std::wstring rel(info->FileName, info->FileNameLength / sizeof(wchar_t));
                std::wstring full = path::Join(w.root, rel);
                Add(path::Parent(full));  // the folder that now looks different
                if (!info->NextEntryOffset) break;
                rec += info->NextEntryOffset;
            }
        }
        arm(w);
    }
    for (auto& w : watches) {
        if (w->armed) {
            CancelIoEx(w->dir, &w->ov);
            DWORD b;
            GetOverlappedResult(w->dir, &w->ov, &b, TRUE);  // wait for the cancelled read so the buffer can be freed
        }
        CloseHandle(w->dir);
        CloseHandle(w->ev);
    }
    running_ = false;
}

}  // namespace ss
