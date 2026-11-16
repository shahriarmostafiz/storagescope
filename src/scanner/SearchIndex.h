#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "scanner/ScanNode.h"

namespace ss {

enum class SearchKind { Both, FilesOnly, FoldersOnly };

struct SearchSpec {
    // Free text: words must all match (AND); "quoted phrase"; * and ? make a word a whole-name wildcard; a word with \ or : matches the full path.
    std::wstring text;
    bool matchCase = false, wholeWord = false, regex = false;  // regex: `text` is one ECMAScript pattern applied to the name
    SearchKind kind = SearchKind::Both;
    int category = -1;                  // FileCategory index (files only), -1 = any
    std::wstring include, exclude;      // "pdf, docx, *.log": a bare word is an extension, * and ? are name wildcards
    uint64_t minSize = 0, maxSize = ~0ull;
    uint64_t minModified = 0, maxModified = ~0ull;  // FILETIME
    std::vector<const ScanNode*> scopes;    // search only inside these folders (empty = everything)
    std::vector<const ScanNode*> excluded;  // never search inside these folders
    bool showHidden = true;
    std::function<bool(const ScanNode&)> hideDir;  // folders (and everything in them) to skip, e.g. app data
    std::function<bool()> cancel;                  // polled now and then; true stops the search
    size_t maxResults = 50000;
};

struct SearchResult {
    std::vector<ScanNode*> hits;  // in tree order
    bool truncated = false;       // stopped at maxResults
    bool canceled = false;        // stopped by `cancel`
    std::wstring error;           // e.g. an invalid regex; hits is empty then
};

// Flat list of every node of a scan in tree order, with lowercase names in one buffer. A folder's whole subtree is the slice
// [i, End(i)), so "search in this folder" and "skip this folder" are integer ranges, and one pass over the list searches everything.
// Holds raw node pointers: rebuild (or Clear) whenever the tree changes structurally.
class SearchIndex {
public:
    void Build(ScanNode* root);
    void Clear();
    bool Empty() const { return nodes_.empty(); }
    size_t Size() const { return nodes_.size(); }
    SearchResult Run(const SearchSpec& spec) const;

private:
    size_t IndexOf(const ScanNode* n) const;
    std::vector<ScanNode*> nodes_;
    std::vector<uint32_t> end_;  // one past the last descendant (i + 1 for files)
    std::vector<uint32_t> off_;  // start of each lowercase name in pool_; one extra entry at the end
    std::wstring pool_;
};

}  // namespace ss
