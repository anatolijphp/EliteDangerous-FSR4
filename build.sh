#!/bin/sh
# Build ED-FSR (dinput8.dll proxy) with llvm-mingw.   OUT=build/dev sh build.sh  -> dev build
# Tool locations can be overridden via env: LLVM_MINGW, FFX_SDK, IMGUI_DIR, RENDERDOC_INC.
set -e
LLVM_MINGW=${LLVM_MINGW:-/d/tools/llvm-mingw-20261020-ucrt-x86_64}
FFX_SDK=${FFX_SDK:-/d/tools/fidelityfx-sdk-2.4.0}
IMGUI_DIR=${IMGUI_DIR:-/d/tools/imgui-1.91.8}
RENDERDOC_INC=${RENDERDOC_INC:-/d/tools/renderdoc}
T=$LLVM_MINGW/bin
FFX=$FFX_SDK/Kits/FidelityFX
IMGUI=$IMGUI_DIR
CXX="$T/x86_64-w64-mingw32-clang++ -std=c++17 -O2 -Wno-unused-value -Wno-deprecated-declarations"
IMGUI_DEFS="-DIM_ASSERT(_EXPR)=((void)0) -DIMGUI_IMPL_WIN32_DISABLE_GAMEPAD -DIMGUI_DISABLE_DEMO_WINDOWS"
cd "$(dirname "$0")"
mkdir -p build build/dev build/obj logs bin

# Dear ImGui objects (built once; rebuilt when IMGUI_DEFS change)
[ "$(cat build/obj/defs.txt 2>/dev/null)" = "$IMGUI_DEFS" ] || { rm -f build/obj/*.o; printf '%s\n' "$IMGUI_DEFS" > build/obj/defs.txt; }
IMOBJ=""
for f in imgui imgui_draw imgui_tables imgui_widgets backends/imgui_impl_dx11 backends/imgui_impl_win32; do
  o=build/obj/$(basename $f).o
  [ $o -nt $IMGUI/$f.cpp ] || $CXX $IMGUI_DEFS -c -I$IMGUI $IMGUI/$f.cpp -o $o
  IMOBJ="$IMOBJ $o"
done

UI_SRC=""; [ -f src/ui.cpp ] && UI_SRC=src/ui.cpp
$CXX $IMGUI_DEFS -shared -static \
  -I$RENDERDOC_INC -I$FFX/api/include -I$FFX/upscalers/include -I$FFX/framegeneration/include -I$IMGUI -I$IMGUI/backends \
  src/edfsr.cpp src/fsr.cpp src/fg.cpp $UI_SRC $IMOBJ src/dinput8.def -o ${OUT:-build}/dinput8.dll \
  -ld3d11 -ld3dcompiler -ldxgi -ldxguid -luuid -ldwmapi -limm32 -lgdi32

# FidelityFX signed runtime DLLs (loader resolves effect DLLs from its own folder)
for f in amd_fidelityfx_loader_dx12.dll amd_fidelityfx_upscaler_dx12.dll amd_fidelityfx_framegeneration_dx12.dll; do
  [ bin/$f -nt $FFX/signedbin/$f ] || cp $FFX/signedbin/$f bin/
done
echo "built ${OUT:-build}/dinput8.dll"
