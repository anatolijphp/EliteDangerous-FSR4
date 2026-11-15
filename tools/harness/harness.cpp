// ED-FSR offline test harness: mimics the end of Elite Dangerous' D3D11 frame so the
// dinput8.dll mod can be exercised without the game.
//
//   scene (render res, RGBA16F + D32S8 typeless depth, reversed-Z infinite, VS b1 = 4480-byte frame cbuffer)
//   -> tonemap PS -> RGBA8 (render res)
//   -> "EASU" CS [64,1,1], 16x16 tile per group, t0/s0/u0/b2 -> display res RGBA8
//   -> "RCAS" CS (different bytecode) -> display res RGBA8
//   -> PS copy to backbuffer -> Present
//
// Usage: dx11test.exe [frames=300] [--size WxH] [--scale 0.67] [--save TAG] [--vsync] [--sep] [--realtime] [--no-unbind]
//                     [--edswap] [--show] [--resize-test]
//   --edswap      ED swapchain path: D3D11CreateDevice, CreateDXGIFactory1 (static import), IDXGIFactory::CreateSwapChain
//   --show        resizable window; WM_SIZE -> ResizeBuffers(0,w,h,UNKNOWN,0); frames=0 runs until closed; 1/s frame stats
//   --resize-test implies --show; resizes client to 1600x900 at frame 100 and back at frame 200
//   EDFSR_PROXY=<path to dinput8.dll>  is LoadLibrary'd before device creation.
// The executable MUST be named dx11test.exe (the mod only activates for EliteDangerous64.exe / dx11test.exe).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("FAILED 0x%08lx: %s (line %d)\n", (unsigned long)hr_, #x, __LINE__); exit(2); } } while (0)

static uint64_t fnv64(const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p; uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; }
    return h;
}

// ------------------------------------------------------------------ shaders
static const char *kSceneHLSL = R"(
cbuffer Frame : register(b1) {            // 4480 bytes, mirrors ED's per-frame cbuffer
    row_major float4x4 viewProj : packoffset(c270);   // byte 4320, camera-relative, row-vector convention
    float3 camPos              : packoffset(c275);   // byte 4400
};
cbuffer Obj : register(b0) { float4 params; };     // x = mode (0 plane, 1 cubes), y = time
struct VSO { float4 pos : SV_Position; float3 wp : WP; float3 n : NRM; float2 uv : UV; nointerpolation float mode : MODE; };

static const float2 kQ[6] = { float2(0,0), float2(1,0), float2(0,1), float2(0,1), float2(1,0), float2(1,1) };
static const float3 kN[6] = { float3(1,0,0), float3(-1,0,0), float3(0,1,0), float3(0,-1,0), float3(0,0,1), float3(0,0,-1) };
static const float3 kT[6] = { float3(0,1,0), float3(0,0,1), float3(1,0,0), float3(0,0,1), float3(1,0,0), float3(0,1,0) };
static const float3 kB[6] = { float3(0,0,1), float3(0,1,0), float3(0,0,1), float3(1,0,0), float3(0,1,0), float3(1,0,0) };

float3x3 rotY(float a) { float s = sin(a), c = cos(a); return float3x3(c,0,-s, 0,1,0, s,0,c); }
float3x3 rotX(float a) { float s = sin(a), c = cos(a); return float3x3(1,0,0, 0,c,s, 0,-s,c); }

VSO vs_main(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    VSO o; float t = params.y; float2 q = kQ[vid % 6];
    float3 wp, n;
    if (params.x < 0.5) {                    // ground plane 200x200 at y = -1.5
        wp = float3((q.x * 2 - 1) * 100, -1.5, (q.y * 2 - 1) * 100); n = float3(0,1,0); o.uv = wp.xz;
    } else {                                 // ring of rotating cubes
        uint f = (vid / 6) % 6;
        float3 lp = kN[f] + (q.x * 2 - 1) * kT[f] + (q.y * 2 - 1) * kB[f];
        float3x3 R = mul(rotX(t * (0.37 + 0.11 * iid)), rotY(t * (0.53 + 0.07 * iid) + iid));
        float ring = 6.2831853 * iid / 7.0 + t * 0.15;
        float3 c = iid == 0 ? float3(0, 0.3, 0) : float3(cos(ring) * 5.0, 0.4 * sin(t + iid), sin(ring) * 5.0);
        float s = iid == 0 ? 1.2 : 0.7;
        wp = mul(lp * s, R) + c; n = mul(kN[f], R); o.uv = q;
    }
    o.wp = wp; o.n = n; o.mode = params.x;
    o.pos = mul(float4(wp - camPos, 1), viewProj);   // camera-relative position * viewProj
    return o;
}

