pskill PagedExplorer
@echo off
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (
  echo Visual Studio with C++ tools not found.
  exit /b 1
)
echo Using VS at: %VS%
set "MSBUILD=%VS%\MSBuild\Current\Bin\MSBuild.exe"
if not exist "%MSBUILD%" (
  echo MSBuild not found: "%MSBUILD%"
  exit /b 1
)

pushd "%~dp0"
"%MSBUILD%" PagedExplorer.sln /m /nologo /p:Configuration=Release /p:Platform=x64
set "BUILD_RESULT=%ERRORLEVEL%"
popd

if not "%BUILD_RESULT%"=="0" (
  echo Build FAILED.
  exit /b %BUILD_RESULT%
)
echo Build OK: bin\Release\PagedExplorer.exe
endlocal


@REM  D:\msys64\ucrt64\bin\cmake.EXE -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE --no-warn-unused-cli -S D:/APrj/BinDir -B d:/APrj/BinDir/build -G "Visual Studio 17 2022" -T host=x64 -A x64