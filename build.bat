@echo off
rem StorageScope build script (MSVC, no CMake required).
rem   build.bat            -> core lib + tests + app
rem   build.bat tests      -> core lib + tests only
rem Output: build\StorageScope.exe, build\StorageScopeTests.exe
setlocal EnableDelayedExpansion
set "ROOT=%~dp0"
set "OUT=%ROOT%build"
if not exist "%OUT%\core" mkdir "%OUT%\core"
if not exist "%OUT%\app" mkdir "%OUT%\app"
if not exist "%OUT%\tests" mkdir "%OUT%\tests"

set "VSW=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSROOT="
for /f "usebackq delims=" %%i in (`"%VSW%" -latest -products * -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT set "VSROOT=%ProgramFiles(x86)%\Microsoft Visual Studio\2019\BuildTools"
if not exist "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" (
  echo Could not find vcvars64.bat - install "Desktop development with C++" ^(MSVC + Windows SDK^).
  exit /b 1
)
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

pushd "%ROOT%"
set "FLAGS=/nologo /std:c++20 /EHsc /O2 /MT /W4 /utf-8 /MP /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_WIN32_WINNT=0x0A00 /DNDEBUG /Isrc"
set "SYSLIBS=user32.lib gdi32.lib comctl32.lib shell32.lib ole32.lib uuid.lib advapi32.lib shlwapi.lib uxtheme.lib dwmapi.lib msimg32.lib oleacc.lib oleaut32.lib uiautomationcore.lib cabinet.lib"

set "CORE=src\filesystem\PathUtils.cpp src\filesystem\DriveManager.cpp src\filesystem\ChangeWatcher.cpp src\safety\PathProtectionService.cpp src\scanner\ScanNode.cpp src\scanner\DirectoryScanner.cpp src\scanner\FileTypes.cpp src\scanner\ScanStats.cpp src\scanner\ScanSnapshot.cpp src\scanner\DirectoryEnum.cpp src\scanner\TreeSync.cpp src\app\History.cpp src\operations\FileOperationService.cpp src\operations\ExplorerService.cpp src\cleanup\TempCleanup.cpp src\utils\FormatUtils.cpp src\utils\Logger.cpp src\app\Settings.cpp"

cl %FLAGS% /c /Fo"%OUT%\core\\" %CORE% || goto :fail
if exist "%OUT%\core.lib" del "%OUT%\core.lib"
lib /nologo /OUT:"%OUT%\core.lib" "%OUT%\core\*.obj" || goto :fail

cl %FLAGS% /Fo"%OUT%\tests\\" /Fe"%OUT%\StorageScopeTests.exe" tests\tests.cpp /link /nologo "%OUT%\core.lib" %SYSLIBS% || goto :fail

if /i "%1"=="tests" goto :ok

rc /nologo /fo "%OUT%\app.res" resources\app.rc || goto :fail
cl %FLAGS% /Fo"%OUT%\app\\" /Fe"%OUT%\StorageScope.exe" src\main.cpp src\ui\*.cpp /link /nologo "%OUT%\core.lib" "%OUT%\app.res" %SYSLIBS% /SUBSYSTEM:WINDOWS /STACK:8388608 /OPT:REF /OPT:ICF /MANIFEST:EMBED /MANIFESTINPUT:"%ROOT%resources\app.manifest" || goto :fail

:ok
popd
echo BUILD OK
exit /b 0
:fail
popd
echo BUILD FAILED
exit /b 1
