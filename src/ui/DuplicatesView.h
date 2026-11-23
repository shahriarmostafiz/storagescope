#pragma once
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "scanner/DuplicateFinder.h"
#include "ui/View.h"

namespace ss {

// "Duplicate Files" page: files with exactly the same content, grouped. The first file of each group (the oldest) is the original
// and is kept; the others are ticked for removal and say which file they are a copy of. Results hold paths, not scan nodes, so an
// auto-update of the scan never invalidates them; paths are looked up in the tree only when something is deleted.
class DuplicatesView : public View {
public:
    void OnCreate() override;
    void Layout(int w, int h) override;
    LRESULT OnMessage(UINT m, WPARAM w, LPARAM l) override;
    void OnShow() override;
    void Reload() override;
    void OnBusyChanged() override { UpdateButtons(); }

private:
    struct Item {
        std::wstring path;
        uint64_t size = 0, modified = 0;
        uint64_t writeTime = 0;  // as the search read it: a different time on disk later means the file changed
    };
    struct Group {
        std::vector<Item> items;  // items[0] is the original (kept); the rest are the copies
    };
    struct Row {
        uint32_t g, i;
    };
    struct Job {
        uint64_t id = 0;
        std::vector<DupInput> in;
        std::vector<std::vector<uint32_t>> out;
        std::vector<DupStamp> stamps;
        DupProgress prog;
        uint64_t minSize = 0;
    };

    void StartFind();
    void StopFind();
    void OnFound();
    void Fill();            // rebuild rows_ from groups_ and refresh the list and texts
    void Prune();           // drop files that were deleted (by this page or elsewhere) since the groups were found
    void UpdateButtons();
    void ShowProgress();
    void ToggleAll();
    void KeepThis();
    void Act(int id);
    bool IsOriginal(const Row& r) const { return r.i == 0; }
    std::vector<std::wstring> Selected() const;  // ticked copies, else highlighted copies (an original is never included)
    std::wstring CellText(const Row& r, int col) const;
    uint64_t MinSize() const;

    HWND lblSize_ = nullptr, cbSize_ = nullptr, lblType_ = nullptr, cbType_ = nullptr, bFind_ = nullptr, list_ = nullptr, info_ = nullptr,
         empty_ = nullptr, bOpen_ = nullptr, bKeep_ = nullptr, bRecycle_ = nullptr, bPerm_ = nullptr, note_ = nullptr, bSelAll_ = nullptr;
    std::vector<Group> groups_;
    std::vector<Row> rows_;
    std::unordered_set<std::wstring> checked_;  // ticked copies (by path)
    std::wstring emptyText_;
    uint64_t rev_ = 0, jobSeq_ = 0, skipped_ = 0;  // skipped_: files of the last search that could not be checked
    bool stale_ = false, barShown_ = false, pruneAfterDelete_ = false;
    int lastW_ = 0, lastH_ = 0;
    std::shared_ptr<Job> job_;  // the running search (null when idle)
    std::jthread th_;           // declared last: stopped and joined first
};

}  // namespace ss
