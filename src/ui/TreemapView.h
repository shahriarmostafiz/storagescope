#pragma once
#include <string>
#include <vector>

#include "scanner/ScanNode.h"
#include "ui/View.h"

namespace ss {

// Squarified treemap of one folder: every box is a child, its area is its size. Click selects, double-click enters a folder.
class TreemapView : public View {
public:
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
    };
    void Rebuild();
    void Paint();
    void UpdateButtons();
    void UpdateInfo();
    int HitTest(POINT p) const;
    void Enter(ScanNode* n);
    void ContextMenu(POINT screen);

    HWND up_ = nullptr, bOpen_ = nullptr, bBasket_ = nullptr, path_ = nullptr, info_ = nullptr, empty_ = nullptr;
    ScanNode* cur_ = nullptr;
    std::wstring keepPath_;
    std::vector<Tile> tiles_;
    RECT area_{};
    int hover_ = -1, sel_ = -1;
    bool dirty_ = true;
};

}  // namespace ss
