@echo off
setlocal
cd /d "%~dp0"

if not exist build\PagedExplorer.exe (
  echo Not built yet, building first...
  call build.bat || exit /b 1
)

start "" build\PagedExplorer.exe
endlocal
