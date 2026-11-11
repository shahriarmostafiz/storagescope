#include "operations/RecycleBin.h"

#include <shellapi.h>
#include <shlobj.h>

#include <unordered_set>

namespace ss {
namespace {

// System.Size and the shell's "deleted from" / "date deleted" columns of the Recycle Bin.
const PROPERTYKEY kSize{{0xB725F130, 0x47EF, 0x101A, {0xA5, 0xF1, 0x02, 0x60, 0x8C, 0x9E, 0xEB, 0xAC}}, 12};
const PROPERTYKEY kDeletedFrom{{0x9B174B33, 0x40FF, 0x11D2, {0xA2, 0x7E, 0x00, 0xC0, 0x4F, 0xC3, 0x08, 0x71}}, 2};
const PROPERTYKEY kDateDeleted{{0x9B174B33, 0x40FF, 0x11D2, {0xA2, 0x7E, 0x00, 0xC0, 0x4F, 0xC3, 0x08, 0x71}}, 3};

std::wstring DisplayName(IShellItem* it) {
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(it->GetDisplayName(SIGDN_NORMALDISPLAY, &p)) && p) {
        s = p;
        CoTaskMemFree(p);
    }
    return s;
}

}  // namespace

void RecycleBin::Clear() {
    for (auto& i : items_)
        if (i.item) i.item->Release();
    items_.clear();
}

void RecycleBin::Reload() {
    Clear();
    IShellItem* bin = nullptr;
    if (FAILED(SHGetKnownFolderItem(FOLDERID_RecycleBinFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&bin))) || !bin) return;
    IEnumShellItems* en = nullptr;
    if (SUCCEEDED(bin->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&en))) && en) {
        IShellItem* it = nullptr;
        ULONG got = 0;
        while (en->Next(1, &it, &got) == S_OK && got == 1) {
            BinItem b;
            b.item = it;  // keeps the reference from Next()
            // The shell's display name here is the full original path without the extension (when Explorer hides extensions).
            // Show the file name like Explorer's Name column: the leaf, plus the extension taken from the "$R....ext" item name.
            b.name = DisplayName(it);
            if (size_t cut = b.name.find_last_of(L"\\/"); cut != std::wstring::npos) b.name.erase(0, cut + 1);
            PWSTR parsing = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &parsing)) && parsing) {
                std::wstring p = parsing;
                CoTaskMemFree(parsing);
                size_t dot = p.find_last_of(L'.');
                if (dot != std::wstring::npos && dot + 1 < p.size()) {
                    std::wstring ext = p.substr(dot);
                    bool has = b.name.size() >= ext.size() && _wcsicmp(b.name.c_str() + b.name.size() - ext.size(), ext.c_str()) == 0;
                    if (!has) b.name += ext;
                }
            }
            SFGAOF attrs = 0;
            if (SUCCEEDED(it->GetAttributes(SFGAO_FOLDER, &attrs))) b.isDir = (attrs & SFGAO_FOLDER) != 0;
            IShellItem2* it2 = nullptr;
            if (SUCCEEDED(it->QueryInterface(IID_PPV_ARGS(&it2))) && it2) {
                PWSTR from = nullptr;
                if (SUCCEEDED(it2->GetString(kDeletedFrom, &from)) && from) {
                    b.from = from;
                    CoTaskMemFree(from);
                }
                ULONGLONG sz = 0;
                if (SUCCEEDED(it2->GetUInt64(kSize, &sz))) b.size = sz;
                FILETIME ft{};
                if (SUCCEEDED(it2->GetFileTime(kDateDeleted, &ft))) b.deleted = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
                it2->Release();
            }
            items_.push_back(std::move(b));
        }
        en->Release();
    }
    bin->Release();
}

size_t RecycleBin::Purge(const std::vector<size_t>& idx) {
    if (idx.empty()) return 0;
    IFileOperation* op = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op))) || !op) return 0;
    // No FOFX_RECYCLEONDELETE / FOF_ALLOWUNDO: deleting an item that already is in the Recycle Bin removes it for good.
    op->SetOperationFlags(FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT);
    for (size_t i : idx)
        if (i < items_.size() && items_[i].item) op->DeleteItem(items_[i].item, nullptr);
    op->PerformOperations();
    op->Release();
    size_t before = items_.size();
    Reload();
    return before > items_.size() ? before - items_.size() : 0;
}

size_t RecycleBin::Restore(const std::vector<size_t>& idx, HWND owner) {
    size_t before = items_.size();
    for (size_t i : idx) {
        if (i >= items_.size() || !items_[i].item) continue;
        IContextMenu* cm = nullptr;
        if (FAILED(items_[i].item->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&cm))) || !cm) continue;
        HMENU menu = CreatePopupMenu();
        if (SUCCEEDED(cm->QueryContextMenu(menu, 0, 1, 0x7FFF, CMF_NORMAL))) {
            CMINVOKECOMMANDINFO ci{sizeof ci};
            ci.hwnd = owner;
            ci.lpVerb = "undelete";
            ci.nShow = SW_SHOWNORMAL;
            cm->InvokeCommand(&ci);
        }
        DestroyMenu(menu);
        cm->Release();
    }
    Reload();
    return before > items_.size() ? before - items_.size() : 0;
}

}  // namespace ss
