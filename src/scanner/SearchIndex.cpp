#include "scanner/SearchIndex.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <regex>
#include <string_view>
#include <unordered_set>

#include "scanner/FileTypes.h"

namespace ss {
namespace {

wchar_t Lower(wchar_t c) { return c < 128 ? (c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c + 32) : c) : static_cast<wchar_t>(towlower(c)); }

std::wstring LowerStr(std::wstring s) {
    for (auto& c : s) c = Lower(c);
    return s;
}

bool IsGlob(const std::wstring& s) { return s.find_first_of(L"*?") != std::wstring::npos; }

// Whole-string wildcard match: * = any run, ? = any one character.
bool Glob(std::wstring_view p, std::wstring_view s) {
    size_t pi = 0, si = 0, star = std::wstring_view::npos, mark = 0;
    while (si < s.size()) {
        if (pi < p.size() && (p[pi] == L'?' || p[pi] == s[si])) {
            ++pi;
            ++si;
        } else if (pi < p.size() && p[pi] == L'*') {
            star = pi++;
            mark = si;
        } else if (star != std::wstring_view::npos) {
            pi = star + 1;
            si = ++mark;
        } else
            return false;
    }
    while (pi < p.size() && p[pi] == L'*') ++pi;
    return pi == p.size();
}

bool IsWordChar(wchar_t c) { return iswalnum(c) || c == L'_'; }

bool ContainsWord(std::wstring_view s, std::wstring_view t) {
    for (size_t p = s.find(t); p != std::wstring_view::npos; p = s.find(t, p + 1)) {
        bool left = p == 0 || !IsWordChar(s[p - 1]);
        bool right = p + t.size() >= s.size() || !IsWordChar(s[p + t.size()]);
        if (left && right) return true;
    }
    return false;
}

// "pdf, docx; *.log" -> {pattern, isGlob}; a bare word is an extension (".pdf" and "pdf" are the same).
struct Pat {
    std::wstring s;
    bool glob = false;
};
std::vector<Pat> ParseList(const std::wstring& text) {
    std::vector<Pat> out;
    std::wstring cur;
    auto flush = [&] {
        if (cur.empty()) return;
        Pat p;
        p.glob = IsGlob(cur);
        if (!p.glob && cur[0] == L'.') cur.erase(0, 1);
        p.s = LowerStr(cur);
        if (!p.s.empty()) out.push_back(std::move(p));
        cur.clear();
    };
    for (wchar_t c : text) {
        if (c == L',' || c == L';' || c == L' ' || c == L'\t') flush();
        else cur += c;
    }
    flush();
    return out;
}

bool ListMatches(const std::vector<Pat>& l, std::wstring_view lowerName) {
    for (const Pat& p : l) {
        if (p.glob) {
            if (Glob(p.s, lowerName)) return true;
        } else if (lowerName.size() > p.s.size() && lowerName[lowerName.size() - p.s.size() - 1] == L'.' &&
                   lowerName.substr(lowerName.size() - p.s.size()) == p.s)
            return true;
    }
    return false;
}

struct Term {
    std::wstring s;
    bool glob = false, path = false;
};

std::vector<Term> ParseTerms(const std::wstring& text, bool matchCase) {
    std::vector<Term> out;
    std::wstring cur;
    bool quoted = false;
    auto flush = [&] {
        if (cur.empty()) return;
        Term t;
        t.s = matchCase ? cur : LowerStr(cur);
        t.path = t.s.find_first_of(L"\\:/") != std::wstring::npos;
        if (t.path) std::replace(t.s.begin(), t.s.end(), L'/', L'\\');
        t.glob = !quoted && !t.path && IsGlob(t.s);
        out.push_back(std::move(t));
        cur.clear();
        quoted = false;
    };
    bool inQuote = false;
    for (wchar_t c : text) {
        if (c == L'"') {
            inQuote = !inQuote;
            quoted = quoted || inQuote;
        } else if (!inQuote && (c == L' ' || c == L'\t')) {
            flush();
        } else {
            cur += c;
        }
    }
    flush();
    return out;
}

}  // namespace

void SearchIndex::Clear() {
    nodes_.clear();
    end_.clear();
    off_.clear();
    pool_.clear();
    nodes_.shrink_to_fit();
    end_.shrink_to_fit();
    off_.shrink_to_fit();
    pool_.shrink_to_fit();
}

void SearchIndex::Build(ScanNode* root) {
    Clear();
    if (!root) return;
    size_t guess = static_cast<size_t>(root->files) + root->dirs + 1;
    nodes_.reserve(guess);
    end_.reserve(guess);
    off_.reserve(guess + 1);
    pool_.reserve(guess * 14);
    auto add = [&](ScanNode* n) {
        size_t idx = nodes_.size();
        nodes_.push_back(n);
        end_.push_back(0);
        off_.push_back(static_cast<uint32_t>(pool_.size()));
        for (wchar_t c : n->name) pool_ += Lower(c);
        return idx;
    };
    struct Frame {
        ScanNode* n;
        size_t next, idx;
    };
    std::vector<Frame> st;
    st.push_back({root, 0, add(root)});
    while (!st.empty()) {
        Frame& f = st.back();
        if (!f.n->dir || f.next >= f.n->dir->children.size()) {
            end_[f.idx] = static_cast<uint32_t>(nodes_.size());
            st.pop_back();
            continue;
        }
        ScanNode* c = &f.n->dir->children[f.next++];
        if (c->removed) continue;
        size_t ci = add(c);
        if (c->dir && !c->dir->children.empty()) st.push_back({c, 0, ci});  // `f` is invalid after this push
        else end_[ci] = static_cast<uint32_t>(ci + 1);
    }
    off_.push_back(static_cast<uint32_t>(pool_.size()));
}

