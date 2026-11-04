#!/bin/sh
# Builds version.dll (64-bit Windows) with MinGW-w64: brew install mingw-w64
# Needs NVIDIA's Streamline headers in ../streamline/src/include and Dear ImGui
# in ../imgui (see README, "Building").
set -e
cd "$(dirname "$0")"
I=../imgui
CXX="x86_64-w64-mingw32-g++ -O2 -std=c++17 -fno-threadsafe-statics"  # thread-safe statics would need libwinpthread-1.dll
x86_64-w64-mingw32-gcc -O2 -Wall -c -o ngxlog.o ngxlog.c
$CXX -Wall -I../streamline/src/include -c -o slfg.o slfg.cpp
$CXX -Wall -I$I -I$I/backends -c -o menu.o menu.cpp
for f in $I/imgui.cpp $I/imgui_draw.cpp $I/imgui_tables.cpp $I/imgui_widgets.cpp \
         $I/backends/imgui_impl_win32.cpp $I/backends/imgui_impl_dx12.cpp; do
    $CXX -I$I -DIMGUI_IMPL_WIN32_DISABLE_GAMEPAD -c -o "im_$(basename "$f" .cpp).o" "$f"
done
x86_64-w64-mingw32-g++ -O2 -shared -s -o version.dll ngxlog.o slfg.o menu.o im_*.o version.def \
    -static -lpsapi -ldxguid -ld3dcompiler -ldwmapi -lgdi32 -limm32  # -static: no MinGW runtime DLLs needed next to the game
rm -f ngxlog.o slfg.o menu.o im_*.o
echo "built $(pwd)/version.dll"
