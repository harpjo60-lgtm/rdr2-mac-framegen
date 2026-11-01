#!/bin/sh
# Builds version.dll (64-bit Windows) with MinGW-w64: brew install mingw-w64
# slfg.cpp (Streamline integration) needs NVIDIA's Streamline headers in ../streamline/src/include.
set -e
cd "$(dirname "$0")"
x86_64-w64-mingw32-gcc -O2 -Wall -c -o ngxlog.o ngxlog.c
x86_64-w64-mingw32-g++ -O2 -Wall -std=c++17 -I../streamline/src/include -c -o slfg.o slfg.cpp
x86_64-w64-mingw32-g++ -O2 -shared -s -o version.dll ngxlog.o slfg.o version.def \
    -static-libgcc -static-libstdc++ -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic -lpsapi -ldxguid
rm -f ngxlog.o slfg.o
echo "built $(pwd)/version.dll"
