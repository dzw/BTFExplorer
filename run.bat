@echo off
setlocal
cd /d "%~dp0"

@REM  if not exist build\PagedExplorer.exe (
@REM    echo Not built yet, building first...
@REM    call build.bat || exit /b 1
@REM  )

start "" build\PagedExplorer.exe
endlocal