float4 ps_main(VSO i) : SV_Target {
    float3 col;
    if (i.mode < 0.5) {
        float2 g = i.uv;
        float2 fw = fwidth(g);
        float2 d = abs(frac(g + 0.5) - 0.5);               // distance to integer grid lines
        float line1 = any(d < 0.015) ? 1 : 0;               // thin (aliasing-prone) lines, 3 cm wide
        float2 d10 = abs(frac(g / 10 + 0.5) - 0.5) * 10;
        float line10 = any(d10 < 0.06) ? 1 : 0;
        float chk = (floor(g.x) + floor(g.y)) % 2 == 0 ? 1 : 0;
        col = lerp(float3(0.05,0.06,0.08), float3(0.12,0.12,0.14), chk) + line1 * float3(0.2,0.9,1.5) + line10 * float3(3.0,1.2,0.3);
    } else {
        float2 u = i.uv * 8;
        float chk = (floor(u.x) + floor(u.y)) % 2 == 0 ? 1 : 0;
        float stripes = frac(i.uv.x * 40 + i.uv.y * 13) < 0.25 ? 1 : 0;   // high-frequency diagonal stripes
        float3 base = lerp(float3(0.9,0.15,0.1), float3(0.95,0.95,0.9), chk);
        float lam = saturate(dot(normalize(i.n), normalize(float3(0.4, 0.8, -0.3)))) * 0.85 + 0.15;
        col = base * lam + stripes * float3(0.0, 0.6, 2.5) * (1 - chk);
    }
    return float4(col, 1);
}
)";

static const char *kFullscreenHLSL = R"(
Texture2D<float4> src : register(t0);
float4 vs_fs(uint id : SV_VertexID) : SV_Position { float2 p = float2((id << 1) & 2, id & 2); return float4(p * float2(2,-2) + float2(-1,1), 0, 1); }
float4 ps_tonemap(float4 p : SV_Position) : SV_Target {
    float3 c = src.Load(int3(p.xy, 0)).rgb;
    c = c / (1 + c);                        // Reinhard
    return float4(pow(saturate(c), 1 / 2.2), 1);
}
float4 ps_copy(float4 p : SV_Position) : SV_Target { return src.Load(int3(p.xy, 0)); }
)";

static const char *kEasuHLSL = R"(
// EASU stand-in: plain bilinear. Group = 16x16 output tile, 64 threads x 4 pixels.
Texture2D<float4> src : register(t0);
SamplerState lin : register(s0);
RWTexture2D<float4> dst : register(u0);
cbuffer c_ffxConst : register(b2) { float4 con0; float4 con1; };   // con0 = inSize.xy, outSize.xy
[numthreads(64,1,1)] void main(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex) {
    uint2 base = gid.xy * 16 + uint2(gi % 8, gi / 8);
    [unroll] for (uint k = 0; k < 4; k++) {
        uint2 p = base + uint2((k & 1) * 8, (k >> 1) * 8);
        if (p.x >= (uint)con0.z || p.y >= (uint)con0.w) continue;
        dst[p] = src.SampleLevel(lin, (p + 0.5) / con0.zw, 0);
    }
}
)";

static const char *kRcasHLSL = R"(
// RCAS stand-in: 5-tap unsharp mask, same tiling as EASU.
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
cbuffer c_ffxConst : register(b2) { float4 con0; float4 con1; };   // con1.x = sharpness
[numthreads(64,1,1)] void main(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex) {
    uint2 base = gid.xy * 16 + uint2(gi % 8, gi / 8);
    int2 mx = int2(con0.zw) - 1;
    [unroll] for (uint k = 0; k < 4; k++) {
        int2 p = int2(base + uint2((k & 1) * 8, (k >> 1) * 8));
        if (p.x > mx.x || p.y > mx.y) continue;
        float3 c = src.Load(int3(p, 0)).rgb;
        float3 n = src.Load(int3(clamp(p + int2(0,-1), 0, mx), 0)).rgb + src.Load(int3(clamp(p + int2(0,1), 0, mx), 0)).rgb
                 + src.Load(int3(clamp(p + int2(-1,0), 0, mx), 0)).rgb + src.Load(int3(clamp(p + int2(1,0), 0, mx), 0)).rgb;
        dst[p] = float4(saturate(c + con1.x * (4 * c - n)), 1);
    }
}
)";

static ID3DBlob *compile(const char *src, const char *entry, const char *tgt) {
    ID3DBlob *b = 0, *e = 0;
    if (FAILED(D3DCompile(src, strlen(src), entry, 0, 0, entry, tgt, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b, &e))) {
        printf("compile %s/%s failed: %s\n", entry, tgt, e ? (char *)e->GetBufferPointer() : "?"); exit(3);
    }
    return b;
}

