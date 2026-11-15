// ED-FSR: frame generation. The game's D3D11 swapchain is redirected to a hidden window; each Present
// copies the game backbuffer into a shared texture and presents it through an AMD FidelityFX
// frame-generation proxy swapchain (D3D12) created on the real window.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <mutex>
#include "fsr.h"
#include "ui.h"
#include "ffx_api.h"
#include "ffx_upscale.h"
#include "dx12/ffx_api_dx12.h"
#include "ffx_framegeneration.h"
#include "dx12/ffx_api_framegeneration_dx12.h"

template <class T> static void rel(T *&p) { if (p) { p->Release(); p = nullptr; } }

FgSettings g_fgCfg;

// ------------------------------------------------------------------ state
static IDXGISwapChain *g_gameSc;        // game's swapchain (on hidden window)
static HWND g_realHwnd, g_hiddenHwnd;
static IDXGISwapChain4 *g_fgSc;         // FFX proxy swapchain on the real window
static ffxContext g_fgSwapCtx, g_fgCtx;
static UINT g_w, g_h; static DXGI_FORMAT g_fmt;
static ID3D11Texture2D *g_share11; static ID3D12Resource *g_share12;
static ID3D12CommandAllocator *g_alloc[3]; static UINT64 g_allocFence[3];
static ID3D12GraphicsCommandList *g_cl;
static HANDLE g_evt;
static UINT64 g_frameID, g_lastFsrFrame;
static bool g_fgActive = true, g_failed;
static std::mutex g_fgMx;
static BridgeState *bs;
static PfnFfxCreateContext pCreate; static PfnFfxDestroyContext pDestroy; static PfnFfxConfigure pConfigure; static PfnFfxDispatch pDispatch;

