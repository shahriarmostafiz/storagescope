#include "scanner/ScanStats.h"

#include <windows.h>

#include <algorithm>
#include <queue>

namespace ss {
namespace {

struct Bigger {  // makes a min-heap: the smallest of the current top N is on top and gets replaced
    bool operator()(const TopItem& a, const TopItem& b) const { return a.size > b.size; }
};
using TopHeap = std::priority_queue<TopItem, std::vector<TopItem>, Bigger>;

void Offer(TopHeap& h, size_t n, ScanNode* node, uint64_t size) {
    if (size == 0) return;
    if (h.size() < n) h.push({node, size});
    else if (size > h.top().size) {
        h.pop();
        h.push({node, size});
    }
}

std::vector<TopItem> Drain(TopHeap& h) {
    std::vector<TopItem> v;
    while (!h.empty()) {
        v.push_back(h.top());
        h.pop();
    }
    std::reverse(v.begin(), v.end());  // largest first
    return v;
}

}  // namespace

ScanStats ComputeStats(ScanNode* root, bool includeHidden, bool includeSystem, size_t topN) {
    ScanStats st;
    TopHeap folders, files;
    std::vector<ScanNode*> stack{root};
    while (!stack.empty()) {
        ScanNode* n = stack.back();
        stack.pop_back();
        if (!n->dir) continue;
        uint64_t ownBytes = 0;  // files directly inside, counted only if they pass the same hidden/system filter as everywhere else
        for (auto& c : n->dir->children) {
            if (c.removed) continue;
            if (!includeHidden && (c.attrs & FILE_ATTRIBUTE_HIDDEN)) continue;
            if (!includeSystem && (c.attrs & FILE_ATTRIBUTE_SYSTEM)) continue;
            if (c.type == NodeType::Directory) {
                stack.push_back(&c);
            } else if (c.type == NodeType::File) {
                int cat = static_cast<int>(Categorize(c.name));
                st.bytes[cat] += c.size;
                ++st.files[cat];
                ownBytes += c.size;
                Offer(files, topN, &c, c.size);
            }
        }
        if (n->type == NodeType::Directory) Offer(folders, topN, n, ownBytes);
    }
    st.topFolders = Drain(folders);
    st.topFiles = Drain(files);
    return st;
}

}  // namespace ss