// ------------------------------------------------------------------ math (row-vector convention: v' = v * M)
struct M4 { float m[4][4]; };
static M4 mul(const M4 &a, const M4 &b) {
    M4 r = {};
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) for (int k = 0; k < 4; k++) r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}
static void norm3(float *v) { float l = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]); v[0] /= l; v[1] /= l; v[2] /= l; }
static void cross3(const float *a, const float *b, float *r) { r[0] = a[1]*b[2]-a[2]*b[1]; r[1] = a[2]*b[0]-a[0]*b[2]; r[2] = a[0]*b[1]-a[1]*b[0]; }

// ------------------------------------------------------------------ BMP
static bool write_bmp(const char *path, const uint8_t *rgba, int w, int h, int pitch) {
    FILE *f = fopen(path, "wb"); if (!f) return false;
    int row = (w * 3 + 3) & ~3, size = row * h;
    uint8_t hdr[54] = {'B', 'M'};
    *(uint32_t *)(hdr + 2) = 54 + size; *(uint32_t *)(hdr + 10) = 54; *(uint32_t *)(hdr + 14) = 40;
    *(int32_t *)(hdr + 18) = w; *(int32_t *)(hdr + 22) = h; *(uint16_t *)(hdr + 26) = 1; *(uint16_t *)(hdr + 28) = 24;
    *(uint32_t *)(hdr + 34) = size;
    fwrite(hdr, 1, 54, f);
    uint8_t *line = (uint8_t *)calloc(1, row);
    for (int y = h - 1; y >= 0; y--) {
        const uint8_t *s = rgba + (size_t)y * pitch;
        for (int x = 0; x < w; x++) { line[x*3] = s[x*4+2]; line[x*3+1] = s[x*4+1]; line[x*3+2] = s[x*4]; }
        fwrite(line, 1, row, f);
    }
    free(line); fclose(f); return true;
}

// ------------------------------------------------------------------ device / swapchain / size-dependent state
static ID3D11Device *dev = 0; static ID3D11DeviceContext *ctx = 0; static IDXGISwapChain *sc = 0;
static float g_scale = 0.67f;
static bool g_resizable = false;     // --show / --resize-test: display size follows the window (WM_SIZE -> ResizeBuffers)
static int g_resizes = 0;

struct Sized {   // everything that depends on display / render size
    int W = 0, H = 0, RW = 0, RH = 0;
    ID3D11Texture2D *hdr = 0, *depth = 0, *ldr = 0, *up = 0, *sharp = 0;
    ID3D11RenderTargetView *hdrRTV = 0, *ldrRTV = 0; ID3D11ShaderResourceView *hdrSRV = 0, *ldrSRV = 0, *upSRV = 0, *sharpSRV = 0, *depthSRV = 0;
    ID3D11UnorderedAccessView *upUAV = 0, *sharpUAV = 0; ID3D11DepthStencilView *dsv = 0; ID3D11Buffer *cbFfx = 0;
    ID3D11Texture2D *bb = 0; ID3D11RenderTargetView *bbRTV = 0;    // backbuffer refs (must be released before ResizeBuffers)
} S;

template <class T> static void rel(T *&p) { if (p) { p->Release(); p = 0; } }

static ID3D11Texture2D *mktex(int w, int h, DXGI_FORMAT f, UINT bind) {
    D3D11_TEXTURE2D_DESC d = {}; d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = f; d.SampleDesc.Count = 1; d.BindFlags = bind; ID3D11Texture2D *t; CHECK(dev->CreateTexture2D(&d, 0, &t)); return t;
}