typedef HRESULT(STDMETHODCALLTYPE *PFN_CreateSC)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **);
typedef HRESULT(STDMETHODCALLTYPE *PFN_Present)(IDXGISwapChain *, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE *PFN_ResizeBuffers)(IDXGISwapChain *, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef HRESULT(STDMETHODCALLTYPE *PFN_SetFS)(IDXGISwapChain *, BOOL, IDXGIOutput *);
typedef HRESULT(STDMETHODCALLTYPE *PFN_GetFS)(IDXGISwapChain *, BOOL *, IDXGIOutput **);
typedef HRESULT(STDMETHODCALLTYPE *PFN_ResizeTarget)(IDXGISwapChain *, const DXGI_MODE_DESC *);
static char g_fgName[64] = "off";
static PFN_CreateSC oCreateSC; static PFN_Present oPresent; static PFN_ResizeBuffers oResizeBuffers;
static PFN_SetFS oSetFS; static PFN_GetFS oGetFS; static PFN_ResizeTarget oResizeTarget;

static void *hook_vt(void *obj, int idx, void *fn) {
    void **vt = *(void ***)obj; void *orig = vt[idx];
    if (orig == fn) return 0;
    DWORD old; VirtualProtect(&vt[idx], sizeof(void *), PAGE_READWRITE, &old);
    vt[idx] = fn; VirtualProtect(&vt[idx], sizeof(void *), old, &old);
    return orig;
}

static void ffxMsg(uint32_t type, const wchar_t *m) {
    char b[1024]; WideCharToMultiByte(CP_UTF8, 0, m, -1, b, sizeof b, 0, 0);
    logf("FFX-FG %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "ERROR" : "warn", b);
}

// ------------------------------------------------------------------ hidden window
static HWND make_hidden_window(UINT w, UINT h) {
    static ATOM cls;
    if (!cls) {
        WNDCLASSW wc = {}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(0); wc.lpszClassName = L"EDFSR_Hidden";
        cls = RegisterClassW(&wc);
    }
    return CreateWindowExW(WS_EX_TOOLWINDOW, L"EDFSR_Hidden", L"edfsr", WS_POPUP, 0, 0, (int)w, (int)h, 0, 0, GetModuleHandleW(0), 0);
}

// ------------------------------------------------------------------ FG resources
static void destroy_share() { rel(g_share12); rel(g_share11); }

static bool make_share(ID3D11Device *d11) {
    destroy_share();
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = g_w; td.Height = g_h; td.MipLevels = 1; td.ArraySize = 1; td.Format = g_fmt; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    HRESULT hr = d11->CreateTexture2D(&td, 0, &g_share11);
    if (FAILED(hr)) { logf("FG share tex failed 0x%08lx", (unsigned long)hr); return false; }
    IDXGIResource1 *xr = 0; HANDLE hd = 0;
    hr = g_share11->QueryInterface(__uuidof(IDXGIResource1), (void **)&xr);
    if (SUCCEEDED(hr)) hr = xr->CreateSharedHandle(0, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, 0, &hd);
    rel(xr);
    if (SUCCEEDED(hr)) { hr = bs->d12->OpenSharedHandle(hd, __uuidof(ID3D12Resource), (void **)&g_share12); CloseHandle(hd); }
    if (FAILED(hr)) { logf("FG share open failed 0x%08lx", (unsigned long)hr); return false; }
    return true;
}

static void destroy_fg_ctx() {
    if (!g_fgCtx) return;
    ffxConfigureDescFrameGeneration c = {}; c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    c.swapChain = g_fgSc; c.frameGenerationEnabled = false;
    pConfigure(&g_fgCtx, &c.header);
    pDestroy(&g_fgCtx, 0); g_fgCtx = 0;
}

static bool create_fg_ctx() {
    destroy_fg_ctx();
    ffxCreateBackendDX12Desc be = {}; be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12; be.device = bs->d12;
    ffxCreateContextDescFrameGenerationVersion ver = {}; ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    ver.version = FFX_FRAMEGENERATION_VERSION; ver.header.pNext = &be.header;
    ffxCreateContextDescFrameGeneration cd = {}; cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION; cd.header.pNext = &ver.header;
    cd.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED | FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE;
    cd.displaySize = {g_w, g_h}; cd.maxRenderSize = {g_w, g_h};
    cd.backBufferFormat = ffxApiGetSurfaceFormatDX12(g_fmt);
    ffxReturnCode_t rc = pCreate(&g_fgCtx, &cd.header, 0);
    if (rc != FFX_API_RETURN_OK) { logf("FG context create rc=%u", rc); g_fgCtx = 0; return false; }
    ffxQueryGetProviderVersion pv = {}; pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    ((PfnFfxQuery)bs->ffxQuery)(&g_fgCtx, &pv.header);
    snprintf(g_fgName, sizeof g_fgName, "FSR FG %s", pv.versionName ? pv.versionName : "?");
    logf("FG context: provider '%s' %ux%u fmt %d", pv.versionName ? pv.versionName : "?", g_w, g_h, g_fmt);
    return true;
}

static bool create_fg_swapchain(IDXGIFactory *factory) {
    pCreate = (PfnFfxCreateContext)bs->ffxCreate; pDestroy = (PfnFfxDestroyContext)bs->ffxDestroy;
    pConfigure = (PfnFfxConfigure)bs->ffxConfigure; pDispatch = (PfnFfxDispatch)bs->ffxDispatch;
    if (!pCreate || !pConfigure || !pDispatch) return false;

    DXGI_SWAP_CHAIN_DESC1 d1 = {};
    d1.Width = g_w; d1.Height = g_h; d1.Format = g_fmt; d1.SampleDesc.Count = 1;
    d1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d1.BufferCount = 3;
    d1.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; d1.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    IDXGIFactory5 *f5 = 0; BOOL tearing = FALSE;
    if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory5), (void **)&f5))) {
        f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof tearing); rel(f5);
    }
    if (tearing) d1.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    ffxCreateContextDescFrameGenerationSwapChainVersionDX12 ver = {};
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12; ver.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;
    ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 cd = {};
    cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12; cd.header.pNext = &ver.header;
    cd.swapchain = &g_fgSc; cd.hwnd = g_realHwnd; cd.desc = &d1; cd.fullscreenDesc = 0;
    cd.dxgiFactory = factory; cd.gameQueue = bs->q12;
    ffxReturnCode_t rc = pCreate(&g_fgSwapCtx, &cd.header, 0);
    if (rc != FFX_API_RETURN_OK || !g_fgSc) { logf("FG swapchain create rc=%u", rc); return false; }

    for (auto &a : g_alloc)
        if (FAILED(bs->d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void **)&a))) return false;
    if (FAILED(bs->d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[0], 0, __uuidof(ID3D12GraphicsCommandList), (void **)&g_cl))) return false;
    g_cl->Close();
    g_evt = CreateEventW(0, FALSE, FALSE, 0);
    logf("FG swapchain created on hwnd %p (%ux%u, tearing=%d)", (void *)g_realHwnd, g_w, g_h, tearing);
    return create_fg_ctx();
}

