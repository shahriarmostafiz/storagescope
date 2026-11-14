#pragma once
#include <string>
#include <unordered_set>
#include <vector>

namespace ss {

// A list that is rebuilt (for example after an automatic refresh) keeps the user's ticks: rows are matched by key, so a row
// that is still there stays ticked, a new row starts unticked, and a row that disappeared is simply dropped.
inline std::vector<bool> CarryTicks(const std::vector<std::wstring>& oldKeys, const std::vector<bool>& oldTicked,
                                    const std::vector<std::wstring>& newKeys) {
    std::unordered_set<std::wstring> on;
    for (size_t i = 0; i < oldKeys.size() && i < oldTicked.size(); ++i)
        if (oldTicked[i]) on.insert(oldKeys[i]);
    std::vector<bool> out(newKeys.size());
    for (size_t i = 0; i < newKeys.size(); ++i) out[i] = on.count(newKeys[i]) != 0;
    return out;
}

}  // namespace ss
