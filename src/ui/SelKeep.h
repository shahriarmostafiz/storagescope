#pragma once
#include <windows.h>

#include <commctrl.h>

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "scanner/ScanNode.h"

namespace ss {

// Keeps a list's ticked rows and highlighted rows when the list is rebuilt behind the user's back (auto-update, refresh,
// a changed scan tree). Rows are remembered by full path, because the nodes themselves may be rebuilt.
//   Remember(...)  before the rows or the tree change (node pointers still valid)
//   Restore(...)   after the new rows are in place and the list has its item count
class SelKeep {
public:
    bool Pending() const { return pending_; }

    // allowAll: "Select all" was used and the rows were only refreshed (not filtered differently): select the new rows too.
    void Remember(const std::vector<ScanNode*>& rows, const std::unordered_set<const ScanNode*>& checked, HWND list, bool allowAll) {
        if (pending_) return;  // already remembered (a DropReferences, then a Reload on its heels): the rows are empty by now, keep the first memory
        pending_ = true;
        all_ = false;
        ticked_.clear();
        picked_.clear();
        if (allowAll && !rows.empty() && checked.size() == rows.size()) {
            all_ = true;
        } else if (!checked.empty() && checked.size() <= kMax) {
            for (ScanNode* n : rows)
                if (!n->removed && checked.count(n)) ticked_.push_back(BuildPath(n));
        }
        if (!list) return;
        for (int i = ListView_GetNextItem(list, -1, LVNI_SELECTED); i >= 0 && picked_.size() < kMax; i = ListView_GetNextItem(list, i, LVNI_SELECTED))
            if (i < static_cast<int>(rows.size()) && !rows[static_cast<size_t>(i)]->removed) picked_.push_back(BuildPath(rows[static_cast<size_t>(i)]));
    }

    void Restore(const std::vector<ScanNode*>& rows, std::unordered_set<const ScanNode*>& checked, HWND list) {
        if (!pending_) return;
        pending_ = false;
        if (all_) {
            checked.clear();
            checked.insert(rows.begin(), rows.end());
        }
        if (ticked_.empty() && picked_.empty()) return;
        std::unordered_set<std::wstring> wantTick(ticked_.begin(), ticked_.end()), wantPick(picked_.begin(), picked_.end());
        for (size_t i = 0; i < rows.size(); ++i) {
            if (wantTick.empty() && wantPick.empty()) break;
            std::wstring p = BuildPath(rows[i]);
            if (wantTick.erase(p)) checked.insert(rows[i]);
            if (wantPick.erase(p) && list) ListView_SetItemState(list, static_cast<int>(i), LVIS_SELECTED, LVIS_SELECTED);
        }
        ticked_.clear();
        picked_.clear();
    }

private:
    static constexpr size_t kMax = 3000;  // beyond this the paths cost more than they are worth ("Select all" is kept as a flag)
    bool pending_ = false, all_ = false;
    std::vector<std::wstring> ticked_, picked_;
};

}  // namespace ss