static ffxReturnCode_t fg_dispatch_cb(ffxDispatchDescFrameGeneration *p, void *ctx) {
    return pDispatch((ffxContext *)ctx, &p->header);
}

static void barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b, D3D12_RESOURCE_BARRIER &out) {
    out = {}; out.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; out.Transition.pResource = r;
    out.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; out.Transition.StateBefore = a; out.Transition.StateAfter = b;
}

// ------------------------------------------------------------------ settings overlay glue
static UiState g_ui;
static int *g_uiQualityPending;
static bool g_uiInit;
static ID3D11RenderTargetView *g_bbRtv; static ID3D11Texture2D *g_bbRtvTex;
static LARGE_INTEGER g_lastPresent, g_qpf; static float g_fps;
static char g_upName[64];

static void save_ini() {
    char ini[MAX_PATH]; snprintf(ini, sizeof ini, "%s\\edfsr.ini", mod_root()); char b[32];
    WritePrivateProfileStringA("edfsr", "enabled", "1", ini);
    WritePrivateProfileStringA("edfsr", "upscaler", g_ui.upscaler == 0 ? "game" : g_ui.upscaler == 2 ? "fsr31" : "fsr4", ini);
    snprintf(b, sizeof b, "%.3f", g_ui.sharpness); WritePrivateProfileStringA("edfsr", "sharpness", b, ini);
    WritePrivateProfileStringA("edfsr", "framegen", g_ui.framegen ? "1" : "0", ini);
    WritePrivateProfileStringA("edfsr", "jitter", g_ui.jitter ? "1" : "0", ini);
    WritePrivateProfileStringA("edfsr", "motion_vectors", g_ui.motionVectors ? "1" : "0", ini);
    snprintf(b, sizeof b, "%.2f", g_ui.cockpitDepth); WritePrivateProfileStringA("edfsr", "cockpit_depth", b, ini);
    WritePrivateProfileStringA("edfsr", "debug_view", g_ui.debugView ? "1" : "0", ini);
    WritePrivateProfileStringA("edfsr", "reactive_mask", g_ui.reactive ? "1" : "0", ini);
    WritePrivateProfileStringA("edfsr", "quality_mode", quality_mode_key(g_ui.qualityMode), ini);
    snprintf(b, sizeof b, "%.2f", g_ui.reactiveScale); WritePrivateProfileStringA("edfsr", "reactive_scale", b, ini);
    logf("settings saved");
}

