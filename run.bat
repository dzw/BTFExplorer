pskill PagedExplorer
@echo off
setlocal
cd /d "%~dp0"

@REM  if not exist build\PagedExplorer.exe (
@REM    echo Not built yet, building first...
@REM    call build.bat || exit /b 1
@REM  )

start "" bin\Release\PagedExplorer.exe
echo Log file: "%LOCALAPPDATA%\PagedExplorer\PagedExplorer.log"
endlocal
