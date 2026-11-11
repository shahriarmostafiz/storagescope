#pragma once
#include <string>

namespace ss {

// One shared table for the "Usage by file type" bar, the Large Files filter and the treemap colours.
// The extension decides the category; it never decides whether a file is safe to delete.
enum class FileCategory { Video, Picture, Audio, Archive, DiskImage, Installer, Document, Other, Count };

constexpr int kCategoryCount = static_cast<int>(FileCategory::Count);
const wchar_t* CategoryName(FileCategory c);
FileCategory Categorize(const std::wstring& fileName);

}  // namespace ss