static void overlay(IDXGISwapChain *sc) {
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
    if (g_lastPresent.QuadPart) {
        float dt = (float)(now.QuadPart - g_lastPresent.QuadPart) / g_qpf.QuadPart;
        if (dt > 0) g_fps = g_fps ? g_fps * 0.95f + 0.05f / dt : 1.f / dt;
    }
    g_lastPresent = now;

    ID3D11Texture2D *bb = 0; sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb);
    if (!bb) return;
    ID3D11Device *d11 = 0; bb->GetDevice(&d11);
    mod_hook_device(d11);  // watchdog: re-hook if the runtime swapped the context vtable
    ID3D11DeviceContext *ctx = 0; d11->GetImmediateContext(&ctx);
    mod_verify_hooks(ctx);
    if (!g_uiInit) { g_uiInit = true; if (!ui_init(g_realHwnd, d11, ctx)) logf("ui_init failed"); else logf("settings overlay ready (CTRL+F8)"); }
    D3D11_TEXTURE2D_DESC td; bb->GetDesc(&td);
    if (bb != g_bbRtvTex) {
        rel(g_bbRtv); g_bbRtvTex = bb;
        D3D11_RENDER_TARGET_VIEW_DESC rd = {}; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        rd.Format = td.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ? DXGI_FORMAT_R8G8B8A8_UNORM
                  : td.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ? DXGI_FORMAT_B8G8R8A8_UNORM : td.Format;
        d11->CreateRenderTargetView(bb, &rd, &g_bbRtv);
    }

    BridgeState *b = fsr_state();
    bool upOn = mod_upscaler_active();
    g_ui.upscaler = !upOn ? 0 : (g_fsrCfg.version == 2 ? 2 : 1);
    g_ui.fsr4Available = fsr_has_fsr4();
    g_ui.sharpness = g_fsrCfg.sharpness;
    g_ui.framegen = g_fgSc ? g_fgActive : g_fgCfg.enabled != 0;
    g_ui.framegenAvailable = g_fgSc != 0;
    g_ui.jitter = g_fsrCfg.jitter != 0; g_ui.motionVectors = g_fsrCfg.mv != 0;
    g_ui.cockpitDepth = g_fsrCfg.cockpitDepth; g_ui.debugView = g_fsrCfg.debugView != 0;
    g_ui.reactive = g_fsrCfg.reactive != 0; g_ui.reactiveScale = g_fsrCfg.reactiveScale;
    { static int pending = -1; static int started = g_qualityMode;
      g_ui.qualityModeActive = started; g_ui.qualityMode = pending >= 0 ? pending : started;
      g_uiQualityPending = &pending; }
    g_ui.renderW = b->rw; g_ui.renderH = b->rh; g_ui.outW = td.Width; g_ui.outH = td.Height;
    if (upOn && !fsr_failed()) snprintf(g_upName, sizeof g_upName, "FSR %s", fsr_provider_name());
    else snprintf(g_upName, sizeof g_upName, "FSR1 (game)");
    g_ui.upscalerName = g_upName;
    g_ui.fgName = (g_fgSc && g_fgActive && upOn) ? g_fgName : "off";
    g_ui.fps = g_fps;
    g_ui.dirty = g_ui.saveRequested = false;

    if (g_bbRtv) ui_frame(g_bbRtv, td.Width, td.Height, g_ui);
    mod_verify_hooks(ctx);  // SwapDeviceContextState in the overlay can rewrite context vtable slots
    {   // debug: EDFSR_UI_DUMP=<file.bmp> dumps the backbuffer (with overlay) at present #120
        static int n; char path[MAX_PATH];
        if (++n == 120 && GetEnvironmentVariableA("EDFSR_UI_DUMP", path, sizeof path)) {
            D3D11_TEXTURE2D_DESC sd = td; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
            ID3D11Texture2D *st = 0;
            if (SUCCEEDED(d11->CreateTexture2D(&sd, 0, &st))) {
                ctx->CopyResource(st, bb);
                D3D11_MAPPED_SUBRESOURCE m;
                if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
                    FILE *f = fopen(path, "wb");
                    if (f) {
                        int W = td.Width, H = td.Height; unsigned sz = 54 + W * H * 4;
                        unsigned char hdr[54] = {'B', 'M'};
                        memcpy(hdr + 2, &sz, 4); unsigned off = 54; memcpy(hdr + 10, &off, 4);
                        unsigned hs = 40; memcpy(hdr + 14, &hs, 4); int hh = -H; memcpy(hdr + 18, &W, 4); memcpy(hdr + 22, &hh, 4);
                        hdr[26] = 1; hdr[28] = 32;
                        fwrite(hdr, 1, 54, f);
                        for (int y = 0; y < H; y++) {
                            const unsigned char *row = (const unsigned char *)m.pData + y * m.RowPitch;
                            for (int x = 0; x < W; x++) { unsigned char px[4] = {row[x * 4 + 2], row[x * 4 + 1], row[x * 4 + 0], 255}; fwrite(px, 1, 4, f); }
                        }
                        fclose(f); logf("UI dump -> %s", path);
                    }
                    ctx->Unmap(st, 0);
                }
                st->Release();
            }
        }
    }

    if (g_ui.dirty) {
        if (g_ui.upscaler == 0) mod_set_upscaler_active(false);
        else {
            int v = g_ui.upscaler == 2 ? 2 : 1;
            mod_set_upscaler_active(true);
            if (v != g_fsrCfg.version) { g_fsrCfg.version = v; fsr_request_reinit(); logf("upscaler version -> %s", v == 2 ? "FSR 3.1" : "FSR 4"); }
        }
        g_fsrCfg.sharpness = g_ui.sharpness;
        g_fsrCfg.jitter = g_ui.jitter; g_fsrCfg.mv = g_ui.motionVectors;
        g_fsrCfg.cockpitDepth = g_ui.cockpitDepth;
        g_fsrCfg.debugView = g_ui.debugView;
        g_fsrCfg.reactive = g_ui.reactive; g_fsrCfg.reactiveScale = g_ui.reactiveScale;
        if (g_uiQualityPending) *g_uiQualityPending = g_ui.qualityMode;
        if (g_fgSc) { if (g_fgActive != g_ui.framegen) logf("FG: %s", g_ui.framegen ? "on" : "off"); g_fgActive = g_ui.framegen; }
        else g_fgCfg.enabled = g_ui.framegen;  // takes effect after restart (when saved)
    }
    if (g_ui.saveRequested) save_ini();
    rel(ctx); rel(d11); rel(bb);
}

