#include "scanner/ScanNode.h"

#include <windows.h>

#include <algorithm>

#include "filesystem/PathUtils.h"

namespace ss {

ScanNode::ScanNode() = default;
ScanNode::~ScanNode() = default;
ScanNode::ScanNode(ScanNode&&) noexcept = default;
ScanNode& ScanNode::operator=(ScanNode&&) noexcept = default;

std::wstring BuildPath(const ScanNode* n) {
    std::vector<const ScanNode*> chain;
    for (const ScanNode* p = n; p; p = p->parent) {
        chain.push_back(p);
        if (p->scanRoot) break;
    }
    std::wstring out;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        if ((*it)->type == NodeType::Virtual) continue;
        if (!out.empty() && out.back() != L'\\') out += L'\\';
        out += (*it)->name;
    }
    return out;
}

std::wstring DisplayName(const ScanNode* n) {
    if (n->type == NodeType::Virtual) return L"This PC";
    if (n->scanRoot && n->name.size() == 3 && n->name[1] == L':' && n->name[2] == L'\\') return n->name.substr(0, 2);
    return n->name;
}

const wchar_t* StatusText(const ScanNode* n) {
    switch (n->status) {
        case NodeStatus::AccessDenied: return L"Access denied";
        case NodeStatus::Unavailable: return L"Unavailable";
        case NodeStatus::Junction: return L"Junction - not scanned";
        case NodeStatus::Symlink: return L"Symbolic link - not scanned";
        case NodeStatus::ReparseOther: return L"Reparse point - not scanned";
        default: return L"";
    }
}

int DetachMatching(ScanNode* root, const std::function<bool(const std::wstring&)>& isMatch) {
    int n = 0;
    std::vector<std::pair<ScanNode*, std::wstring>> stack;
    stack.emplace_back(root, BuildPath(root));
    while (!stack.empty()) {
        auto [node, dirPath] = std::move(stack.back());
        stack.pop_back();
        if (!node->dir) continue;
        for (auto& c : node->dir->children) {
            if (c.removed || c.type != NodeType::Directory) continue;
            std::wstring cp = path::Join(dirPath, c.name);
            if (isMatch(cp)) {
                DetachFromTree(&c);
                ++n;
            } else {
                stack.emplace_back(&c, std::move(cp));
            }
        }
    }
    return n;
}

int DetachMissing(const std::vector<ScanNode*>& nodes) {
    int n = 0;
    for (ScanNode* nd : nodes) {
        if (nd->removed) continue;
        std::wstring p = BuildPath(nd);
        if (p.empty()) continue;
        if (GetFileAttributesW(path::ToExtended(p).c_str()) != INVALID_FILE_ATTRIBUTES) continue;
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) {
            DetachFromTree(nd);
            ++n;
        }
    }
    return n;
}

std::unique_ptr<DirData> ReplaceSubtree(ScanNode* t, ScanNode& fresh) {
    if (!fresh.dir) return nullptr;
    int64_t dSize = static_cast<int64_t>(fresh.size) - static_cast<int64_t>(t->size);
    int64_t dAlloc = static_cast<int64_t>(fresh.allocated) - static_cast<int64_t>(t->allocated);
    int64_t dFiles = static_cast<int64_t>(fresh.files) - static_cast<int64_t>(t->files);
    int64_t dDirs = static_cast<int64_t>(fresh.dirs) - static_cast<int64_t>(t->dirs);

    std::unique_ptr<DirData> old = std::move(t->dir);
    t->dir = std::move(fresh.dir);  // child addresses stay valid: only the vector's buffer pointer moves
    for (auto& c : t->dir->children) c.parent = t;
    t->size = fresh.size;
    t->allocated = fresh.allocated;
    t->files = fresh.files;
    t->dirs = fresh.dirs;
    t->status = fresh.status;
    AdjustAncestors(t->parent, dSize, dAlloc, dFiles, dDirs);
    return old;
}

void AdjustAncestors(ScanNode* start, int64_t dSize, int64_t dAlloc, int64_t dFiles, int64_t dDirs) {
    auto add = [](auto& field, int64_t d) {  // clamped at 0: a stale delta must never wrap around
        using T = std::remove_reference_t<decltype(field)>;
        if (d >= 0) field += static_cast<T>(d);
        else field = field > static_cast<T>(-d) ? field - static_cast<T>(-d) : 0;
    };
    for (ScanNode* p = start; p; p = p->parent) {
        add(p->size, dSize);
        add(p->allocated, dAlloc);
        add(p->files, dFiles);
        add(p->dirs, dDirs);
    }
}

ScanNode* FindNodeByPath(ScanNode* root, const std::wstring& path) {
    std::wstring c = path::Canonicalize(path);
    if (c.empty() || !root) return nullptr;
    ScanNode* cur = nullptr;
    std::wstring rest;
    auto tryRoot = [&](ScanNode* r) {
        if (!path::IsSameOrUnder(c, r->name)) return false;
        cur = r;
        rest = c.substr(r->name.size());
        while (!rest.empty() && rest.front() == L'\\') rest.erase(rest.begin());
        return true;
    };
    if (root->type == NodeType::Virtual) {  // "This PC": the scan roots are its children
        if (root->dir)
            for (auto& k : root->dir->children)
                if (!k.removed && k.scanRoot && tryRoot(&k)) break;
        if (!cur) return nullptr;
    } else if (!tryRoot(root)) {
        return nullptr;
    }
    size_t pos = 0;
    while (pos < rest.size()) {
        size_t e = rest.find(L'\\', pos);
        std::wstring comp = rest.substr(pos, e == std::wstring::npos ? std::wstring::npos : e - pos);
        pos = e == std::wstring::npos ? rest.size() : e + 1;
        if (comp.empty()) continue;
        ScanNode* next = nullptr;
        if (cur->dir)
            for (auto& k : cur->dir->children)
                if (!k.removed && path::EqualsNoCase(k.name, comp)) {
                    next = &k;
                    break;
                }
        if (!next) return nullptr;
        cur = next;
    }
    return cur;
}

void DetachFromTree(ScanNode* n) {
    if (n->removed) return;
    n->removed = true;
    uint64_t sz = n->size, al = n->allocated;
    uint32_t f = n->type == NodeType::File ? 1u : (n->type == NodeType::Directory ? n->files : 0u);
    uint32_t d = n->type == NodeType::Directory ? n->dirs + 1u : 0u;
    for (ScanNode* p = n->parent; p; p = p->parent) {
        p->size -= std::min(p->size, sz);
        p->allocated -= std::min(p->allocated, al);
        p->files -= std::min(p->files, f);
        p->dirs -= std::min(p->dirs, d);
    }
}

}  // namespace ss
