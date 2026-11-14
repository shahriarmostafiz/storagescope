#pragma once
#include <string>
#include <vector>

#include "scanner/ScanNode.h"
#include "ui/View.h"

namespace ss {

// Icon cards for one folder: every card is a child with its size, share and a coloured share bar. Click selects, double-click enters a folder.
class TreemapView : public View {
public:
    ~TreemapView() override;
    void OnCreate() override;
    void Layout(int w, int h) override;
    LRESULT OnMessage(UINT m, WPARAM w, LPARAM l) override;
    void OnShow() override { Rebuild(); }
    void Reload() override { if (IsWindowVisible(hwnd_)) Rebuild(); else dirty_ = true; }
    void OnBusyChanged() override { UpdateButtons(); }
    void DropReferences() override;

private:
    struct Tile {
        RECT r{};
        ScanNode* n = nullptr;  // null = the "Other (n items)" remainder
        uint64_t size = 0;
        size_t count = 0;       // items in the remainder tile
        bool locked = false;    // app data (AppData / ProgramData): shown for its size only, no select / open / delete
    };
    void Rebuild();
    void Paint();
    void UpdateButtons();
    void UpdateInfo();
    void UpdateScroll();
    void ScrollTo(int y);
    int HitTest(POINT p) const;
    void Enter(ScanNode* n);
    void ContextMenu(POINT screen);

    HWND up_ = nullptr, bOpen_ = nullptr, bBasket_ = nullptr, path_ = nullptr, info_ = nullptr, infoIcon_ = nullptr, empty_ = nullptr;
    ScanNode* cur_ = nullptr;
    std::wstring keepPath_;
    std::vector<Tile> tiles_;
    RECT area_{};
    int hover_ = -1, sel_ = -1;
    int scroll_ = 0, bottom_ = 0;  // vertical scroll of the card grid; bottom of the last card (content coordinates)
    bool inScroll_ = false;
    uint64_t hiddenApp_ = 0;  // bytes of AppData / ProgramData below the shown folder while they are hidden
    HFONT iconFont_ = nullptr;
    int iconPx_ = 0;
    bool dirty_ = true;
};

}  // namespace ss