void fg_on_dinput_created(void *di) { if (di) ui_hook_dinput(di); }

// ------------------------------------------------------------------ swapchain hooks
static HRESULT STDMETHODCALLTYPE hPresent(IDXGISwapChain *sc, UINT sync, UINT flags) {
    if (sc != g_gameSc) return oPresent(sc, sync, flags);
    if (flags & DXGI_PRESENT_TEST) return g_fgSc ? S_OK : oPresent(sc, sync, flags);
    overlay(sc);
    if (!g_fgSc) return oPresent(sc, sync, flags);
    std::lock_guard<std::mutex> lk(g_fgMx);

    ID3D11Texture2D *bb = 0;
    sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb);
    if (!bb) return S_OK;
    ID3D11Device *d11 = 0; bb->GetDevice(&d11);
    ID3D11DeviceContext *ctx = 0; d11->GetImmediateContext(&ctx);
    if (!g_share11 && !make_share(d11)) { g_failed = true; }
    if (g_share11) ctx->CopyResource(g_share11, bb);
    rel(bb);

    // D3D11 -> D3D12 handoff
    UINT64 v11 = ++*bs->fval;
    bs->ctx4->Signal(bs->fence11, v11);
    ctx->Flush();

    g_frameID++;
    bool fresh = bs->frameNo != g_lastFsrFrame && bs->depth && bs->mv;  // upscaler ran this frame -> depth/MV valid
    g_lastFsrFrame = bs->frameNo;
    bool fgOn = g_fgActive && fresh && g_fgCtx && !g_failed && !ui_is_open() && mod_upscaler_active();

    // configure (every frame)
    ffxConfigureDescFrameGeneration c = {}; c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    c.swapChain = g_fgSc; c.frameGenerationEnabled = fgOn;
    c.frameGenerationCallback = fg_dispatch_cb; c.frameGenerationCallbackUserContext = &g_fgCtx;
    c.allowAsyncWorkloads = false; c.flags = g_fgCfg.debug ? (FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_TEAR_LINES | FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_PACING_LINES) : 0;
    c.generationRect = {0, 0, (int)g_w, (int)g_h}; c.frameID = g_frameID;
    if (g_fgCtx) {
        ffxReturnCode_t rc = pConfigure(&g_fgCtx, &c.header);
        if (rc != FFX_API_RETURN_OK) { static int n; if (n++ < 5) logf("FG configure rc=%u", rc); }
    }

    int ai = (int)(g_frameID % 3);
    if (bs->fence12->GetCompletedValue() < g_allocFence[ai]) { bs->fence12->SetEventOnCompletion(g_allocFence[ai], g_evt); WaitForSingleObject(g_evt, INFINITE); }
    g_alloc[ai]->Reset(); g_cl->Reset(g_alloc[ai], 0);

    // prepare (depth + motion vectors + camera)
    if (fgOn) {
        D3D12_RESOURCE_BARRIER br[2];
        barrier(bs->depth, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, br[0]);
        barrier(bs->mv, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, br[1]);
        g_cl->ResourceBarrier(2, br);
        ffxDispatchDescFrameGenerationPrepareV2 p = {}; p.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
        p.frameID = g_frameID; p.flags = 0; p.commandList = g_cl;
        p.renderSize = {bs->rw, bs->rh}; p.jitterOffset = {bs->jx, bs->jy}; p.motionVectorScale = {(float)bs->rw, (float)bs->rh};
        p.frameTimeDelta = bs->dtMs; p.reset = false;
        p.cameraNear = 0.025f; p.cameraFar = 1.0e8f; p.cameraFovAngleVertical = 0.98f; p.viewSpaceToMetersFactor = 1.0f;
        p.depth = ffxApiGetResourceDX12(bs->depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        p.motionVectors = ffxApiGetResourceDX12(bs->mv, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        for (int i = 0; i < 3; i++) { p.cameraPosition[i] = bs->camPos[i]; p.cameraUp[i] = bs->up[i]; p.cameraRight[i] = bs->right[i]; p.cameraForward[i] = bs->forward[i]; }
        if (!bs->camValid) { p.cameraForward[2] = 1; p.cameraUp[1] = 1; p.cameraRight[0] = 1; }
        ffxReturnCode_t rc = pDispatch(&g_fgCtx, &p.header);
        if (rc != FFX_API_RETURN_OK) { static int n; if (n++ < 5) logf("FG prepare rc=%u", rc); }
        for (auto &b : br) std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        g_cl->ResourceBarrier(2, br);
    }

    // copy game frame into the proxy swapchain's current backbuffer
    UINT idx = g_fgSc->GetCurrentBackBufferIndex();
    ID3D12Resource *dst = 0; g_fgSc->GetBuffer(idx, __uuidof(ID3D12Resource), (void **)&dst);
    if (dst && g_share12) {
        D3D12_RESOURCE_BARRIER br[2];
        barrier(dst, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST, br[0]);
        barrier(g_share12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE, br[1]);
        g_cl->ResourceBarrier(2, br);
        g_cl->CopyResource(dst, g_share12);
        for (auto &b : br) std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        g_cl->ResourceBarrier(2, br);
    }
    rel(dst);
    g_cl->Close();
    bs->q12->Wait(bs->fence12, v11);
    ID3D12CommandList *lists[] = {g_cl};
    bs->q12->ExecuteCommandLists(1, lists);
    UINT64 v12 = ++*bs->fval;
    bs->q12->Signal(bs->fence12, v12);
    g_allocFence[ai] = v12;
    // D3D11 must not overwrite depth/MV/share before D3D12 consumed them
    bs->ctx4->Wait(bs->fence11, v12);
    rel(ctx); rel(d11);

    UINT pflags = flags & ~DXGI_PRESENT_RESTART;
    if (sync == 0) {
        DXGI_SWAP_CHAIN_DESC1 d; g_fgSc->GetDesc1(&d);
        if (d.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) pflags |= DXGI_PRESENT_ALLOW_TEARING;
    }
    HRESULT hr = g_fgSc->Present(sync, pflags);
    static int logged; if (!logged++) logf("first FG present hr=0x%08lx fgOn=%d", (unsigned long)hr, fgOn);
    return SUCCEEDED(hr) ? S_OK : hr;
}

static HRESULT STDMETHODCALLTYPE hResizeBuffers(IDXGISwapChain *sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) {
    if (sc == g_gameSc) { rel(g_bbRtv); g_bbRtvTex = 0; ui_on_resize(); }
    if (sc != g_gameSc || !g_fgSc) return oResizeBuffers(sc, n, w, h, f, fl);
    std::lock_guard<std::mutex> lk(g_fgMx);
    RECT rc; GetClientRect(g_realHwnd, &rc);
    if (!w) w = rc.right - rc.left; if (!h) h = rc.bottom - rc.top;
    if (w && h) SetWindowPos(g_hiddenHwnd, 0, 0, 0, (int)w, (int)h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    HRESULT hr = oResizeBuffers(sc, n, w, h, f, fl);
    if (FAILED(hr)) return hr;
    DXGI_SWAP_CHAIN_DESC gd; sc->GetDesc(&gd);
    // drain GPU work that references the proxy swapchain / shared texture
    UINT64 v = ++*bs->fval; bs->q12->Signal(bs->fence12, v);
    if (bs->fence12->GetCompletedValue() < v) { bs->fence12->SetEventOnCompletion(v, g_evt); WaitForSingleObject(g_evt, INFINITE); }
    destroy_fg_ctx();
    destroy_share();
    g_w = gd.BufferDesc.Width; g_h = gd.BufferDesc.Height; g_fmt = gd.BufferDesc.Format;
    DXGI_SWAP_CHAIN_DESC1 d1; g_fgSc->GetDesc1(&d1);
    HRESULT h2 = g_fgSc->ResizeBuffers(d1.BufferCount, g_w, g_h, g_fmt, d1.Flags);
    logf("resize %ux%u -> proxy hr=0x%08lx", g_w, g_h, (unsigned long)h2);
    create_fg_ctx();
    g_frameID += 10;  // force FG reset
    return hr;
}

static HRESULT STDMETHODCALLTYPE hSetFS(IDXGISwapChain *sc, BOOL fs, IDXGIOutput *o) {
    if (sc != g_gameSc || !g_fgSc) return oSetFS(sc, fs, o);
    if (fs) logf("game requested exclusive fullscreen: ignored with frame generation (use Borderless)");
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE hGetFS(IDXGISwapChain *sc, BOOL *fs, IDXGIOutput **o) {
    if (sc != g_gameSc || !g_fgSc) return oGetFS(sc, fs, o);
    if (fs) *fs = FALSE; if (o) *o = 0;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE hResizeTarget(IDXGISwapChain *sc, const DXGI_MODE_DESC *m) {
    if (sc != g_gameSc || !g_fgSc) return oResizeTarget(sc, m);
    return S_OK;  // the real window is owned by the game; ignore target resizes
}

// common: decide whether to take over this swapchain; returns hidden hwnd to use instead of the real one
static HWND fg_setup(IDXGIFactory *f, IUnknown *dev, HWND hwnd, UINT w, UINT h, DXGI_FORMAT fmt) {
    ID3D11Device *d11 = 0;
    if (!g_fgCfg.enabled || g_fgSc || !dev || FAILED(dev->QueryInterface(__uuidof(ID3D11Device), (void **)&d11))) return 0;
    mod_hook_device(d11);  // the swapchain's device is the one that renders
    ID3D11DeviceContext *ctx = 0; d11->GetImmediateContext(&ctx);
    bool ok = fsr_bridge_init(d11, ctx);
    rel(ctx); rel(d11);
    if (!ok) return 0;
    bs = fsr_state();
    g_realHwnd = hwnd;
    RECT rc; GetClientRect(hwnd, &rc);
    g_w = w ? w : (UINT)(rc.right - rc.left);
    g_h = h ? h : (UINT)(rc.bottom - rc.top);
    g_fmt = fmt;
    if (g_fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) g_fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (g_fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) g_fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
    if (!create_fg_swapchain(f)) { logf("FG unavailable -> normal swapchain"); g_fgSc = 0; return 0; }
    g_hiddenHwnd = make_hidden_window(g_w, g_h);
    return g_hiddenHwnd;
}

static bool is_d3d11(IUnknown *dev) {
    ID3D11Device *d = 0;
    if (!dev || FAILED(dev->QueryInterface(__uuidof(ID3D11Device), (void **)&d))) return false;
    d->Release(); return true;
}

static void hook_game_swapchain(IDXGISwapChain *sc, DXGI_FORMAT origFmt, BOOL windowed) {
    g_gameSc = sc;
    void *o;
    if ((o = hook_vt(sc, 8, (void *)hPresent))) oPresent = (PFN_Present)o;
    if ((o = hook_vt(sc, 10, (void *)hSetFS))) oSetFS = (PFN_SetFS)o;
    if ((o = hook_vt(sc, 11, (void *)hGetFS))) oGetFS = (PFN_GetFS)o;
    if ((o = hook_vt(sc, 13, (void *)hResizeBuffers))) oResizeBuffers = (PFN_ResizeBuffers)o;
    if ((o = hook_vt(sc, 14, (void *)hResizeTarget))) oResizeTarget = (PFN_ResizeTarget)o;
    if (g_fgSc) logf("game swapchain redirected to hidden window (%ux%u fmt %d, game wanted windowed=%d)", g_w, g_h, origFmt, windowed);
    else logf("game swapchain hooked for overlay (no frame generation)");
}

// IDXGIFactory::CreateSwapChain (vtable 10)
static HRESULT STDMETHODCALLTYPE hCreateSC(IDXGIFactory *f, IUnknown *dev, DXGI_SWAP_CHAIN_DESC *desc, IDXGISwapChain **out) {
    HWND hid = desc ? fg_setup(f, dev, desc->OutputWindow, desc->BufferDesc.Width, desc->BufferDesc.Height, desc->BufferDesc.Format) : 0;
    if (!hid) {
        HRESULT hr = oCreateSC(f, dev, desc, out);
        if (SUCCEEDED(hr) && out && *out && desc && !g_gameSc && is_d3d11(dev)) {
            ID3D11Device *d = 0; dev->QueryInterface(__uuidof(ID3D11Device), (void **)&d); mod_hook_device(d); rel(d);
            g_realHwnd = desc->OutputWindow; hook_game_swapchain(*out, desc->BufferDesc.Format, desc->Windowed);
        }
        return hr;
    }
    DXGI_SWAP_CHAIN_DESC d = *desc; d.OutputWindow = hid; d.Windowed = TRUE;
    HRESULT hr = oCreateSC(f, dev, &d, out);
    if (SUCCEEDED(hr) && out && *out) hook_game_swapchain(*out, desc->BufferDesc.Format, desc->Windowed);
    return hr;
}

// IDXGIFactory2::CreateSwapChainForHwnd (vtable 15)
typedef HRESULT(STDMETHODCALLTYPE *PFN_CreateSCHwnd)(IDXGIFactory2 *, IUnknown *, HWND, const DXGI_SWAP_CHAIN_DESC1 *, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, IDXGIOutput *, IDXGISwapChain1 **);
static PFN_CreateSCHwnd oCreateSCHwnd;
static HRESULT STDMETHODCALLTYPE hCreateSCHwnd(IDXGIFactory2 *f, IUnknown *dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1 *desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs, IDXGIOutput *out0, IDXGISwapChain1 **out) {
    HWND hid = desc ? fg_setup(f, dev, hwnd, desc->Width, desc->Height, desc->Format) : 0;
    if (!hid) {
        HRESULT hr = oCreateSCHwnd(f, dev, hwnd, desc, fs, out0, out);
        if (SUCCEEDED(hr) && out && *out && desc && !g_gameSc && is_d3d11(dev)) { g_realHwnd = hwnd; hook_game_swapchain(*out, desc->Format, fs ? fs->Windowed : TRUE); }
        return hr;
    }
    HRESULT hr = oCreateSCHwnd(f, dev, hid, desc, 0, 0, out);
    if (SUCCEEDED(hr) && out && *out) hook_game_swapchain(*out, desc->Format, fs ? fs->Windowed : TRUE);
    return hr;
}

void fg_install_factory_hooks(void *factory) {
    if (!factory) return;
    void *o = hook_vt(factory, 10, (void *)hCreateSC);
    if (o && !oCreateSC) { oCreateSC = (PFN_CreateSC)o; logf("FG: factory CreateSwapChain hooked"); }
    IDXGIFactory2 *f2 = 0;
    if (SUCCEEDED(((IUnknown *)factory)->QueryInterface(__uuidof(IDXGIFactory2), (void **)&f2))) {
        o = hook_vt(f2, 15, (void *)hCreateSCHwnd);
        if (o && !oCreateSCHwnd) { oCreateSCHwnd = (PFN_CreateSCHwnd)o; logf("FG: factory CreateSwapChainForHwnd hooked"); }
        f2->Release();
    }
}
