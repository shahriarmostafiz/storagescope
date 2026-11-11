#pragma once
#include <string>

namespace ss::path {

// Lexical canonical form: env vars expanded, "\\?\" stripped, '/' -> '\', "." and ".." resolved,
// duplicate/trailing separators and trailing dots/spaces removed, 8.3 names expanded where the
// path exists, drive letter upper-cased. Returns "" for anything that is not an absolute
// drive-letter or UNC path, or contains wildcards / ADS colons / device prefixes.
std::wstring Canonicalize(const std::wstring& in);

// "\\?\C:\x" form (long-path safe) for a canonical path.
std::wstring ToExtended(const std::wstring& canonical);

// Follows junctions/symlinks via an open handle. Returns "" if the path cannot be opened.
std::wstring ResolveFinal(const std::wstring& canonical);

// Component-aware prefix test (C:\Windows2 is NOT under C:\Windows). Case-insensitive.
bool IsSameOrUnder(const std::wstring& canonicalPath, const std::wstring& canonicalRoot);
bool EqualsNoCase(const std::wstring& a, const std::wstring& b);
bool StartsWithNoCase(const std::wstring& s, const std::wstring& prefix);
bool IsDriveRoot(const std::wstring& canonical);
std::wstring Parent(const std::wstring& canonical);
std::wstring Leaf(const std::wstring& canonical);
std::wstring Join(const std::wstring& a, const std::wstring& b);
std::wstring StripExtendedPrefix(const std::wstring& s);

}  // namespace ss::path
