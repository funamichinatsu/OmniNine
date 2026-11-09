@echo off
rem Build Setup with Open Watcom.
rem Require Win32 headers: $(WATCOM)\h and $(WATCOM)\h\nt
set WATCOM=C:\WATCOM
set PATH=%WATCOM%\binnt;%PATH%
wcl386 -zq -bt=nt -l=nt_win -i=%WATCOM%\h;%WATCOM%\h\nt -fe=setup.exe setup.c
if errorlevel 1 goto fail
echo.
echo setup.exe built successfully.
goto end
:fail
echo Build FAILED.
:end
pause
wrc -q -bt=nt -fo=setup.res setup.rc
wcl386 -zq -bt=nt -l=nt_win -i=%WATCOM%\h;%WATCOM%\h\nt -fe=setup.exe setup.c setup.res
