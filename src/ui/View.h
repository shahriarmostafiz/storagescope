#pragma once
#include <windows.h>

namespace ss {

class MainWindow;

// A page hosted inside the main window. Child controls send WM_COMMAND/WM_NOTIFY to hwnd().
class View {
public:
    virtual ~View() = default;
    bool Create(HWND parent, MainWindow* mw);
    HWND hwnd() const { return hwnd_; }

    virtual void OnCreate() = 0;                 // create child controls
    virtual void Layout(int w, int h) = 0;
    virtual LRESULT OnMessage(UINT m, WPARAM w, LPARAM l);
    virtual void OnShow() {}                     // page became visible
    virtual void Reload() {}                     // scan data changed (scan done, items deleted, settings)
    virtual void OnBusyChanged() {}              // scan / delete started or finished
    virtual void DropReferences() {}             // the scan tree is about to be replaced/freed: forget every ScanNode*

protected:
    HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0);
    MainWindow* mw_ = nullptr;
    HWND hwnd_ = nullptr;

    // Vertical scrolling for pages taller than the window. Call EnableScroll() in OnCreate; in Layout():
    //   if (!BeginLayout(w, h)) return;  y = top - scrollY_;  ...place controls...;  EndLayout(bottomOfContent);
    void EnableScroll();
    bool BeginLayout(int w, int h);
    void EndLayout(int contentBottom);
    int scrollY_ = 0;

private:
    void ScrollTo(int y);
    int contentH_ = 0, viewW_ = 0, viewH_ = 0, retry_ = 0;
    bool inLayout_ = false, scrollable_ = false;
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
};

}  // namespace ss