static void create_sized(int W, int H) {
    S.W = W; S.H = H; S.RW = (int)(W * g_scale); S.RH = (int)(H * g_scale);
    if (S.RW < 1) S.RW = 1; if (S.RH < 1) S.RH = 1;
    const int RW = S.RW, RH = S.RH;
    S.hdr = mktex(RW, RH, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    S.depth = mktex(RW, RH, DXGI_FORMAT_R32G8X24_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE);
    S.ldr = mktex(RW, RH, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    S.up = mktex(W, H, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
    S.sharp = mktex(W, H, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
    D3D11_RENDER_TARGET_VIEW_DESC rvd = {}; rvd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC svd = {}; svd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; svd.Texture2D.MipLevels = 1;
    D3D11_UNORDERED_ACCESS_VIEW_DESC uvd = {}; uvd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; uvd.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    D3D11_DEPTH_STENCIL_VIEW_DESC dvd = {}; dvd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; dvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    CHECK(dev->CreateRenderTargetView(S.hdr, 0, &S.hdrRTV)); CHECK(dev->CreateShaderResourceView(S.hdr, 0, &S.hdrSRV));
    CHECK(dev->CreateRenderTargetView(S.ldr, &rvd, &S.ldrRTV)); CHECK(dev->CreateShaderResourceView(S.ldr, &svd, &S.ldrSRV));
    CHECK(dev->CreateUnorderedAccessView(S.up, &uvd, &S.upUAV)); CHECK(dev->CreateShaderResourceView(S.up, &svd, &S.upSRV));
    CHECK(dev->CreateUnorderedAccessView(S.sharp, &uvd, &S.sharpUAV)); CHECK(dev->CreateShaderResourceView(S.sharp, &svd, &S.sharpSRV));
    CHECK(dev->CreateDepthStencilView(S.depth, &dvd, &S.dsv));
    svd.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; CHECK(dev->CreateShaderResourceView(S.depth, &svd, &S.depthSRV));
    D3D11_BUFFER_DESC bd = {}; bd.ByteWidth = 32; bd.Usage = D3D11_USAGE_IMMUTABLE; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    float ffx[8] = {(float)RW, (float)RH, (float)W, (float)H, 0.25f, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA init = {ffx, 0, 0}; CHECK(dev->CreateBuffer(&bd, &init, &S.cbFfx));
}
static void release_sized() {
    rel(S.hdrRTV); rel(S.ldrRTV); rel(S.hdrSRV); rel(S.ldrSRV); rel(S.upSRV); rel(S.sharpSRV); rel(S.depthSRV);
    rel(S.upUAV); rel(S.sharpUAV); rel(S.dsv); rel(S.cbFfx);
    rel(S.hdr); rel(S.depth); rel(S.ldr); rel(S.up); rel(S.sharp);
}
static void create_bb() { CHECK(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&S.bb)); CHECK(dev->CreateRenderTargetView(S.bb, 0, &S.bbRTV)); }
static void release_bb() { rel(S.bbRTV); rel(S.bb); }

// WM_SIZE handler body (ED does the same: release backbuffer refs, ResizeBuffers(0, w, h, UNKNOWN, 0), recreate)
static void on_resize(int w, int h) {
    if (!sc || !g_resizable || w <= 0 || h <= 0 || (w == S.W && h == S.H)) return;
    ctx->ClearState();                // every binding is re-set each frame
    release_bb(); release_sized();
    ctx->Flush();                     // let deferred destruction of the backbuffer views happen
    HRESULT hr = sc->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) { printf("ResizeBuffers(%d,%d) failed 0x%08lx\n", w, h, (unsigned long)hr); exit(7); }
    create_bb(); create_sized(w, h); g_resizes++;
    DXGI_SWAP_CHAIN_DESC d; sc->GetDesc(&d);
    printf("WM_SIZE -> ResizeBuffers ok: display %dx%d render %dx%d (swapchain %ux%u)\n", S.W, S.H, S.RW, S.RH, d.BufferDesc.Width, d.BufferDesc.Height);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_SIZE: if (w != SIZE_MINIMIZED) on_resize(LOWORD(l), HIWORD(l)); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static void set_client_size(HWND hwnd, int w, int h) {
    RECT r = {0, 0, w, h}; AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    SetWindowPos(hwnd, 0, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);   // sends WM_SIZE synchronously
}

int main(int argc, char **argv) {
    int frames = 300, W = 2560, H = 1440; const char *tag = 0;
    bool vsync = false, sep = false, realtime = false, unbind = true, edswap = false, show = false, resizeTest = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &W, &H);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) g_scale = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--save") && i + 1 < argc) tag = argv[++i];
        else if (!strcmp(argv[i], "--vsync")) vsync = true;
        else if (!strcmp(argv[i], "--sep")) sep = true;
        else if (!strcmp(argv[i], "--realtime")) realtime = true;
        else if (!strcmp(argv[i], "--no-unbind")) unbind = false;   // keep CS t0/u0 bound after each Dispatch (runtime auto-unbinds hazards)
        else if (!strcmp(argv[i], "--edswap")) edswap = true;        // ED path: D3D11CreateDevice + CreateDXGIFactory1 + IDXGIFactory::CreateSwapChain
        else if (!strcmp(argv[i], "--show")) show = true;            // resizable window, runs until closed or N frames (0 = until closed)
        else if (!strcmp(argv[i], "--resize-test")) { resizeTest = true; show = true; }
        else if (argv[i][0] >= '0' && argv[i][0] <= '9') frames = atoi(argv[i]);
        else { printf("usage: dx11test.exe [frames] [--size WxH] [--scale 0.67] [--save TAG] [--vsync] [--sep] [--realtime] [--no-unbind] [--edswap] [--show] [--resize-test]\n"); return 1; }
    }
    if (edswap && sep) { printf("--edswap and --sep are exclusive\n"); return 1; }
    g_resizable = show;
    setvbuf(stdout, 0, _IONBF, 0);

    // mod must be loaded before device creation (it IAT-hooks D3D11CreateDevice* / CreateDXGIFactory1 of this exe)
    const char *proxy = getenv("EDFSR_PROXY");
    if (proxy && *proxy) {
        HMODULE pm = LoadLibraryA(proxy);
        printf("proxy %s -> %p (err %lu)\n", proxy, (void *)pm, pm ? 0 : GetLastError());
        if (!pm) return 4;
    }

    // compile shaders first so the hash is printed even if device creation fails
    ID3DBlob *bVS = compile(kSceneHLSL, "vs_main", "vs_5_0"), *bPS = compile(kSceneHLSL, "ps_main", "ps_5_0");
    ID3DBlob *bFS = compile(kFullscreenHLSL, "vs_fs", "vs_5_0"), *bTM = compile(kFullscreenHLSL, "ps_tonemap", "ps_5_0");
    ID3DBlob *bCP = compile(kFullscreenHLSL, "ps_copy", "ps_5_0");
    ID3DBlob *bEA = compile(kEasuHLSL, "main", "cs_5_0"), *bRC = compile(kRcasHLSL, "main", "cs_5_0");
    printf("EASU_CS fnv=0x%016llx size=%zu\n", (unsigned long long)fnv64(bEA->GetBufferPointer(), bEA->GetBufferSize()), bEA->GetBufferSize());
    printf("RCAS_CS fnv=0x%016llx size=%zu\n", (unsigned long long)fnv64(bRC->GetBufferPointer(), bRC->GetBufferSize()), bRC->GetBufferSize());
    printf("display %dx%d render %dx%d frames %d%s%s%s\n", W, H, (int)(W * g_scale), (int)(H * g_scale), frames,
           edswap ? " edswap" : sep ? " sep" : "", show ? " show" : "", resizeTest ? " resize-test" : "");

    WNDCLASSA wc = {}; wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleA(0); wc.lpszClassName = "edfsr_harness";
    wc.hCursor = LoadCursor(0, IDC_ARROW); RegisterClassA(&wc);
    RECT rc = {0, 0, W, H}; AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowA("edfsr_harness", "ED-FSR harness", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0,
                              rc.right - rc.left, rc.bottom - rc.top, 0, 0, wc.hInstance, 0);

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2; sd.BufferDesc.Width = W; sd.BufferDesc.Height = H; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (edswap) {
        // exactly ED's sequence: device first, then a fresh DXGI 1.1 factory (static import -> exe IAT), classic CreateSwapChain
        CHECK(D3D11CreateDevice(0, D3D_DRIVER_TYPE_HARDWARE, 0, 0, &fl, 1, D3D11_SDK_VERSION, &dev, 0, &ctx));
        IDXGIFactory1 *fac = 0;
        CHECK(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&fac));
        CHECK(fac->CreateSwapChain(dev, &sd, &sc));
        printf("edswap: D3D11CreateDevice -> CreateDXGIFactory1 -> IDXGIFactory::CreateSwapChain ok (factory %p swapchain %p)\n", (void *)fac, (void *)sc);
        fac->Release();
    } else if (sep) {
        CHECK(D3D11CreateDevice(0, D3D_DRIVER_TYPE_HARDWARE, 0, 0, &fl, 1, D3D11_SDK_VERSION, &dev, 0, &ctx));
        IDXGIDevice *dd; IDXGIAdapter *ad; IDXGIFactory *fac;
        CHECK(dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dd)); CHECK(dd->GetAdapter(&ad));
        CHECK(ad->GetParent(__uuidof(IDXGIFactory), (void **)&fac)); CHECK(fac->CreateSwapChain(dev, &sd, &sc));
    } else {
        CHECK(D3D11CreateDeviceAndSwapChain(0, D3D_DRIVER_TYPE_HARDWARE, 0, 0, &fl, 1, D3D11_SDK_VERSION, &sd, &sc, &dev, 0, &ctx));
    }

    // ---- resources
    create_sized(W, H); create_bb();
    if (show) {   // display size follows the actual client area (window may have been clamped)
        RECT cr; GetClientRect(hwnd, &cr); on_resize(cr.right - cr.left, cr.bottom - cr.top);
    }

    ID3D11VertexShader *vsScene, *vsFS; ID3D11PixelShader *psScene, *psTM, *psCopy; ID3D11ComputeShader *csEasu, *csRcas;
    CHECK(dev->CreateVertexShader(bVS->GetBufferPointer(), bVS->GetBufferSize(), 0, &vsScene));
    CHECK(dev->CreateVertexShader(bFS->GetBufferPointer(), bFS->GetBufferSize(), 0, &vsFS));
    CHECK(dev->CreatePixelShader(bPS->GetBufferPointer(), bPS->GetBufferSize(), 0, &psScene));
    CHECK(dev->CreatePixelShader(bTM->GetBufferPointer(), bTM->GetBufferSize(), 0, &psTM));
    CHECK(dev->CreatePixelShader(bCP->GetBufferPointer(), bCP->GetBufferSize(), 0, &psCopy));
    CHECK(dev->CreateComputeShader(bEA->GetBufferPointer(), bEA->GetBufferSize(), 0, &csEasu));
    CHECK(dev->CreateComputeShader(bRC->GetBufferPointer(), bRC->GetBufferSize(), 0, &csRcas));

    D3D11_BUFFER_DESC bd = {}; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.ByteWidth = 4480; ID3D11Buffer *cbFrame; CHECK(dev->CreateBuffer(&bd, 0, &cbFrame));
    bd.ByteWidth = 16; ID3D11Buffer *cbObjPlane, *cbObjCube; CHECK(dev->CreateBuffer(&bd, 0, &cbObjPlane)); CHECK(dev->CreateBuffer(&bd, 0, &cbObjCube));

    D3D11_SAMPLER_DESC smd = {}; smd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; smd.AddressU = smd.AddressV = smd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smd.MaxAnisotropy = 1; smd.ComparisonFunc = D3D11_COMPARISON_NEVER; smd.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11SamplerState *smp; CHECK(dev->CreateSamplerState(&smd, &smp));
    D3D11_DEPTH_STENCIL_DESC dsd = {}; dsd.DepthEnable = TRUE; dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dsd.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    ID3D11DepthStencilState *dssScene; CHECK(dev->CreateDepthStencilState(&dsd, &dssScene));
    dsd.DepthEnable = FALSE; dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; ID3D11DepthStencilState *dssOff; CHECK(dev->CreateDepthStencilState(&dsd, &dssOff));
    D3D11_RASTERIZER_DESC rsd = {}; rsd.FillMode = D3D11_FILL_SOLID; rsd.CullMode = D3D11_CULL_NONE; rsd.DepthClipEnable = TRUE;
    ID3D11RasterizerState *rs; CHECK(dev->CreateRasterizerState(&rsd, &rs));

    // ---- projection: reversed-Z infinite, near 0.025 (clip.z = near, clip.w = viewZ), vertical FOV 56 deg
    const float nearZ = 0.025f, sy = 1.0f / tanf(28.0f * 3.14159265f / 180.0f);

    const int origW = S.W, origH = S.H;
    LARGE_INTEGER qf, t0, t1, tLast; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&t0); tLast = t0;
    double gpuWaitMs = 0; int ok = 0, okLast = 0, imgW = 0, imgH = 0; bool quit = false;
    uint8_t *img = 0; int imgPitch = 0;
    for (int fr = 0; frames == 0 || fr < frames; fr++) {
        MSG msg; while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) { if (msg.message == WM_QUIT) quit = true; TranslateMessage(&msg); DispatchMessageA(&msg); }
        if (quit) { printf("window closed at frame %d\n", fr); break; }
        if (resizeTest && fr == 100) { printf("resize-test: frame %d -> 1600x900\n", fr); set_client_size(hwnd, 1600, 900); }
        if (resizeTest && fr == 200) { printf("resize-test: frame %d -> %dx%d\n", fr, origW, origH); set_client_size(hwnd, origW, origH); }
        if (show && IsIconic(hwnd)) { Sleep(10); fr--; continue; }   // minimized: nothing to present
        const int W = S.W, H = S.H, RW = S.RW, RH = S.RH;
        const float sx = sy * RH / RW;
        M4 P = {{{sx, 0, 0, 0}, {0, sy, 0, 0}, {0, 0, 0, 1}, {0, 0, nearZ, 0}}};
        float t;
        if (realtime) { LARGE_INTEGER n; QueryPerformanceCounter(&n); t = (float)((n.QuadPart - t0.QuadPart) / (double)qf.QuadPart); }
        else t = fr / 60.0f;   // deterministic animation

        // camera: slow orbit around origin, looking at it
        float ang = t * 0.2f, cam[3] = {sinf(ang) * 9.0f, 1.6f + 0.5f * sinf(t * 0.3f), -cosf(ang) * 9.0f};
        float fwd[3] = {-cam[0], 0.3f - cam[1], -cam[2]}; norm3(fwd);
        float upw[3] = {0, 1, 0}, right[3], up[3]; cross3(upw, fwd, right); norm3(right); cross3(fwd, right, up);
        M4 V = {{{right[0], up[0], fwd[0], 0}, {right[1], up[1], fwd[1], 0}, {right[2], up[2], fwd[2], 0}, {0, 0, 0, 1}}};
        M4 VP = mul(V, P);

        D3D11_MAPPED_SUBRESOURCE ms;
        CHECK(ctx->Map(cbFrame, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms));
        memset(ms.pData, 0, 4480);
        memcpy((uint8_t *)ms.pData + 4320, &VP, 64);
        memcpy((uint8_t *)ms.pData + 4400, cam, 12);
        ctx->Unmap(cbFrame, 0);
        float op[4] = {0, t, 0, 0};
        CHECK(ctx->Map(cbObjPlane, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)); memcpy(ms.pData, op, 16); ctx->Unmap(cbObjPlane, 0);
        op[0] = 1;
        CHECK(ctx->Map(cbObjCube, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)); memcpy(ms.pData, op, 16); ctx->Unmap(cbObjCube, 0);

        // ---- scene pass (render res)
        float sky[4] = {0.02f, 0.03f, 0.07f, 1};
        ctx->ClearRenderTargetView(S.hdrRTV, sky);
        ctx->ClearDepthStencilView(S.dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.0f, 0);   // reversed-Z: far = 0
        ctx->OMSetRenderTargets(1, &S.hdrRTV, S.dsv);
        ctx->OMSetDepthStencilState(dssScene, 0);
        D3D11_VIEWPORT vpR = {0, 0, (float)RW, (float)RH, 0, 1}; ctx->RSSetViewports(1, &vpR);
        ctx->RSSetState(rs);
        ctx->IASetInputLayout(0); ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vsScene, 0, 0); ctx->PSSetShader(psScene, 0, 0);
        ctx->VSSetConstantBuffers(1, 1, &cbFrame); ctx->PSSetConstantBuffers(1, 1, &cbFrame);
        ctx->VSSetConstantBuffers(0, 1, &cbObjPlane); ctx->PSSetConstantBuffers(0, 1, &cbObjPlane);
        ctx->Draw(6, 0);
        ctx->VSSetConstantBuffers(0, 1, &cbObjCube); ctx->PSSetConstantBuffers(0, 1, &cbObjCube);
        ctx->DrawInstanced(36, 8, 0, 0);

        // ---- tonemap (render res, no DSV)
        ctx->OMSetRenderTargets(1, &S.ldrRTV, 0);
        ctx->OMSetDepthStencilState(dssOff, 0);
        ctx->VSSetShader(vsFS, 0, 0); ctx->PSSetShader(psTM, 0, 0); ctx->PSSetShaderResources(0, 1, &S.hdrSRV);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView *nullSRV = 0; ID3D11UnorderedAccessView *nullUAV = 0; ID3D11RenderTargetView *nullRTV = 0;
        ctx->PSSetShaderResources(0, 1, &nullSRV);
        ctx->OMSetRenderTargets(1, &nullRTV, 0);

        // ---- EASU (render -> display)
        UINT gx = (W + 15) / 16, gy = (H + 15) / 16;
        ctx->CSSetShader(csEasu, 0, 0);
        ctx->CSSetShaderResources(0, 1, &S.ldrSRV); ctx->CSSetSamplers(0, 1, &smp); ctx->CSSetConstantBuffers(2, 1, &S.cbFfx);
        ctx->CSSetUnorderedAccessViews(0, 1, &S.upUAV, 0);
        ctx->Dispatch(gx, gy, 1);
        if (unbind) { ctx->CSSetUnorderedAccessViews(0, 1, &nullUAV, 0); ctx->CSSetShaderResources(0, 1, &nullSRV); }

        // ---- RCAS (display -> display)
        ctx->CSSetShader(csRcas, 0, 0);
        ctx->CSSetUnorderedAccessViews(0, 1, &S.sharpUAV, 0);   // UAV first: replaces EASU output UAV so the SRV bind is not refused
        ctx->CSSetShaderResources(0, 1, &S.upSRV); ctx->CSSetConstantBuffers(2, 1, &S.cbFfx);
        ctx->Dispatch(gx, gy, 1);
        ctx->CSSetUnorderedAccessViews(0, 1, &nullUAV, 0); ctx->CSSetShaderResources(0, 1, &nullSRV);   // always: backbuffer copy reads it
        ctx->CSSetShader(0, 0, 0);

        // ---- copy to backbuffer
        ctx->OMSetRenderTargets(1, &S.bbRTV, 0);
        D3D11_VIEWPORT vpD = {0, 0, (float)W, (float)H, 0, 1}; ctx->RSSetViewports(1, &vpD);
        ctx->VSSetShader(vsFS, 0, 0); ctx->PSSetShader(psCopy, 0, 0); ctx->PSSetShaderResources(0, 1, &S.sharpSRV);
        ctx->Draw(3, 0);
        ctx->PSSetShaderResources(0, 1, &nullSRV);
        ctx->OMSetRenderTargets(1, &nullRTV, 0);

        if (frames != 0 && fr == frames - 1) {   // read back final backbuffer before Present (flip-discard)
            LARGE_INTEGER a, b; QueryPerformanceCounter(&a);
            D3D11_TEXTURE2D_DESC sdsc; S.bb->GetDesc(&sdsc);
            sdsc.Usage = D3D11_USAGE_STAGING; sdsc.BindFlags = 0; sdsc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sdsc.MiscFlags = 0;
            ID3D11Texture2D *st; CHECK(dev->CreateTexture2D(&sdsc, 0, &st));
            ctx->CopyResource(st, S.bb);
            CHECK(ctx->Map(st, 0, D3D11_MAP_READ, 0, &ms));
            imgW = W; imgH = H;
            imgPitch = ms.RowPitch; img = (uint8_t *)malloc((size_t)imgPitch * H); memcpy(img, ms.pData, (size_t)imgPitch * H);
            ctx->Unmap(st, 0); st->Release();
            QueryPerformanceCounter(&b); gpuWaitMs = (b.QuadPart - a.QuadPart) * 1000.0 / qf.QuadPart;
        }
        HRESULT hr = sc->Present(vsync ? 1 : 0, 0);
        if (FAILED(hr)) { printf("Present failed 0x%08lx at frame %d (removed reason 0x%08lx)\n", (unsigned long)hr, fr, (unsigned long)dev->GetDeviceRemovedReason()); return 5; }
        ok++;

        if (show) {   // once per second: app presents + DXGI frame statistics
            LARGE_INTEGER n; QueryPerformanceCounter(&n);
            double dt = (n.QuadPart - tLast.QuadPart) / (double)qf.QuadPart;
            if (dt >= 1.0) {
                DXGI_FRAME_STATISTICS fs = {}; HRESULT fh = sc->GetFrameStatistics(&fs);
                if (SUCCEEDED(fh) || fh == DXGI_ERROR_FRAME_STATISTICS_DISJOINT)   // DISJOINT (e.g. right after a resize) still fills the struct
                    printf("[%5.1fs] presented=%d (%.1f/s) display=%dx%d PresentCount=%u PresentRefreshCount=%u SyncRefreshCount=%u%s\n",
                           (n.QuadPart - t0.QuadPart) / (double)qf.QuadPart, ok, (ok - okLast) / dt, W, H, fs.PresentCount, fs.PresentRefreshCount, fs.SyncRefreshCount,
                           SUCCEEDED(fh) ? "" : " (disjoint)");
                else
                    printf("[%5.1fs] presented=%d (%.1f/s) display=%dx%d GetFrameStatistics=0x%08lx\n",
                           (n.QuadPart - t0.QuadPart) / (double)qf.QuadPart, ok, (ok - okLast) / dt, W, H, (unsigned long)fh);
                tLast = n; okLast = ok;
            }
        }
    }
    QueryPerformanceCounter(&t1);
    double total = (t1.QuadPart - t0.QuadPart) * 1000.0 / qf.QuadPart;
    if (show) {
        DXGI_FRAME_STATISTICS fs = {}; HRESULT fh = sc->GetFrameStatistics(&fs);
        printf("final: presented=%d resizes=%d PresentCount=%u PresentRefreshCount=%u (hr 0x%08lx)\n", ok, g_resizes, fs.PresentCount, fs.PresentRefreshCount, (unsigned long)fh);
    }
    if (!img) { printf("STATS frames=%d total=%.1fms avg=%.3fms/frame (no readback: run ended early)\n", ok, total, ok ? total / ok : 0.0); DestroyWindow(hwnd); return 0; }

    // ---- stats
    const int W2 = imgW, H2 = imgH;
    double sum[3] = {0, 0, 0}; uint64_t h = 0xcbf29ce484222325ull;
    for (int y = 0; y < H2; y++) {
        const uint8_t *r = img + (size_t)y * imgPitch;
        for (int x = 0; x < W2; x++) { for (int c = 0; c < 3; c++) { sum[c] += r[x*4+c]; h ^= r[x*4+c]; h *= 0x100000001b3ull; } }
    }
    auto px = [&](int x, int y) { return *(uint32_t *)(img + (size_t)y * imgPitch + x * 4); };
    uint32_t marker = px(8, 8);
    bool green = (marker & 0xff) < 40 && ((marker >> 8) & 0xff) > 200 && ((marker >> 16) & 0xff) < 40;
    printf("STATS frames=%d total=%.1fms avg=%.3fms/frame readback=%.1fms mean_rgb=%.1f,%.1f,%.1f img_fnv=0x%016llx px(8,8)=%08x marker=%s px(center)=%08x\n",
           ok, total, total / ok, gpuWaitMs, sum[0] / ((double)W2 * H2), sum[1] / ((double)W2 * H2), sum[2] / ((double)W2 * H2),
           (unsigned long long)h, marker, green ? "GREEN" : "none", px(W2 / 2, H2 / 2));
    if (tag) {
        char path[MAX_PATH]; GetModuleFileNameA(0, path, MAX_PATH);
        char *s = strrchr(path, '\\'); if (s) s[1] = 0; else path[0] = 0;
        char out[MAX_PATH]; snprintf(out, sizeof out, "%sout_%s.bmp", path, tag);
        if (!write_bmp(out, img, W2, H2, imgPitch)) { printf("cannot write %s\n", out); return 6; }
        printf("saved %s\n", out);
    }
    free(img);
    if (IsWindow(hwnd)) DestroyWindow(hwnd);
    return 0;
}
