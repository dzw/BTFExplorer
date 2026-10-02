@echo off
rem Remove the obsolete per-user override created by older versions of register-wine.bat.
reg delete "HKCU\Software\Classes\CLSID\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\shell\open" /f
if errorlevel 1 (
  echo No obsolete override was found.
) else (
  echo Removed the obsolete override.
)
