#pragma once
// ED-FSR in-game settings overlay (Dear ImGui 1.91.8, D3D11 + Win32).
// Toast for ~8 s after the first ui_frame, Ctrl+F8 toggles the settings panel, ESC closes it.
// Test/debug: env var EDFSR_UI_OPEN=1 (read in ui_init) opens the panel immediately.
#include <windows.h>
#include <d3d11.h>
struct UiState {
    // editable settings (UI writes, sets dirty=true when user changes something)
    int   upscaler;          // 0 = game's FSR1 (mod off), 1 = FSR 4 (ML), 2 = FSR 3.1
    bool  fsr4Available;     // if false, grey out FSR 4 option
    float sharpness;         // 0..1
    bool  framegen;          // frame generation on/off
    bool  framegenAvailable; // false -> show "restart the game with frame generation enabled" hint, checkbox still editable (saved for next start)
    bool  jitter, motionVectors;
    float cockpitDepth;      // 0..20
    bool  debugView;
    // read-only info
    unsigned renderW, renderH, outW, outH;
    const char *upscalerName;   // e.g. "FSR 4.1.2" or "FSR1 (game)"
    const char *fgName;         // e.g. "FSR FG 4.0.2" or "off"
    float fps;                  // presented-by-game fps
    // flags set by UI (reset to false at the start of every ui_frame call)
    bool dirty;                 // any setting changed this frame
    bool saveRequested;         // user pressed SAVE
};
bool ui_init(HWND gameWindow, ID3D11Device *dev, ID3D11DeviceContext *ctx); // once; subclasses the game window proc
void ui_frame(ID3D11RenderTargetView *rtv, unsigned w, unsigned h, UiState &st); // every frame right before Present
bool ui_is_open();
void ui_on_resize();  // backbuffer resized/recreated; release anything size-dependent
void ui_hook_dinput(void *pDirectInput8);  // IDirectInput8W* (or A): hooks CreateDevice -> GetDeviceState/GetDeviceData
