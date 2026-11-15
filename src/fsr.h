#pragma once
#include <d3d11.h>

struct FsrFrame {
    ID3D11DeviceContext *ctx;
    ID3D11Texture2D *color;   // EASU input (render res, post tonemap+SMAA, RGBA8)
    ID3D11Texture2D *depth;   // scene depth (render res), may be null
    ID3D11Texture2D *output;  // EASU output (display res)
};

struct FsrSettings {
    int jitter = 1;
    float jitterSignX = 1.f, jitterSignY = 1.f;
    float cockpitDepth = 6.0f;   // view depth below which motion = 0 (cockpit moves with camera)
    int mv = 1;                  // 0 = zero motion vectors
    float sharpness = 0.f;       // FSR internal sharpening (game RCAS runs afterwards anyway)
    int debugView = 0;
    int version = 1;             // 1 = FSR 4 (ML), 2 = FSR 3.1
    int reactive = 1;            // reactive mask from (final HDR - opaque snapshot)
    float reactiveScale = 1.0f;  // mask strength
};
extern FsrSettings g_fsrCfg;

// Upscaling quality mode -> game render scale (ED "SSAAMultiplier"). 0 = keep the game's own setting.
enum { QM_GAME, QM_ULTRA_QUALITY, QM_QUALITY, QM_BALANCED, QM_PERFORMANCE, QM_ULTRA_PERFORMANCE, QM_NATIVE_AA, QM_COUNT };
extern int g_qualityMode;                       // from ini "quality_mode", applied at game start
const char *quality_mode_key(int m);            // ini token
float quality_mode_scale(int m);                // render scale (0 for QM_GAME)

// Original (unhooked) context functions, filled by the hook layer.
struct CtxFns {
    void(STDMETHODCALLTYPE *CSSetShader)(ID3D11DeviceContext *, ID3D11ComputeShader *, ID3D11ClassInstance *const *, UINT);
    void(STDMETHODCALLTYPE *CSSetShaderResources)(ID3D11DeviceContext *, UINT, UINT, ID3D11ShaderResourceView *const *);
    void(STDMETHODCALLTYPE *CSSetUnorderedAccessViews)(ID3D11DeviceContext *, UINT, UINT, ID3D11UnorderedAccessView *const *, const UINT *);
};
extern CtxFns g_o;

bool fsr_failed();
bool fsr_run(const FsrFrame &f);           // true = output written
void fsr_capture_frame_cb(ID3D11DeviceContext *ctx);  // copy per-frame camera cbuffer (VS b1) during scene pass
void fsr_jitter(float &x, float &y);       // current jitter in render pixels
void fsr_reset();                          // drop history (toggle / scene cut)
void logf(const char *fmt, ...);
const char *mod_root();                  // data folder: edfsr.ini, bin\, logs\

// ---- shared bridge state (used by frame generation)
struct ID3D12Device; struct ID3D12CommandQueue; struct ID3D12Fence; struct ID3D12Resource;
struct ID3D11Fence; struct ID3D11DeviceContext4;
struct BridgeState {
    ID3D12Device *d12; ID3D12CommandQueue *q12; ID3D12Fence *fence12; ID3D11Fence *fence11; ID3D11DeviceContext4 *ctx4;
    unsigned long long *fval;            // shared monotonically increasing fence counter
    void *ffxCreate, *ffxDestroy, *ffxConfigure, *ffxQuery, *ffxDispatch;
    ID3D12Resource *depth, *mv;          // render-res, state COMMON between frames
    unsigned rw, rh;
    float jx, jy, dtMs;                  // jitter used for the last upscaled frame
    unsigned long long frameNo;          // incremented by every fsr_run
    float camPos[3], right[3], up[3], forward[3]; bool camValid;
};
bool fsr_bridge_init(ID3D11Device *dev, ID3D11DeviceContext *ctx);
BridgeState *fsr_state();

// ---- frame generation (fg.cpp)
void fg_install_factory_hooks(void *factory);   // called with every IDXGIFactory the game creates
struct FgSettings { int enabled = 0; int startOn = 1; int debug = 0; };  // enabled = route presents through the FG swapchain; startOn = FG on at start
extern FgSettings g_fgCfg;

// upscaler info for UI
const char *fsr_provider_name();   // active upscaler provider, e.g. "4.1.2"
bool fsr_has_fsr4();
void fsr_request_reinit();         // re-create upscaler context (version change)

// edfsr.cpp
bool mod_upscaler_active();
void mod_hook_device(ID3D11Device *dev);
void mod_verify_hooks(ID3D11DeviceContext *ctx);  // re-patch rewritten vtable slots   // hook a device/context (safe to call repeatedly)
void mod_set_upscaler_active(bool on);
// fg.cpp
void fg_on_dinput_created(void *pDirectInput8);
void fsr_snapshot_opaque(ID3D11DeviceContext *ctx, ID3D11Texture2D *hdr);  // end of opaque pass (reactive mask)
