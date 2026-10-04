@echo off
rem Compiles bfbbtest.vmf into bfbbtest.bsp and installs it in your mod. No Hammer needed.
rem
rem EDIT THESE TWO LINES to match your machine:
set SDKBASE=C:\Program Files (x86)\Steam\steamapps\common\Source SDK Base 2013 Multiplayer
set MODDIR=C:\path\to\source-sdk-2013\game\mod_tf

cd /d "%~dp0"
python make_bfbbtest_vmf.py bfbbtest.vmf || goto :fail

"%SDKBASE%\bin\x64\vbsp.exe" -game "%MODDIR%" "%~dp0bfbbtest" || goto :fail
"%SDKBASE%\bin\x64\vvis.exe" -game "%MODDIR%" -fast "%~dp0bfbbtest" || goto :fail
"%SDKBASE%\bin\x64\vrad.exe" -game "%MODDIR%" -fast "%~dp0bfbbtest" || goto :fail

if not exist "%MODDIR%\maps" mkdir "%MODDIR%\maps"
copy /y "%~dp0bfbbtest.bsp" "%MODDIR%\maps\bfbbtest.bsp" || goto :fail

echo.
echo Done. In the TF2 mod console:  map bfbbtest
exit /b 0

:fail
echo.
echo FAILED -- read the messages above. Common causes: wrong SDKBASE/MODDIR, or python not on PATH.
exit /b 1
