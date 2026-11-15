// ED-FSR — Elite Dangerous: replace built-in FSR1 with AMD FSR 3.1 / FSR 4. Loaded as dinput8.dll proxy.
//
// Hook layer:
//  * IAT hook D3D11CreateDevice(AndSwapChain) in the game exe -> vtable hooks on device/context.
//  * Never hook Draw/Dispatch: the D3D11 runtime rewrites those vtable slots on the fly.
//  * CSSetShader(EASU) -> substitute a no-op CS, remember EASU's t0/u0; at the next CSSetShader
//    (RCAS) run FSR into EASU's output texture.
//  * OMSetRenderTargets/RSSetViewports -> subpixel jitter while the scene depth buffer is bound,
//    and copy the per-frame camera cbuffer (VS b1) when the scene pass ends.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <mutex>
#include <unordered_set>
#include "renderdoc_app.h"
#include "fsr.h"

// ------------------------------------------------------------------ config / log
static const char *kRoot = "D:\\ed-fsr";
static struct Config {
    int enabled = 1;
    int renderdoc = 0;
    uint64_t easu_fnv = 0xf4335f6d4ecad949ull;
    uint32_t easu_size = 38208;
} cfg;

void logf(const char *fmt, ...) {
    static std::mutex m; std::lock_guard<std::mutex> l(m);
    char path[MAX_PATH]; snprintf(path, sizeof path, "%s\\logs\\edfsr.log", kRoot);
    FILE *f = fopen(path, "a"); if (!f) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(f, "%02d:%02d:%02d.%03d [%lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentProcessId());
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a);
    fputc('\n', f); fclose(f);
}

static bool g_active = true;  // upscaler on (settings overlay / ini "upscaler")

static float ini_float(const char *ini, const char *key, float def) {
    char b[64], d[64]; snprintf(d, sizeof d, "%g", def);
    GetPrivateProfileStringA("edfsr", key, d, b, sizeof b, ini); return (float)atof(b);
}

static void load_config() {
    char ini[MAX_PATH]; snprintf(ini, sizeof ini, "%s\\edfsr.ini", kRoot);
    cfg.enabled = GetPrivateProfileIntA("edfsr", "enabled", cfg.enabled, ini);
    cfg.renderdoc = GetPrivateProfileIntA("edfsr", "renderdoc", cfg.renderdoc, ini);
    g_fsrCfg.jitter = GetPrivateProfileIntA("edfsr", "jitter", g_fsrCfg.jitter, ini);
    g_fsrCfg.mv = GetPrivateProfileIntA("edfsr", "motion_vectors", g_fsrCfg.mv, ini);
    g_fsrCfg.debugView = GetPrivateProfileIntA("edfsr", "debug_view", g_fsrCfg.debugView, ini);
    g_fsrCfg.jitterSignX = ini_float(ini, "jitter_sign_x", g_fsrCfg.jitterSignX);
    g_fsrCfg.jitterSignY = ini_float(ini, "jitter_sign_y", g_fsrCfg.jitterSignY);
    g_fsrCfg.cockpitDepth = ini_float(ini, "cockpit_depth", g_fsrCfg.cockpitDepth);
    g_fsrCfg.sharpness = ini_float(ini, "sharpness", g_fsrCfg.sharpness);
    { char u[16]; GetPrivateProfileStringA("edfsr", "upscaler", "fsr4", u, sizeof u, ini);
      if (!_stricmp(u, "game")) g_active = false; else g_fsrCfg.version = !_stricmp(u, "fsr31") ? 2 : 1; }
    g_fgCfg.enabled = GetPrivateProfileIntA("edfsr", "framegen", g_fgCfg.enabled, ini) && cfg.enabled;
    g_fgCfg.debug = GetPrivateProfileIntA("edfsr", "framegen_debug", g_fgCfg.debug, ini);
    { char e[8]; if (GetEnvironmentVariableA("EDFSR_FRAMEGEN", e, sizeof e)) g_fgCfg.enabled = atoi(e) && cfg.enabled; }
    char buf[64];
    if (GetEnvironmentVariableA("EDFSR_EASU_FNV", buf, sizeof buf)) { cfg.easu_fnv = _strtoui64(buf, 0, 0); cfg.easu_size = 0; }
}

static uint64_t fnv64(const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p; uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; }
    return h;
}

