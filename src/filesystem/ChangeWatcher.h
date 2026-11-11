#pragma once
#include <windows.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ss {

// Watches whole directory trees with ReadDirectoryChangesW (no admin rights needed) and collects the *folders* in which something
// changed (a file or folder was added, removed, renamed or resized). The caller re-lists those folders and applies them to its tree.
class ChangeWatcher {
public:
    using Notify = std::function<void()>;  // called on the watcher thread when new dirty folders appear (post a message, do not do work)
    ~ChangeWatcher() { Stop(); }

    // Paths are canonical. Anything inside an `ignored` folder is never reported. Restarts if already running.
    void Start(const std::vector<std::wstring>& roots, const std::vector<std::wstring>& ignored, Notify notify, DWORD bufferBytes = 256 * 1024);
    void Stop();
    bool Running() const { return running_; }

    std::vector<std::wstring> TakeDirty(size_t max);  // removes and returns up to `max` changed folders
    bool TakeOverflow();                              // true once if events were lost: the caller must re-check everything
    size_t Pending() const;

private:
    void Loop(std::vector<std::wstring> roots, DWORD bufferBytes);
    void Add(const std::wstring& path);

    std::jthread thread_;
    HANDLE stop_ = nullptr;
    std::atomic<bool> running_{false}, overflow_{false}, notified_{false};
    Notify notify_;
    std::vector<std::wstring> ignored_;
    mutable std::mutex m_;
    std::unordered_map<std::wstring, std::wstring> dirty_;  // lower-case key -> path as reported
};

}  // namespace ss
