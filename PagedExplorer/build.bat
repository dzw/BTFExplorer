@echo off
setlocal
set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`%VSWHERE% -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (
  echo Visual Studio with C++ tools not found.
  exit /b 1
)
echo Using VS at: %VS%
call "%VS%\Common7\Tools\VsDevCmd.bat" -arch=x64 -no_logo

if not exist build mkdir build

cl /nologo /std:c++20 /EHsc /W3 /O2 /Zi /FS /utf-8 ^
  /DUNICODE /D_UNICODE /DNOMINMAX ^
  src\App\main.cpp src\Shell\*.cpp src\Pagination\*.cpp src\UI\*.cpp ^
  /Fe:build\PagedExplorer.exe /Fd:build\PagedExplorer.pdb /Fo:build\ ^
  /link /SUBSYSTEM:WINDOWS ^
  Comctl32.lib Shell32.lib Ole32.lib OleAut32.lib Shlwapi.lib Propsys.lib User32.lib Gdi32.lib Advapi32.lib

if errorlevel 1 (
  echo Build FAILED.
  exit /b 1
)
echo Build OK: build\PagedExplorer.exe
endlocal
