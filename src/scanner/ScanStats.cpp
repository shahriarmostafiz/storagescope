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

AppDataInfo ComputeAppData(const ScanNode* root, size_t topN) {
    AppDataInfo info;
    std::vector<const ScanNode*> roots;
    FindAppDataRoots(root, roots);
    auto addFolders = [&](const ScanNode* parent, const wchar_t* where) {
        if (!parent->dir) return;
        for (const auto& c : parent->dir->children)
            if (!c.removed && c.type == NodeType::Directory && c.size >= (1ull << 20)) info.top.push_back({&c, c.size, c.name + L" (" + where + L")"});
    };
    for (const ScanNode* r : roots) {
        info.total += r->size;
        if (_wcsicmp(r->name.c_str(), L"ProgramData") == 0) {
            addFolders(r, L"ProgramData");
        } else if (r->dir) {  // "AppData": the apps live one level down
            for (const auto& c : r->dir->children) {
                if (c.removed || c.type != NodeType::Directory) continue;
                if (c.name == L"Local" || c.name == L"Roaming" || c.name == L"LocalLow") addFolders(&c, c.name.c_str());
            }
        }
    }
    std::sort(info.top.begin(), info.top.end(), [](const AppDataItem& a, const AppDataItem& b) { return a.size > b.size; });
    if (info.top.size() > topN) info.top.resize(topN);
    return info;
}

ScanStats ComputeStats(ScanNode* root, bool includeHidden, bool includeSystem, size_t topN, bool includeAppData) {
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
                if (!includeAppData && IsAppDataRoot(&c)) continue;
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
