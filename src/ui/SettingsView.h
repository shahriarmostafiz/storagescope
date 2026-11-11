#pragma once
#include "ui/View.h"

namespace ss {

class SettingsView : public View {
public:
    void OnCreate() override;
    void Layout(int w, int h) override;
    LRESULT OnMessage(UINT m, WPARAM w, LPARAM l) override;
    void OnShow() override { Load(); }

private:
    void Load();
    void Store();
    void FillIgnoreList();
    void AddIgnored();
    void RemoveIgnored();
    void UpdateRemoveButton();

    HWND hdrPriv_ = nullptr, history_ = nullptr, bClearHist_ = nullptr;
    HWND hdrMem_ = nullptr, remember_ = nullptr, autoUpd_ = nullptr, memInfo_ = nullptr, bClearScan_ = nullptr;
    HWND hdrIgnore_ = nullptr, noteIgnore_ = nullptr, ignoreList_ = nullptr, bAdd_ = nullptr, bRemove_ = nullptr;
    HWND hdrTheme_ = nullptr, cbTheme_ = nullptr, hdrScan_ = nullptr, hdrTemp_ = nullptr, hdrDel_ = nullptr, hdrPerf_ = nullptr, skipReparse_ = nullptr, skipDenied_ = nullptr,
         showHidden_ = nullptr, cbAge_ = nullptr, askRecycle_ = nullptr,
         second_ = nullptr, lblWorkers_ = nullptr, cbWorkers_ = nullptr, safety_ = nullptr, bLogs_ = nullptr;
    bool loading_ = false;
};

}  // namespace ss
