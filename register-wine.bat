@echo off
setlocal

rem Win+E is intercepted by PagedExplorer while the application is running.
rem This script starts it for the current session; it does not configure auto-start.
set "EXE=%~dp0bin\Release\PagedExplorer.exe"
if not exist "%EXE%" (
  echo PagedExplorer.exe was not found: "%EXE%"
  echo Run build.bat first.
  exit /b 1
)

start "" "%EXE%"
echo PagedExplorer started. Keep it running for Win+E interception to work.
endlocal
