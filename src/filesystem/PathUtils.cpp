#include "filesystem/PathUtils.h"

#include <windows.h>

#include <vector>

namespace ss::path {

namespace {
bool IsAsciiAlpha(wchar_t c) { return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z'); }
}  // namespace

std::wstring StripExtendedPrefix(const std::wstring& s) {
    if (s.rfind(L"\\\\?\\UNC\\", 0) == 0) return L"\\\\" + s.substr(8);
    if (s.rfind(L"\\\\?\\", 0) == 0) return s.substr(4);
    return s;
}

bool EqualsNoCase(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() &&
           CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) ==
               CSTR_EQUAL;
}

bool StartsWithNoCase(const std::wstring& s, const std::wstring& prefix) {
    return s.size() >= prefix.size() &&
           CompareStringOrdinal(s.c_str(), static_cast<int>(prefix.size()), prefix.c_str(),
                                static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL;
}

bool IsSameOrUnder(const std::wstring& p, const std::wstring& root) {
    if (root.empty() || !StartsWithNoCase(p, root)) return false;
    if (p.size() == root.size() || root.back() == L'\\') return true;
    return p[root.size()] == L'\\';
}

bool IsDriveRoot(const std::wstring& c) { return c.size() == 3 && IsAsciiAlpha(c[0]) && c[1] == L':' && c[2] == L'\\'; }

std::wstring Leaf(const std::wstring& c) {
    size_t k = c.rfind(L'\\');
    return k == std::wstring::npos ? c : c.substr(k + 1);
}

std::wstring Parent(const std::wstring& c) {
    if (IsDriveRoot(c)) return c;
    size_t k = c.rfind(L'\\');
    if (k == std::wstring::npos) return L"";
    if (k == 2 && c[1] == L':') return c.substr(0, 3);
    return c.substr(0, k);
}

std::wstring Join(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    return a.back() == L'\\' ? a + b : a + L"\\" + b;
}

std::wstring ToExtended(const std::wstring& c) {
    if (c.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + c.substr(2);
    return L"\\\\?\\" + c;
}

std::wstring Canonicalize(const std::wstring& in) {
    if (in.empty()) return L"";
    std::wstring s = in;

    if (s.find(L'%') != std::wstring::npos) {
        DWORD n = ExpandEnvironmentStringsW(s.c_str(), nullptr, 0);
        if (n) {
            std::wstring t(n, L'\0');
            n = ExpandEnvironmentStringsW(s.c_str(), t.data(), n);
            if (n) {
                t.resize(n - 1);
                s = t;
            }
        }
    }
    for (auto& ch : s)
        if (ch == L'/') ch = L'\\';

    if (s.rfind(L"\\\\.\\", 0) == 0) return L"";
    s = StripExtendedPrefix(s);

    bool drive = s.size() >= 2 && IsAsciiAlpha(s[0]) && s[1] == L':';
    bool unc = !drive && s.size() >= 3 && s[0] == L'\\' && s[1] == L'\\' && s[2] != L'\\' && s[2] != L'?' && s[2] != L'.';
    if (drive) {
        if (s.size() == 2) s += L'\\';
        else if (s[2] != L'\\') return L"";  // drive-relative ("C:foo") is ambiguous
    } else if (!unc) {
        return L"";  // relative or rooted-without-drive paths are never accepted
    }
    for (size_t i = 0; i < s.size(); ++i) {
        wchar_t ch = s[i];
        if (ch < 32 || ch == L'<' || ch == L'>' || ch == L'"' || ch == L'|' || ch == L'*' || ch == L'?') return L"";
        if (ch == L':' && !(drive && i == 1)) return L"";  // alternate data streams / device tricks
    }

    std::wstring root;
    size_t pos;
    if (drive) {
        root = s.substr(0, 2) + L"\\";
        root[0] = static_cast<wchar_t>(towupper(root[0]));
        pos = 3;
    } else {
        size_t a = s.find(L'\\', 2);
        if (a == std::wstring::npos) return L"";
        size_t b = s.find(L'\\', a + 1);
        root = s.substr(0, b == std::wstring::npos ? s.size() : b);
        if (a + 1 >= root.size()) return L"";  // "\\server\" without share
        pos = b == std::wstring::npos ? s.size() : b + 1;
    }

    std::vector<std::wstring> parts;
    while (pos <= s.size()) {
        size_t e = s.find(L'\\', pos);
        std::wstring tok = s.substr(pos, e == std::wstring::npos ? std::wstring::npos : e - pos);
        pos = e == std::wstring::npos ? s.size() + 1 : e + 1;
        if (tok == L".") continue;
        if (tok == L"..") {
            if (!parts.empty()) parts.pop_back();
            continue;
        }
        while (!tok.empty() && (tok.back() == L' ' || tok.back() == L'.')) tok.pop_back();  // Win32 strips these
        if (!tok.empty()) parts.push_back(std::move(tok));
    }

    std::wstring out = root;
    for (auto& p : parts) {
        if (out.back() != L'\\') out += L'\\';
        out += p;
    }

    // Expand 8.3 short names on the longest existing prefix (PROGRA~1 -> Program Files).
    if (out.find(L'~') != std::wstring::npos) {
        std::wstring cur = out, tail;
        for (;;) {
            std::wstring e = ToExtended(cur);
            DWORD n = GetLongPathNameW(e.c_str(), nullptr, 0);
            if (n) {
                std::wstring l(n, L'\0');
                n = GetLongPathNameW(e.c_str(), l.data(), n);
                l.resize(n);
                out = StripExtendedPrefix(l) + tail;
                break;
            }
            size_t k = cur.rfind(L'\\');
            if (k == std::wstring::npos || k <= 2) break;
            tail = cur.substr(k) + tail;
            cur = cur.substr(0, k);
        }
    }
    return out;
}

std::wstring ResolveFinal(const std::wstring& canonical) {
    if (canonical.empty()) return L"";
    HANDLE h = CreateFileW(ToExtended(canonical).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";
    DWORD n = GetFinalPathNameByHandleW(h, nullptr, 0, VOLUME_NAME_DOS | FILE_NAME_NORMALIZED);
    std::wstring r;
    if (n) {
        r.assign(n, L'\0');
        n = GetFinalPathNameByHandleW(h, r.data(), n, VOLUME_NAME_DOS | FILE_NAME_NORMALIZED);
        r.resize(n);
    }
    CloseHandle(h);
    return StripExtendedPrefix(r);
}

}  // namespace ss::path
