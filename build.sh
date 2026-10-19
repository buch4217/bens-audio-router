#!/bin/bash
# Baut BensAudioRouter.exe mit MinGW-w64 (Linux-Cross-Compile; unter Windows via MSYS2 mingw64 ebenfalls moeglich).
# Voraussetzung (Ubuntu/Debian): sudo apt install g++-mingw-w64-x86-64
set -e
cd "$(dirname "$0")"
x86_64-w64-mingw32-windres app.rc -O coff -o app.res.o
x86_64-w64-mingw32-gcc -O2 -c alac.c -o alac.o -w
x86_64-w64-mingw32-g++-posix -std=c++17 -O3 -ffast-math -Wall -Wextra -municode -mwindows -static -static-libgcc -static-libstdc++ \
  BensAudioRouter.cpp app.res.o alac.o -o BensAudioRouter.exe \
  -lole32 -loleaut32 -luuid -lcomctl32 -lavrt -lwinmm -lgdi32 -luser32 -lshell32 -lws2_32 -liphlpapi -ldwmapi -luxtheme -lcomdlg32 -ladvapi32
ls -la BensAudioRouter.exe
