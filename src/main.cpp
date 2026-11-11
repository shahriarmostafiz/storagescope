#include <windows.h>

#include <commctrl.h>
#include <objbase.h>

#include "app/Settings.h"
#include "ui/MainWindow.h"
#include "utils/Logger.h"

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int nShow) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);  // no "insert disk" dialogs for empty drives
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return 1;
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);
    ss::Logger::Get().Init(ss::Settings::AppDataDir() + L"\\logs");
    int rc = ss::MainWindow::Run(hi, nShow);
    CoUninitialize();
    return rc;
}
