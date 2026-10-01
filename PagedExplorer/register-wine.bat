@echo off
rem 把 Win+E 重定向到 PagedExplorer（写入 HKCU，无需管理员权限）
rem 原理：Win+E 触发“此电脑”CLSID 的 shell\open 动词，同名 HKCU 键优先于系统定义

set "EXE=%~dp0build\PagedExplorer.exe"
if not exist "%EXE%" (
  echo 请先运行 build.bat 生成 build\PagedExplorer.exe
  exit /b 1
)

reg add "HKCU\Software\Classes\CLSID\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\shell\open\command" /ve /d "\"%EXE%\" \"%%1\"" /f
reg add "HKCU\Software\Classes\CLSID\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\shell\open" /v "DelegateExecute" /d "" /f
reg add "HKCU\Software\Classes\CLSID\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\shell\open" /v "Icon" /d "\"%EXE%\"" /f

echo 已生效：Win+E 将打开 PagedExplorer。重启资源管理器或注销后更可靠。
echo 还原请运行 unregister-wine.bat