static void *hook_vt(void *obj, int idx, void *fn) {
    void **vt = *(void ***)obj;
    void *orig = vt[idx];
    if (orig == fn) return 0;
    DWORD old; VirtualProtect(&vt[idx], sizeof(void *), PAGE_READWRITE, &old);
    vt[idx] = fn; VirtualProtect(&vt[idx], sizeof(void *), old, &old);
    return orig;
}
template <class T> static void rel(T *&p) { if (p) { p->Release(); p = nullptr; } }
template <class T> static void assign(T *&dst, T *src) { if (src) src->AddRef(); if (dst) dst->Release(); dst = src; }

// ------------------------------------------------------------------ originals
typedef HRESULT(STDMETHODCALLTYPE *PFN_CreateCS)(ID3D11Device *, const void *, SIZE_T, ID3D11ClassLinkage *, ID3D11ComputeShader **);
typedef void(STDMETHODCALLTYPE *PFN_OMSetRT)(ID3D11DeviceContext *, UINT, ID3D11RenderTargetView *const *, ID3D11DepthStencilView *);
typedef void(STDMETHODCALLTYPE *PFN_OMSetRTUAV)(ID3D11DeviceContext *, UINT, ID3D11RenderTargetView *const *, ID3D11DepthStencilView *, UINT, UINT, ID3D11UnorderedAccessView *const *, const UINT *);
typedef void(STDMETHODCALLTYPE *PFN_RSSetVP)(ID3D11DeviceContext *, UINT, const D3D11_VIEWPORT *);
static PFN_CreateCS oCreateCS;
static PFN_OMSetRT oOMSetRT;
static PFN_OMSetRTUAV oOMSetRTUAV;
static PFN_RSSetVP oRSSetVP;

// ------------------------------------------------------------------ state (render thread)
static std::mutex g_mx;
static std::unordered_set<ID3D11ComputeShader *> g_easu;
static uint64_t g_frames;
static ID3D11ComputeShader *g_noop;
static bool g_noopFailed;

static bool g_pendingEASU;                 // EASU (substituted) is the current CS
static ID3D11ShaderResourceView *g_easuSrv;
static ID3D11UnorderedAccessView *g_easuUav;
static UINT g_renderW, g_renderH;          // learned from EASU input
static ID3D11Texture2D *g_sceneDepth;      // last DSV texture with render-res size
static bool g_inScene;
static D3D11_VIEWPORT g_vp[16]; static UINT g_nvp;

static bool fsr_on() { return cfg.enabled && g_active && !fsr_failed(); }

static void tex_of_view(ID3D11View *v, ID3D11Texture2D **out) {
    *out = 0; if (!v) return;
    ID3D11Resource *r = 0; v->GetResource(&r); if (!r) return;
    r->QueryInterface(__uuidof(ID3D11Texture2D), (void **)out); r->Release();
}

// ------------------------------------------------------------------ jitter via viewport offset
static void apply_viewports(ID3D11DeviceContext *c) {
    if (!g_nvp) return;
    D3D11_VIEWPORT v[16]; memcpy(v, g_vp, g_nvp * sizeof(D3D11_VIEWPORT));
    if (g_inScene && fsr_on() && g_fsrCfg.jitter) {
        float jx, jy; fsr_jitter(jx, jy);
        for (UINT i = 0; i < g_nvp; i++) { v[i].TopLeftX += jx; v[i].TopLeftY += jy; }
    }
    oRSSetVP(c, g_nvp, v);
}

static void STDMETHODCALLTYPE hRSSetVP(ID3D11DeviceContext *c, UINT n, const D3D11_VIEWPORT *v) {
    g_nvp = n > 16 ? 16 : n;
    if (v && g_nvp) memcpy(g_vp, v, g_nvp * sizeof(D3D11_VIEWPORT));
    if (g_inScene && fsr_on() && g_fsrCfg.jitter) apply_viewports(c);
    else oRSSetVP(c, n, v);
}

static void on_dsv(ID3D11DeviceContext *c, ID3D11DepthStencilView *dsv) {
    bool scene = false;
    if (dsv && g_renderW) {
        ID3D11Texture2D *t = 0; tex_of_view(dsv, &t);
        if (t) {
            D3D11_TEXTURE2D_DESC d; t->GetDesc(&d);
            if (d.Width == g_renderW && d.Height == g_renderH && d.SampleDesc.Count == 1) { scene = true; assign(g_sceneDepth, t); }
            t->Release();
        }
    }
    if (scene == g_inScene) return;
    if (g_inScene && !scene && fsr_on()) fsr_capture_frame_cb(c);  // leaving scene pass: camera cbuffer is current
    g_inScene = scene;
    if (fsr_on() && g_fsrCfg.jitter) apply_viewports(c);
}

