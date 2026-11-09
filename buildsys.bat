@set WATCOM=C:\WATCOM
@set PATH=%WATCOM%\binnt;%PATH%
@set INCLUDE=%WATCOM%\H;%WATCOM%\H\WIN
@set LIB=%WATCOM%\lib286\win
@wasm -q -bt=DOS -mt fixsys.asm
@wlink system dos com file fixsys.obj name fixsys.sys
