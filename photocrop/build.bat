@echo off
rem build.bat        builds build\PhotoCrop.exe IN PLACE (always the same file) and refreshes build\PhotoCrop.zip.
rem build.bat test   builds and runs the tests (tests.cpp); nothing is left in the project folder.
rem Close any running PhotoCrop.exe first, or the link step cannot overwrite it.
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
set OPTS=/nologo /utf-8 /std:c++17 /EHsc /MT /O2 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /W3
if /i "%~1"=="test" goto test
if not exist build mkdir build
rc /nologo photocrop.rc
cl %OPTS% photocrop.cpp photocrop.res /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /OUT:build\PhotoCrop.exe
if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)
del /q photocrop.obj photocrop.res 2>nul
rem a freshly linked exe is briefly locked (antivirus scan), so retry the zip a few times
powershell -NoProfile -Command "for($i=0;$i -lt 10;$i++){ try { Compress-Archive -Force -Path 'build\PhotoCrop.exe','README.txt' -DestinationPath 'build\PhotoCrop.zip' -ErrorAction Stop; break } catch { Start-Sleep 2 } }"
echo Built build\PhotoCrop.exe
exit /b 0

:test
set T=%TEMP%\photocrop_build
if not exist "%T%" mkdir "%T%"
cl %OPTS% /Fo"%T%\\" tests.cpp /link /SUBSYSTEM:CONSOLE /MANIFEST:EMBED /OUT:"%T%\tests.exe"
if errorlevel 1 (
  echo TEST BUILD FAILED
  exit /b 1
)
"%T%\tests.exe"
exit /b %errorlevel%