static void STDMETHODCALLTYPE hOMSetRT(ID3D11DeviceContext *c, UINT n, ID3D11RenderTargetView *const *rtv, ID3D11DepthStencilView *dsv) {
    oOMSetRT(c, n, rtv, dsv);
    on_dsv(c, dsv);
}
static void STDMETHODCALLTYPE hOMSetRTUAV(ID3D11DeviceContext *c, UINT n, ID3D11RenderTargetView *const *rtv, ID3D11DepthStencilView *dsv,
                                          UINT us, UINT un, ID3D11UnorderedAccessView *const *uav, const UINT *ic) {
    oOMSetRTUAV(c, n, rtv, dsv, us, un, uav, ic);
    if (n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) on_dsv(c, dsv);
}

// ------------------------------------------------------------------ EASU interception
static bool init_noop(ID3D11DeviceContext *c) {
    if (g_noop) return true;
    if (g_noopFailed) return false;
    const char *src = "[numthreads(64,1,1)] void main() {}";
    ID3DBlob *b = 0, *e = 0;
    ID3D11Device *dev = 0; c->GetDevice(&dev);
    HRESULT hr = D3DCompile(src, strlen(src), "noop", 0, 0, "main", "cs_5_0", 0, 0, &b, &e);
    if (SUCCEEDED(hr)) { hr = oCreateCS(dev, b->GetBufferPointer(), b->GetBufferSize(), 0, &g_noop); b->Release(); }
    dev->Release();
    if (FAILED(hr)) { logf("noop CS failed 0x%08lx", (unsigned long)hr); g_noopFailed = true; return false; }
    return true;
}

// runtime on/off (settings overlay)
bool mod_upscaler_active() { return cfg.enabled && g_active; }
void mod_set_upscaler_active(bool on) { if (on != g_active) { g_active = on; fsr_reset(); logf("upscaler: %s", on ? "ED-FSR" : "game FSR1"); } }

static void run_fsr_after_easu(ID3D11DeviceContext *c) {
    g_pendingEASU = false;
    ID3D11Texture2D *color = 0, *out = 0;
    tex_of_view(g_easuSrv, &color); tex_of_view(g_easuUav, &out);
    { static int dn; if (dn++ < 3) logf("after EASU: srv=%p uav=%p color=%p out=%p depth=%p", (void *)g_easuSrv, (void *)g_easuUav, (void *)color, (void *)out, (void *)g_sceneDepth); }
    if (color && out) {
        FsrFrame f = {c, color, g_sceneDepth, out};
        if (!fsr_run(f)) { static int n; if (n++ < 3) logf("fsr_run returned false"); }
    }
    if (color) color->Release(); if (out) out->Release();
}

static void STDMETHODCALLTYPE hCSSetShader(ID3D11DeviceContext *c, ID3D11ComputeShader *s, ID3D11ClassInstance *const *ci, UINT n) {
    if (g_pendingEASU) run_fsr_after_easu(c);  // EASU (no-op) has been dispatched by now
    bool easu = false;
    if (s) { std::lock_guard<std::mutex> l(g_mx); easu = g_easu.count(s) != 0; }
    if (easu) {
        g_frames++;
        // learn render size from currently bound input (also re-captured by the SRV/UAV hooks)
        ID3D11ShaderResourceView *sv = 0; c->CSGetShaderResources(0, 1, &sv);
        ID3D11UnorderedAccessView *uv = 0; c->CSGetUnorderedAccessViews(0, 1, &uv);
        if (sv) assign(g_easuSrv, sv); if (uv) assign(g_easuUav, uv); rel(sv); rel(uv);
        ID3D11Texture2D *t = 0; tex_of_view(g_easuSrv, &t);
        if (t) { D3D11_TEXTURE2D_DESC d; t->GetDesc(&d); g_renderW = d.Width; g_renderH = d.Height; t->Release(); }
        if (fsr_on() && init_noop(c)) {
            if (g_frames == 1) logf("EASU intercepted (render %ux%u)", g_renderW, g_renderH);
            g_pendingEASU = true;
            g_o.CSSetShader(c, g_noop, 0, 0);
            return;
        }
    }
    g_o.CSSetShader(c, s, ci, n);
}

