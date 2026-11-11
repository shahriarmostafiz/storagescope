#include "scanner/FileTypes.h"

#include <cwctype>
#include <unordered_map>

namespace ss {

const wchar_t* CategoryName(FileCategory c) {
    static const wchar_t* names[] = {L"Videos", L"Pictures", L"Audio", L"Archives", L"Disk Images", L"Installers", L"Documents", L"Other"};
    return names[static_cast<int>(c)];
}

FileCategory Categorize(const std::wstring& name) {
    static const std::unordered_map<std::wstring, FileCategory> map = [] {
        std::unordered_map<std::wstring, FileCategory> m;
        auto add = [&](FileCategory c, std::initializer_list<const wchar_t*> exts) {
            for (auto* e : exts) m.emplace(e, c);
        };
        add(FileCategory::Video, {L".mp4", L".mkv", L".avi", L".mov", L".wmv", L".webm", L".m4v", L".flv", L".mpg", L".mpeg"});
        add(FileCategory::Picture, {L".jpg", L".jpeg", L".png", L".gif", L".bmp", L".heic", L".webp", L".tif", L".tiff", L".cr2", L".nef"});
        add(FileCategory::Audio, {L".mp3", L".wav", L".flac", L".aac", L".m4a", L".ogg", L".wma"});
        add(FileCategory::Archive, {L".zip", L".rar", L".7z", L".tar", L".gz", L".bz2", L".xz", L".cab", L".tgz"});
        add(FileCategory::DiskImage, {L".iso", L".vhd", L".vhdx", L".img", L".wim", L".vmdk", L".vdi", L".qcow2"});
        add(FileCategory::Installer, {L".exe", L".msi", L".msix", L".msp", L".appx"});
        add(FileCategory::Document, {L".pdf", L".doc", L".docx", L".xls", L".xlsx", L".ppt", L".pptx", L".txt", L".odt", L".csv", L".rtf"});
        return m;
    }();
    size_t d = name.rfind(L'.');
    if (d == std::wstring::npos || d == 0 || name.size() - d > 8) return FileCategory::Other;
    wchar_t buf[9];
    size_t n = name.size() - d;
    for (size_t i = 0; i < n; ++i) {
        wchar_t c = name[d + i];
        buf[i] = c < 128 ? (c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c + 32) : c) : static_cast<wchar_t>(towlower(c));
    }
    auto it = map.find(std::wstring(buf, n));
    return it == map.end() ? FileCategory::Other : it->second;
}

}  // namespace ss