size_t SearchIndex::IndexOf(const ScanNode* n) const {
    auto it = std::find(nodes_.begin(), nodes_.end(), n);
    return it == nodes_.end() ? static_cast<size_t>(-1) : static_cast<size_t>(it - nodes_.begin());
}

SearchResult SearchIndex::Run(const SearchSpec& sp) const {
    SearchResult res;
    if (nodes_.empty()) return res;

    std::vector<Term> terms;
    std::wregex re;
    bool useRe = sp.regex && !sp.text.empty();
    if (useRe) {
        try {
            auto flags = std::regex_constants::ECMAScript | std::regex_constants::optimize;
            if (!sp.matchCase) flags |= std::regex_constants::icase;
            re = std::wregex(sp.text, flags);
        } catch (const std::regex_error&) {
            res.error = L"This is not a valid regular expression.";
            return res;
        }
    } else
        terms = ParseTerms(sp.text, sp.matchCase);
    std::vector<Pat> inc = ParseList(sp.include), exc = ParseList(sp.exclude);
    bool typeFilter = sp.category >= 0 || !inc.empty();
    std::unordered_set<const ScanNode*> excl(sp.excluded.begin(), sp.excluded.end());

    // Ranges to scan: the whole list, or the slice of each chosen folder (nested choices are not searched twice).
    std::vector<std::pair<size_t, size_t>> ranges;
    if (sp.scopes.empty()) ranges.emplace_back(0, nodes_.size());
    else {
        for (const ScanNode* s : sp.scopes) {
            size_t i = IndexOf(s);
            if (i != static_cast<size_t>(-1)) ranges.emplace_back(i + 1, end_[i]);  // the folder itself is not a result
        }
        std::sort(ranges.begin(), ranges.end());
        std::vector<std::pair<size_t, size_t>> merged;
        for (auto& r : ranges)
            if (merged.empty() || r.first >= merged.back().second) merged.push_back(r);
        ranges = std::move(merged);
    }

    std::wstring pathBuf;
    size_t tick = 0;
    for (auto [lo, hi] : ranges) {
        for (size_t i = lo; i < hi;) {
            if ((++tick & (useRe ? 0x3F : 0xFFF)) == 0 && sp.cancel && sp.cancel()) {
                res.canceled = true;
                return res;
            }
            ScanNode* n = nodes_[i];
            if (n->removed || (n->attrs & FILE_ATTRIBUTE_SYSTEM) || (!sp.showHidden && (n->attrs & FILE_ATTRIBUTE_HIDDEN))) {
                i = end_[i];
                continue;
            }
            if (n->type == NodeType::Virtual) {
                ++i;
                continue;
            }
            bool dir = n->type == NodeType::Directory;
            if (dir && (excl.count(n) || (sp.hideDir && sp.hideDir(*n)))) {
                i = end_[i];
                continue;
            }
            size_t at = i++;
            if (n->scanRoot || (n->type != NodeType::File && !dir)) continue;  // scan roots (full-path names) and reparse points are not results
            if ((sp.kind == SearchKind::FilesOnly && dir) || (sp.kind == SearchKind::FoldersOnly && !dir)) continue;
            if (dir && typeFilter) continue;
            if (n->size < sp.minSize || n->size > sp.maxSize || n->modified < sp.minModified || n->modified > sp.maxModified) continue;
            if (sp.category >= 0 && static_cast<int>(Categorize(n->name)) != sp.category) continue;

            std::wstring_view lower(pool_.data() + off_[at], off_[at + 1] - off_[at]);
            if (!inc.empty() && !ListMatches(inc, lower)) continue;
            if (!exc.empty() && ListMatches(exc, lower)) continue;

            std::wstring_view name = sp.matchCase ? std::wstring_view(n->name) : lower;
            bool ok = true;
            if (useRe) ok = std::regex_search(n->name.data(), n->name.data() + n->name.size(), re);
            else
                for (const Term& t : terms) {
                    if (t.path) {
                        pathBuf = BuildPath(n);
                        std::wstring p = sp.matchCase ? pathBuf : LowerStr(pathBuf);
                        ok = p.find(t.s) != std::wstring::npos;
                    } else if (t.glob) ok = Glob(t.s, name);
                    else ok = sp.wholeWord ? ContainsWord(name, t.s) : name.find(t.s) != std::wstring_view::npos;
                    if (!ok) break;
                }
            if (!ok) continue;
            res.hits.push_back(n);
            if (res.hits.size() >= sp.maxResults) {
                res.truncated = true;
                return res;
            }
        }
    }
    return res;
}

}  // namespace ss