static void STDMETHODCALLTYPE hCSSetSRV(ID3D11DeviceContext *c, UINT start, UINT n, ID3D11ShaderResourceView *const *v) {
    if (g_pendingEASU && v && start == 0 && n >= 1 && v[0]) assign(g_easuSrv, v[0]);
    g_o.CSSetShaderResources(c, start, n, v);
}
static void STDMETHODCALLTYPE hCSSetUAV(ID3D11DeviceContext *c, UINT start, UINT n, ID3D11UnorderedAccessView *const *v, const UINT *ic) {
    if (g_pendingEASU && v && start == 0 && n >= 1 && v[0]) assign(g_easuUav, v[0]);
    g_o.CSSetUnorderedAccessViews(c, start, n, v, ic);
}

static HRESULT STDMETHODCALLTYPE hCreateCS(ID3D11Device *d, const void *bc, SIZE_T n, ID3D11ClassLinkage *cl, ID3D11ComputeShader **out) {
    HRESULT hr = oCreateCS(d, bc, n, cl, out);
    if (cfg.easu_size && n == cfg.easu_size) logf("CS of EASU size created: fnv=0x%016llx", (unsigned long long)fnv64(bc, n));
    if (SUCCEEDED(hr) && out && *out && (cfg.easu_fnv == 0 || ((!cfg.easu_size || n == cfg.easu_size) && fnv64(bc, n) == cfg.easu_fnv))) {
        std::lock_guard<std::mutex> l(g_mx); g_easu.insert(*out);
        logf("EASU shader detected: %p (%zu bytes)", (void *)*out, (size_t)n);
    }
    return hr;
}

// vtable indices (d3d11.h order)
enum { DEV_CreateComputeShader = 18,
       CTX_OMSetRenderTargets = 33, CTX_OMSetRenderTargetsAndUAVs = 34, CTX_RSSetViewports = 44,
       CTX_CSSetShaderResources = 67, CTX_CSSetUnorderedAccessViews = 68, CTX_CSSetShader = 69 };

static void install_device_hooks(ID3D11Device *dev, ID3D11DeviceContext *ctx) {
    static bool done; if (done || !dev) return;
    ID3D11DeviceContext *ic = ctx; if (!ic) dev->GetImmediateContext(&ic); else ic->AddRef();
    void *o;
    if ((o = hook_vt(dev, DEV_CreateComputeShader, (void *)hCreateCS))) oCreateCS = (PFN_CreateCS)o;
    if ((o = hook_vt(ic, CTX_CSSetShader, (void *)hCSSetShader))) g_o.CSSetShader = (decltype(g_o.CSSetShader))o;
    if ((o = hook_vt(ic, CTX_CSSetShaderResources, (void *)hCSSetSRV))) g_o.CSSetShaderResources = (decltype(g_o.CSSetShaderResources))o;
    if ((o = hook_vt(ic, CTX_CSSetUnorderedAccessViews, (void *)hCSSetUAV))) g_o.CSSetUnorderedAccessViews = (decltype(g_o.CSSetUnorderedAccessViews))o;
    if ((o = hook_vt(ic, CTX_OMSetRenderTargets, (void *)hOMSetRT))) oOMSetRT = (PFN_OMSetRT)o;
    if ((o = hook_vt(ic, CTX_OMSetRenderTargetsAndUAVs, (void *)hOMSetRTUAV))) oOMSetRTUAV = (PFN_OMSetRTUAV)o;
    if ((o = hook_vt(ic, CTX_RSSetViewports, (void *)hRSSetVP))) oRSSetVP = (PFN_RSSetVP)o;
    ic->Release();
    done = oCreateCS && g_o.CSSetShader && g_o.CSSetShaderResources && g_o.CSSetUnorderedAccessViews && oOMSetRT && oOMSetRTUAV && oRSSetVP;
    logf("device hooks installed: %s", done ? "ok" : "FAILED");
}

typedef HRESULT(WINAPI *PFN_CreateDev)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
typedef HRESULT(WINAPI *PFN_CreateDevSC)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT, const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
static PFN_CreateDev oCreateDev; static PFN_CreateDevSC oCreateDevSC;
typedef HRESULT(WINAPI *PFN_CreateFactory)(REFIID, void **);
static PFN_CreateFactory oCreateFactory1, oCreateFactory;
static HRESULT WINAPI hCreateFactory1(REFIID r, void **f) { HRESULT hr = oCreateFactory1(r, f); if (SUCCEEDED(hr) && f && *f) fg_install_factory_hooks(*f); return hr; }
static HRESULT WINAPI hCreateFactory(REFIID r, void **f) { HRESULT hr = oCreateFactory(r, f); if (SUCCEEDED(hr) && f && *f) fg_install_factory_hooks(*f); return hr; }

