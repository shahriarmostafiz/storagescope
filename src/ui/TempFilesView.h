#pragma once
#include <mutex>
#include <thread>
#include <vector>

#include "cleanup/TempCleanup.h"
#include "ui/View.h"

namespace ss {

class MainWindow;

class TempFilesView : public View {
public:
    ~TempFilesView() override;
    void OnCreate() override;
    void Layout(int w, int h) override;
    LRESULT OnMessage(UINT m, WPARAM w, LPARAM l) override;
    void OnShow() override { if (cats_.empty()) Discover(); }
    void OnBusyChanged() override { UpdateButtons(); }

private:
    struct Row {
        TempCategory cat;
        TempScanResult res;
        bool scanned = false;
    };
    void Discover();
    void FillList();
    void UpdateButtons();
    void StartScan(bool afterClean = false);
    void StartClean();
    uint32_t AgeHours() const;
    bool Checked(int i) const;

    HWND list_ = nullptr, cbAge_ = nullptr, lblAge_ = nullptr, bScan_ = nullptr, bClean_ = nullptr, total_ = nullptr, result_ = nullptr,
         note_ = nullptr;
    std::vector<Row> cats_;
    std::mutex m_;
    TempCleanResult lastClean_;
    std::wstring confirmText_;   // shown in a message box once the post-clean refresh has finished
    bool pendingConfirm_ = false;
    std::wstring cleanedNote_;  // "Cleaned: ..." summary kept on screen while the list refreshes after a clean
    bool working_ = false;
    bool filling_ = false;
    // last members: joined first
    std::jthread th_;
};

}  // namespace ss
