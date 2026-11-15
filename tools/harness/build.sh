#!/bin/sh
# Build the ED-FSR offline test harness. Output name MUST be dx11test.exe (the mod only activates for it / ED).
set -e
T=/d/tools/llvm-mingw-20261020-ucrt-x86_64/bin
cd /d/ed-fsr/tools/harness
$T/x86_64-w64-mingw32-clang++ -std=c++17 -O2 -static -Wno-deprecated-declarations \
  harness.cpp -o dx11test.exe -ld3d11 -ld3dcompiler -ldxgi -ldxguid -luuid
echo "built tools/harness/dx11test.exe"