static HRESULT WINAPI hCreateDev(IDXGIAdapter *a, D3D_DRIVER_TYPE t, HMODULE s, UINT f, const D3D_FEATURE_LEVEL *fl, UINT n, UINT v, ID3D11Device **d, D3D_FEATURE_LEVEL *ofl, ID3D11DeviceContext **c) {
    HRESULT hr = oCreateDev(a, t, s, f, fl, n, v, d, ofl, c);
    if (SUCCEEDED(hr) && d && *d) install_device_hooks(*d, c ? *c : 0);
    return hr;
}
static HRESULT WINAPI hCreateDevSC(IDXGIAdapter *a, D3D_DRIVER_TYPE t, HMODULE s, UINT f, const D3D_FEATURE_LEVEL *fl, UINT n, UINT v, const DXGI_SWAP_CHAIN_DESC *sd, IDXGISwapChain **sc, ID3D11Device **d, D3D_FEATURE_LEVEL *ofl, ID3D11DeviceContext **c) {
    HRESULT hr = oCreateDevSC(a, t, s, f, fl, n, v, sd, sc, d, ofl, c);
    if (SUCCEEDED(hr) && d && *d) install_device_hooks(*d, c ? *c : 0);
    return hr;
}

// ------------------------------------------------------------------ IAT patching
struct IatHook { const char *dll, *fn; void *rep; void **orig; };
static int patch_iat(HMODULE mod, const IatHook *hs, int nh) {
    BYTE *b = (BYTE *)mod; int n = 0;
    auto *nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    for (auto *d = (IMAGE_IMPORT_DESCRIPTOR *)(b + dir.VirtualAddress); d->Name; d++) {
        const char *dll = (const char *)(b + d->Name);
        if (!d->OriginalFirstThunk) continue;
        auto *on = (IMAGE_THUNK_DATA *)(b + d->OriginalFirstThunk), *ft = (IMAGE_THUNK_DATA *)(b + d->FirstThunk);
        for (; on->u1.AddressOfData; on++, ft++) {
            if (IMAGE_SNAP_BY_ORDINAL(on->u1.Ordinal)) continue;
            const char *fn = (const char *)((IMAGE_IMPORT_BY_NAME *)(b + on->u1.AddressOfData))->Name;
            for (int i = 0; i < nh; i++) {
                if (_stricmp(dll, hs[i].dll) || strcmp(fn, hs[i].fn)) continue;
                *hs[i].orig = (void *)ft->u1.Function;
                DWORD old; VirtualProtect(&ft->u1.Function, 8, PAGE_READWRITE, &old);
                ft->u1.Function = (ULONG_PTR)hs[i].rep; VirtualProtect(&ft->u1.Function, 8, old, &old); n++;
            }
        }
    }
    return n;
}

// Epic overlay (EOSOVH) crashes together with RenderDoc — blocked only when RenderDoc is loaded.
typedef HMODULE(WINAPI *PFN_LLA)(LPCSTR); typedef HMODULE(WINAPI *PFN_LLW)(LPCWSTR);
static PFN_LLA oLLA; static PFN_LLW oLLW;
static HMODULE WINAPI hLLA(LPCSTR n) { if (n && strstr(n, "EOSOVH")) { logf("blocked EOS overlay"); SetLastError(ERROR_MOD_NOT_FOUND); return 0; } return oLLA(n); }
static HMODULE WINAPI hLLW(LPCWSTR n) { if (n && wcsstr(n, L"EOSOVH")) { logf("blocked EOS overlay"); SetLastError(ERROR_MOD_NOT_FOUND); return 0; } return oLLW(n); }

