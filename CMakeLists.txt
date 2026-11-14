cmake_minimum_required(VERSION 3.20)
project(StorageScope LANGUAGES CXX)

if(NOT MSVC)
  message(FATAL_ERROR "StorageScope targets MSVC (Visual Studio 2019 16.11+ / 2022) on Windows 10/11 x64.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Static CRT: no VC++ redistributable needed, single EXE.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(SS_COMPILE_DEFS UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
set(SS_SYS_LIBS user32 gdi32 comctl32 shell32 ole32 uuid advapi32 shlwapi uxtheme dwmapi msimg32 oleacc oleaut32 uiautomationcore cabinet gdiplus)

# Everything that has no UI: scanner, safety rules, file operations, temp cleanup, utils.
add_library(storagescope_core STATIC
  src/filesystem/PathUtils.cpp
  src/filesystem/DriveManager.cpp
  src/filesystem/ChangeWatcher.cpp
  src/safety/PathProtectionService.cpp
  src/scanner/ScanNode.cpp
  src/scanner/DirectoryScanner.cpp
  src/scanner/FileTypes.cpp
  src/scanner/ScanStats.cpp
  src/scanner/ScanSnapshot.cpp
  src/scanner/DirectoryEnum.cpp
  src/scanner/TreeSync.cpp
  src/app/History.cpp
  src/operations/FileOperationService.cpp
  src/operations/ExplorerService.cpp
  src/operations/RecycleBin.cpp
  src/cleanup/TempCleanup.cpp
  src/utils/FormatUtils.cpp
  src/utils/Logger.cpp
  src/app/Settings.cpp)
target_include_directories(storagescope_core PUBLIC src)
target_compile_definitions(storagescope_core PUBLIC ${SS_COMPILE_DEFS})
target_compile_options(storagescope_core PUBLIC /W4 /utf-8 /EHsc /permissive-)
target_link_libraries(storagescope_core PUBLIC ${SS_SYS_LIBS})

add_executable(StorageScope WIN32
  resources/app.rc
  src/main.cpp
  src/ui/MainWindow.cpp
  src/ui/MainWindowSession.cpp
  src/ui/View.cpp
  src/ui/DashboardView.cpp
  src/ui/AnalyzerView.cpp
  src/ui/LargeFilesView.cpp
  src/ui/TempFilesView.cpp
  src/ui/SettingsView.cpp
  src/ui/TreemapView.cpp
  src/ui/RecycleBinView.cpp
  src/ui/Gfx.cpp)
target_link_libraries(StorageScope PRIVATE storagescope_core)
# Manifest: asInvoker, PerMonitorV2 DPI, long paths, Common Controls v6.
target_link_options(StorageScope PRIVATE
  "/MANIFEST:EMBED" "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/resources/app.manifest"
  /STACK:8388608 /OPT:REF /OPT:ICF)

enable_testing()
add_executable(StorageScopeTests tests/tests.cpp)
target_link_libraries(StorageScopeTests PRIVATE storagescope_core)
add_test(NAME safety_and_scanner COMMAND StorageScopeTests)
