@echo off
rem Copyright (C) 2026 DmShAl (Shepeta Dmitry)
rem SPDX-License-Identifier: GPL-3.0-or-later
setlocal

set "ROOT=%~dp0"
if not defined GCC_PATH set "GCC_PATH=C:\Qt\Tools\mingw1310_64\bin\gcc.exe"

if not exist "%GCC_PATH%" (
    echo GCC was not found at "%GCC_PATH%".
    echo Set GCC_PATH to the full path of gcc.exe and run this script again.
    exit /b 1
)

"%GCC_PATH%" -O2 -std=c11 -D_CRT_SECURE_NO_WARNINGS -DDICT_VERSION=\"1.13.0-Win64-1.0.0-v2\" -I "%ROOT%zlib" ^
    "%ROOT%src\dictzip.c" ^
    "%ROOT%src\data.c" ^
    "%ROOT%zlib\crc32.c" ^
    "%ROOT%zlib\zutil.c" ^
    "%ROOT%zlib\deflate.c" ^
    "%ROOT%zlib\inflate.c" ^
    "%ROOT%zlib\inftrees.c" ^
    "%ROOT%zlib\inffast.c" ^
    "%ROOT%zlib\adler32.c" ^
    "%ROOT%zlib\trees.c" ^
    -o "%ROOT%dictzip2.exe"

if errorlevel 1 exit /b %errorlevel%
echo Built "%ROOT%dictzip2.exe"
exit /b 0