static void load_renderdoc() {
    HMODULE m = LoadLibraryW(L"D:\\tools\\renderdoc\\renderdoc.dll");
    if (!m) { logf("renderdoc.dll load FAILED"); return; }
    auto g = (pRENDERDOC_GetAPI)GetProcAddress(m, "RENDERDOC_GetAPI");
    RENDERDOC_API_1_1_2 *api = 0;
    if (g && g(eRENDERDOC_API_Version_1_1_2, (void **)&api) && api) {
        api->SetCaptureFilePathTemplate("D:\\ed-fsr\\captures\\ed");
        RENDERDOC_InputButton keys[] = {eRENDERDOC_Key_F12, eRENDERDOC_Key_PrtScrn};
        api->SetCaptureKeys(keys, 2);
        logf("renderdoc loaded");
    }
}

static void startup() {
    char exe[MAX_PATH]; GetModuleFileNameA(0, exe, MAX_PATH);
    if (!strstr(exe, "EliteDangerous64.exe") && !strstr(exe, "dx11test.exe")) return;
    load_config();
    logf("---- ED-FSR start in %s (enabled=%d renderdoc=%d jitter=%d mv=%d framegen=%d)", exe, cfg.enabled, cfg.renderdoc, g_fsrCfg.jitter, g_fsrCfg.mv, g_fgCfg.enabled);
    if (cfg.renderdoc) load_renderdoc();
    HMODULE eos = GetModuleHandleA("EOSSDK-Win64-Shipping.dll");
    if (eos && cfg.renderdoc) {
        IatHook h[] = {{"KERNEL32.dll", "LoadLibraryA", (void *)hLLA, (void **)&oLLA}, {"KERNEL32.dll", "LoadLibraryW", (void *)hLLW, (void **)&oLLW}};
        logf("EOS overlay blocker: %d imports patched", patch_iat(eos, h, 2));
    }
    IatHook d[] = {{"d3d11.dll", "D3D11CreateDevice", (void *)hCreateDev, (void **)&oCreateDev},
                   {"d3d11.dll", "D3D11CreateDeviceAndSwapChain", (void *)hCreateDevSC, (void **)&oCreateDevSC}};
    logf("d3d11 IAT hooks: %d", patch_iat(GetModuleHandleA(0), d, 2));
    {
        IatHook x[] = {{"dxgi.dll", "CreateDXGIFactory1", (void *)hCreateFactory1, (void **)&oCreateFactory1},
                       {"dxgi.dll", "CreateDXGIFactory", (void *)hCreateFactory, (void **)&oCreateFactory}};
        logf("dxgi IAT hooks: %d (framegen=%d)", patch_iat(GetModuleHandleA(0), x, 2), g_fgCfg.enabled);
    }
}

// ------------------------------------------------------------------ dinput8 proxy
static HMODULE g_real;
static FARPROC real_fn(const char *n) {
    if (!g_real) { wchar_t p[MAX_PATH]; GetSystemDirectoryW(p, MAX_PATH); wcscat(p, L"\\dinput8.dll"); g_real = LoadLibraryW(p); }
    return g_real ? GetProcAddress(g_real, n) : 0;
}
extern "C" {
HRESULT WINAPI DirectInput8Create(HINSTANCE a, DWORD b, REFIID c, LPVOID *d, void *e) {
    auto f = (HRESULT(WINAPI *)(HINSTANCE, DWORD, REFIID, LPVOID *, void *))real_fn("DirectInput8Create");
    HRESULT hr = f ? f(a, b, c, d, e) : E_FAIL;
    if (SUCCEEDED(hr) && d && *d) fg_on_dinput_created(*d);
    return hr;
}
HRESULT WINAPI DllCanUnloadNow(void) { auto f = (HRESULT(WINAPI *)())real_fn("DllCanUnloadNow"); return f ? f() : S_FALSE; }
HRESULT WINAPI DllRegisterServer(void) { auto f = (HRESULT(WINAPI *)())real_fn("DllRegisterServer"); return f ? f() : E_FAIL; }
HRESULT WINAPI DllUnregisterServer(void) { auto f = (HRESULT(WINAPI *)())real_fn("DllUnregisterServer"); return f ? f() : E_FAIL; }
HRESULT WINAPI DllGetClassObject(REFCLSID a, REFIID b, LPVOID *c) { auto f = (HRESULT(WINAPI *)(REFCLSID, REFIID, LPVOID *))real_fn("DllGetClassObject"); return f ? f(a, b, c) : E_FAIL; }
LPCVOID WINAPI GetdfDIJoystick(void) { auto f = (LPCVOID(WINAPI *)())real_fn("GetdfDIJoystick"); return f ? f() : 0; }

BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID) {
    if (r == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); startup(); }
    return TRUE;
}
}
