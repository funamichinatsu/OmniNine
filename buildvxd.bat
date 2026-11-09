@echo off
set INCLUDE=C:\98DDK\inc\win98;%INCLUDE%
set PATH=C:\98DDK\BIN;C:\WATCOM\binnt;%PATH%

nmake LINK="C:\98DDK\BIN\LINK.EXE" minivdd.vxd
