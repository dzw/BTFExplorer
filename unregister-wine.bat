@echo off
rem 还原 Win+E 为系统默认（删除 HKCU 劫持键）
reg delete "HKCU\Software\Classes\CLSID\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\shell\open" /f
echo 已还原：Win+E 恢复打开系统资源管理器。
