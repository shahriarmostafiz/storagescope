#pragma once
#include <windows.h>

#include <string>

namespace ss::explorer {

bool OpenFolder(const std::wstring& dir);                  // Explorer at a folder
bool Reveal(const std::wstring& path, bool isDir);         // folder: open it; file: open parent with the file selected
// Directories open in Explorer; files open with the default handler, except executable/script
// types, which are only revealed (never launched from a disk-cleaning tool).
bool OpenItem(const std::wstring& path, bool isDir);
bool ShowProperties(HWND owner, const std::wstring& path);
bool CopyText(HWND owner, const std::wstring& text);       // CF_UNICODETEXT
bool OpenInstalledApps();
bool OpenPathNoSelect(const std::wstring& path);
bool IsRiskyToLaunch(const std::wstring& path);

}  // namespace ss::explorer
