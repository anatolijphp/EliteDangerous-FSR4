// ED-FSR: D3D11 -> D3D12 bridge running AMD FidelityFX upscaler (FSR 3.1 / FSR 4 via driver).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>
#include <utility>
#include "fsr.h"
#include "ffx_api.h"
#include "ffx_upscale.h"
#include "dx12/ffx_api_dx12.h"

FsrSettings g_fsrCfg;
CtxFns g_o;

template <class T> static void rel(T *&p) { if (p) { p->Release(); p = nullptr; } }

// ------------------------------------------------------------------ state
static bool g_failed, g_inited;
static ID3D11Device *d11; static ID3D11Device1 *d11_1; static ID3D11Device5 *d11_5;
static ID3D11DeviceContext4 *ctx4;
static ID3D12Device *d12; static ID3D12CommandQueue *q12;
static ID3D12CommandAllocator *alloc12[3]; static UINT64 allocFence[3];
static ID3D12GraphicsCommandList *cl12;
static ID3D12Fence *fence12; static ID3D11Fence *fence11; static UINT64 fval; static HANDLE fevt;

struct Shared { ID3D12Resource *r12 = 0; ID3D11Texture2D *t11 = 0; ID3D11UnorderedAccessView *uav = 0; };
static Shared sColor, sDepth, sMV, sOut, sReactive;
static ID3D11Texture2D *snapTex, *snapSrc;           // opaque snapshot copy / the HDR target it was taken from
static ID3D11ShaderResourceView *snapSrv, *hdrSrv; static ID3D11Texture2D *hdrSrvTex;
static bool snapValid;
static UINT rw, rh, dw, dh;  // render / display size

static HMODULE ffxDll;
static PfnFfxCreateContext pCreate; static PfnFfxDestroyContext pDestroy;
static PfnFfxQuery pQuery; static PfnFfxDispatch pDispatch; static PfnFfxConfigure pConfigure;
static ffxContext fctx;
static int jitterPhases = 8, jitterIndex;
static float jx, jy;
static bool resetNext = true;
static char g_provName[64] = "-";
static bool g_hasFsr4 = true, g_versionsQueried, g_reinit;
static uint64_t g_verFsr4, g_verFsr31;
static LARGE_INTEGER lastQpc, qpf;

// D3D11 prep pass (depth copy + motion vectors)
static ID3D11ComputeShader *csPrep;
static ID3D11Buffer *cbParams;
static ID3D11Buffer *cbFrame[2]; static UINT cbFrameSize; static int cbCur; static bool cbHave[2];
static ID3D11Buffer *cbStaging[3]; static UINT64 stagingFrame[3]; static UINT64 frameNo;
static BridgeState g_bs;
static ID3D11ShaderResourceView *depthSrv; static ID3D11Texture2D *depthSrvTex;

bool fsr_failed() { return g_failed; }
const char *fsr_provider_name() { return g_provName; }
bool fsr_has_fsr4() { return g_hasFsr4; }
void fsr_request_reinit() { g_reinit = true; }
void fsr_jitter(float &x, float &y) { x = jx * g_fsrCfg.jitterSignX; y = jy * g_fsrCfg.jitterSignY; }
void fsr_reset() { resetNext = true; cbHave[0] = cbHave[1] = false; }

static void ffxMsg(uint32_t type, const wchar_t *m) {
    char b[1024]; WideCharToMultiByte(CP_UTF8, 0, m, -1, b, sizeof b, 0, 0);
    logf("FFX %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "ERROR" : "warn", b);
}

static bool fail(const char *what, HRESULT hr = 0) {
    logf("FSR init FAILED at %s (hr=0x%08lx) -> falling back to native FSR1", what, (unsigned long)hr);
    g_failed = true; return false;
}

// ------------------------------------------------------------------ prep shader
static const char *kPrepCS = R"(
Texture2D<float> depthTex : register(t0);
Texture2D<float3> hdrFinal : register(t1);
Texture2D<float3> hdrOpaque : register(t2);
RWTexture2D<float> outDepth : register(u0);
RWTexture2D<float2> outMV : register(u1);
RWTexture2D<float> outReactive : register(u2);
cbuffer Cur  : register(b0) { float4 cur[276]; };
cbuffer Prev : register(b1) { float4 prv[276]; };
cbuffer Params : register(b2) { float2 rsize; float cockpitDepth; float valid; float zNear; float useMV; float2 jitter;
                                 float useReactive; float reactiveScale; float2 pad2; };

float4x4 inv(float4x4 m) {
    // gluInvertMatrix: layout-agnostic (same layout in and out)
    float a[16] = {m[0][0],m[0][1],m[0][2],m[0][3], m[1][0],m[1][1],m[1][2],m[1][3],
                   m[2][0],m[2][1],m[2][2],m[2][3], m[3][0],m[3][1],m[3][2],m[3][3]};
    float r[16];
    r[0] = a[5]*a[10]*a[15] - a[5]*a[11]*a[14] - a[9]*a[6]*a[15] + a[9]*a[7]*a[14] + a[13]*a[6]*a[11] - a[13]*a[7]*a[10];
    r[4] = -a[4]*a[10]*a[15] + a[4]*a[11]*a[14] + a[8]*a[6]*a[15] - a[8]*a[7]*a[14] - a[12]*a[6]*a[11] + a[12]*a[7]*a[10];
    r[8] = a[4]*a[9]*a[15] - a[4]*a[11]*a[13] - a[8]*a[5]*a[15] + a[8]*a[7]*a[13] + a[12]*a[5]*a[11] - a[12]*a[7]*a[9];
    r[12] = -a[4]*a[9]*a[14] + a[4]*a[10]*a[13] + a[8]*a[5]*a[14] - a[8]*a[6]*a[13] - a[12]*a[5]*a[10] + a[12]*a[6]*a[9];
    r[1] = -a[1]*a[10]*a[15] + a[1]*a[11]*a[14] + a[9]*a[2]*a[15] - a[9]*a[3]*a[14] - a[13]*a[2]*a[11] + a[13]*a[3]*a[10];
    r[5] = a[0]*a[10]*a[15] - a[0]*a[11]*a[14] - a[8]*a[2]*a[15] + a[8]*a[3]*a[14] + a[12]*a[2]*a[11] - a[12]*a[3]*a[10];
    r[9] = -a[0]*a[9]*a[15] + a[0]*a[11]*a[13] + a[8]*a[1]*a[15] - a[8]*a[3]*a[13] - a[12]*a[1]*a[11] + a[12]*a[3]*a[9];
    r[13] = a[0]*a[9]*a[14] - a[0]*a[10]*a[13] - a[8]*a[1]*a[14] + a[8]*a[2]*a[13] + a[12]*a[1]*a[10] - a[12]*a[2]*a[9];
    r[2] = a[1]*a[6]*a[15] - a[1]*a[7]*a[14] - a[5]*a[2]*a[15] + a[5]*a[3]*a[14] + a[13]*a[2]*a[7] - a[13]*a[3]*a[6];
    r[6] = -a[0]*a[6]*a[15] + a[0]*a[7]*a[14] + a[4]*a[2]*a[15] - a[4]*a[3]*a[14] - a[12]*a[2]*a[7] + a[12]*a[3]*a[6];
    r[10] = a[0]*a[5]*a[15] - a[0]*a[7]*a[13] - a[4]*a[1]*a[15] + a[4]*a[3]*a[13] + a[12]*a[1]*a[7] - a[12]*a[3]*a[5];
    r[14] = -a[0]*a[5]*a[14] + a[0]*a[6]*a[13] + a[4]*a[1]*a[14] - a[4]*a[2]*a[13] - a[12]*a[1]*a[6] + a[12]*a[2]*a[5];
    r[3] = -a[1]*a[6]*a[11] + a[1]*a[7]*a[10] + a[5]*a[2]*a[11] - a[5]*a[3]*a[10] - a[9]*a[2]*a[7] + a[9]*a[3]*a[6];
    r[7] = a[0]*a[6]*a[11] - a[0]*a[7]*a[10] - a[4]*a[2]*a[11] + a[4]*a[3]*a[10] + a[8]*a[2]*a[7] - a[8]*a[3]*a[6];
    r[11] = -a[0]*a[5]*a[11] + a[0]*a[7]*a[9] + a[4]*a[1]*a[11] - a[4]*a[3]*a[9] - a[8]*a[1]*a[7] + a[8]*a[3]*a[5];
    r[15] = a[0]*a[5]*a[10] - a[0]*a[6]*a[9] - a[4]*a[1]*a[10] + a[4]*a[2]*a[9] + a[8]*a[1]*a[6] - a[8]*a[2]*a[5];
    float det = a[0]*r[0] + a[1]*r[4] + a[2]*r[8] + a[3]*r[12];
    float id = 1.0 / det;
    return float4x4(r[0],r[1],r[2],r[3], r[4],r[5],r[6],r[7], r[8],r[9],r[10],r[11], r[12],r[13],r[14],r[15]) * id;
}

[numthreads(8,8,1)] void main(uint2 id : SV_DispatchThreadID) {
    if (any(id >= (uint2)rsize)) return;
    float z = depthTex[id];
    outDepth[id] = z;
    float2 mv = 0;
    if (valid > 0 && useMV > 0) {
        // per-frame cbuffer (VS b1): viewProj (row-vector, camera-relative) at byte 4320, camPos at 4400
        float4x4 vpC = float4x4(cur[270], cur[271], cur[272], cur[273]);
        float4x4 vpP = float4x4(prv[270], prv[271], prv[272], prv[273]);
        float3 camC = cur[275].xyz, camP = prv[275].xyz;
        float2 uv = (id + 0.5 - jitter) / rsize;
        float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2);
        float viewZ = z > 0 ? zNear / z : 1e30;
        if (viewZ >= cockpitDepth) {
            float4 h = mul(float4(ndc, z, 1), inv(vpC));
            h.xyz += (camC - camP) * h.w;
            float4 c = mul(h, vpP);
            if (abs(c.w) > 1e-8) {
                float2 n2 = c.xy / c.w;
                float2 uvP = float2(n2.x * 0.5 + 0.5, 0.5 - n2.y * 0.5);
                mv = uvP - uv;
                if (any(abs(mv) > 0.5)) mv = 0;
            }
        }
    }
    outMV[id] = mv;
    float r = 0;
    if (useReactive > 0) {
        float3 a = hdrFinal[id], b = hdrOpaque[id];
        float3 d = abs(a - b);
        float m = max(d.r, max(d.g, d.b));
        float l = max(max(a.r, max(a.g, a.b)), 1e-3);
        r = saturate(reactiveScale * smoothstep(0.02, 0.25, m / l));   // relative change -> transparent/HUD coverage
    }
    outReactive[id] = r;
}
)";

// ------------------------------------------------------------------ helpers
static bool make_shared(UINT w, UINT h, DXGI_FORMAT fmt, bool uav11, Shared &s, const char *name) {
    // Create in D3D11 (NT-handle shared) and open in D3D12 — the reverse direction fails on some drivers.
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = fmt; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    HRESULT hr = d11->CreateTexture2D(&td, 0, &s.t11);
    if (FAILED(hr)) { logf("CreateTexture2D %s fmt %d", name, fmt); return fail(name, hr); }
    IDXGIResource1 *xr = 0; HANDLE hd = 0;
    hr = s.t11->QueryInterface(__uuidof(IDXGIResource1), (void **)&xr);
    if (SUCCEEDED(hr)) hr = xr->CreateSharedHandle(0, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, 0, &hd);
    rel(xr);
    if (FAILED(hr)) return fail("CreateSharedHandle(11)", hr);
    hr = d12->OpenSharedHandle(hd, __uuidof(ID3D12Resource), (void **)&s.r12);
    CloseHandle(hd);
    if (FAILED(hr)) return fail("OpenSharedHandle(12)", hr);
    if (uav11) {
        hr = d11->CreateUnorderedAccessView(s.t11, 0, &s.uav);
        if (FAILED(hr)) return fail("UAV11", hr);
    }
    return true;
}
static void free_shared(Shared &s) { rel(s.uav); rel(s.t11); rel(s.r12); }

static void gpu_idle() {
    if (!fence12) return;
    // keep the shared fence monotonic: D3D11 signals v, D3D12 waits v then signals v+1
    UINT64 v = ++fval;
    if (ctx4) ctx4->Signal(fence11, v);
    if (d11) { ID3D11DeviceContext *ic = 0; d11->GetImmediateContext(&ic); if (ic) { ic->Flush(); ic->Release(); } }
    if (q12) { q12->Wait(fence12, v); q12->Signal(fence12, ++fval); }
    if (fence12->GetCompletedValue() < fval) { fence12->SetEventOnCompletion(fval, fevt); WaitForSingleObject(fevt, INFINITE); }
}

static void destroy_sized() {
    gpu_idle();
    if (fctx && pDestroy) { pDestroy(&fctx, 0); fctx = 0; }
    free_shared(sColor); free_shared(sDepth); free_shared(sMV); free_shared(sOut); free_shared(sReactive);
    rw = rh = dw = dh = 0;
}

static bool init_device(ID3D11Device *dev, ID3D11DeviceContext *ctx) {
    d11 = dev;
    if (FAILED(dev->QueryInterface(__uuidof(ID3D11Device1), (void **)&d11_1))) return fail("ID3D11Device1");
    if (FAILED(dev->QueryInterface(__uuidof(ID3D11Device5), (void **)&d11_5))) return fail("ID3D11Device5");
    if (FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&ctx4))) return fail("ID3D11DeviceContext4");

    // same adapter for D3D12
    IDXGIDevice *xd = 0; IDXGIAdapter *ad = 0; DXGI_ADAPTER_DESC desc = {};
    dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&xd);
    if (!xd || FAILED(xd->GetAdapter(&ad))) return fail("GetAdapter");
    ad->GetDesc(&desc); rel(xd);
    char an[128]; WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, an, sizeof an, 0, 0);
    HMODULE m12 = LoadLibraryW(L"d3d12.dll");
    auto pD3D12CreateDevice = (PFN_D3D12_CREATE_DEVICE)(m12 ? GetProcAddress(m12, "D3D12CreateDevice") : 0);
    if (!pD3D12CreateDevice) { rel(ad); return fail("d3d12.dll"); }
    HRESULT hr = pD3D12CreateDevice(ad, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), (void **)&d12);
    rel(ad);
    if (FAILED(hr)) return fail("D3D12CreateDevice", hr);
    logf("D3D12 device on %s", an);

    D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(hr = d12->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void **)&q12))) return fail("queue", hr);
    for (int i = 0; i < 3; i++)
        if (FAILED(hr = d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void **)&alloc12[i]))) return fail("allocator", hr);
    if (FAILED(hr = d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc12[0], 0, __uuidof(ID3D12GraphicsCommandList), (void **)&cl12))) return fail("cmdlist", hr);
    cl12->Close();
    if (FAILED(hr = d12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence), (void **)&fence12))) return fail("fence12", hr);
    HANDLE fh = 0;
    if (FAILED(hr = d12->CreateSharedHandle(fence12, 0, GENERIC_ALL, 0, &fh))) return fail("fence handle", hr);
    hr = d11_5->OpenSharedFence(fh, __uuidof(ID3D11Fence), (void **)&fence11);
    CloseHandle(fh);
    if (FAILED(hr)) return fail("OpenSharedFence", hr);
    fevt = CreateEventW(0, FALSE, FALSE, 0);

    // FidelityFX loader (resolves effect DLLs from its own folder)
    ffxDll = LoadLibraryW(L"D:\\ed-fsr\\bin\\amd_fidelityfx_loader_dx12.dll");
    if (!ffxDll) return fail("load amd_fidelityfx_loader_dx12.dll", GetLastError());
    pCreate = (PfnFfxCreateContext)GetProcAddress(ffxDll, "ffxCreateContext");
    pDestroy = (PfnFfxDestroyContext)GetProcAddress(ffxDll, "ffxDestroyContext");
    pQuery = (PfnFfxQuery)GetProcAddress(ffxDll, "ffxQuery");
    pDispatch = (PfnFfxDispatch)GetProcAddress(ffxDll, "ffxDispatch");
    pConfigure = (PfnFfxConfigure)GetProcAddress(ffxDll, "ffxConfigure");
    if (!pCreate || !pDestroy || !pQuery || !pDispatch) return fail("ffx exports");

    // D3D11 prep shader + params
    ID3DBlob *b = 0, *e = 0;
    if (FAILED(D3DCompile(kPrepCS, strlen(kPrepCS), "prep", 0, 0, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b, &e))) {
        logf("prep compile: %s", e ? (char *)e->GetBufferPointer() : "?"); return fail("prep compile");
    }
    // NOTE: device CreateComputeShader is hooked; harmless (hash won't match EASU)
    hr = d11->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), 0, &csPrep); b->Release();
    if (FAILED(hr)) return fail("prep CS", hr);
    D3D11_BUFFER_DESC bd = {48, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    if (FAILED(hr = d11->CreateBuffer(&bd, 0, &cbParams))) return fail("params cb", hr);
    QueryPerformanceFrequency(&qpf);
    logf("FSR bridge device init ok");
    return true;
}

static bool init_sized(UINT nrw, UINT nrh, UINT ndw, UINT ndh) {
    destroy_sized();
    rw = nrw; rh = nrh; dw = ndw; dh = ndh;
    if (!make_shared(rw, rh, DXGI_FORMAT_R8G8B8A8_UNORM, false, sColor, "color")) return false;
    if (!make_shared(rw, rh, DXGI_FORMAT_R32_FLOAT, true, sDepth, "depth")) return false;
    if (!make_shared(rw, rh, DXGI_FORMAT_R16G16_FLOAT, true, sMV, "mv")) return false;
    if (!make_shared(rw, rh, DXGI_FORMAT_R8_UNORM, true, sReactive, "reactive")) return false;
    if (!make_shared(dw, dh, DXGI_FORMAT_R8G8B8A8_UNORM, false, sOut, "output")) return false;

    ffxCreateBackendDX12Desc be = {}; be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12; be.device = d12;
    ffxCreateContextDescUpscaleVersion ver = {}; ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    ver.version = FFX_UPSCALER_VERSION; ver.header.pNext = &be.header;
    ffxCreateContextDescUpscale cd = {}; cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE; cd.header.pNext = &ver.header;
    cd.flags = FFX_UPSCALE_ENABLE_DEBUG_CHECKING | FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_DEPTH_INFINITE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE |
               FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
    cd.maxRenderSize = {dw, dh}; cd.maxUpscaleSize = {dw, dh}; cd.fpMessage = ffxMsg;
    // version selection: FSR 4 (ML) or FSR 3.1
    if (!g_versionsQueried) {
        g_versionsQueried = true;
        uint64_t cnt = 0;
        ffxQueryDescGetVersions qv = {}; qv.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        qv.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE; qv.device = d12; qv.outputCount = &cnt;
        pQuery(0, &qv.header);
        uint64_t ids[32] = {}; const char *names[32] = {};
        if (cnt > 32) cnt = 32;
        qv.versionIds = ids; qv.versionNames = names;
        pQuery(0, &qv.header);
        g_hasFsr4 = false;
        for (uint64_t i = 0; i < cnt; i++) {
            const char *n = names[i] ? names[i] : "";
            logf("upscaler version available: '%s' id 0x%llx", n, (unsigned long long)ids[i]);
            if (n[0] == '4' && !g_verFsr4) { g_verFsr4 = ids[i]; g_hasFsr4 = true; }
            if (!strncmp(n, "3.1", 3) && !g_verFsr31) g_verFsr31 = ids[i];
        }
    }
    ffxOverrideVersion ov = {}; ov.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    uint64_t want = g_fsrCfg.version == 2 ? g_verFsr31 : g_verFsr4;
    if (want) { ov.versionId = want; be.header.pNext = &ov.header; }
    ffxReturnCode_t rc = pCreate(&fctx, &cd.header, 0);
    if (rc != FFX_API_RETURN_OK) { logf("ffxCreateContext rc=%u", rc); return fail("ffxCreateContext"); }

    ffxQueryGetProviderVersion pv = {}; pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    pQuery(&fctx, &pv.header);
    ffxQueryDescUpscaleGetJitterPhaseCount jp = {}; jp.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
    jp.renderWidth = rw; jp.displayWidth = dw; jp.pOutPhaseCount = &jitterPhases;
    pQuery(&fctx, &jp.header);
    snprintf(g_provName, sizeof g_provName, "%s", pv.versionName ? pv.versionName : "?");
    logf("FFX upscaler context: provider '%s' (id 0x%llx), render %ux%u -> %ux%u, jitter phases %d",
         pv.versionName ? pv.versionName : "?", (unsigned long long)pv.versionId, rw, rh, dw, dh, jitterPhases);
    fsr_reset();
    return true;
}

static void next_jitter() {
    jitterIndex = (jitterIndex + 1) % (jitterPhases > 0 ? jitterPhases : 8);
    ffxQueryDescUpscaleGetJitterOffset jo = {}; jo.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET;
    jo.index = jitterIndex; jo.phaseCount = jitterPhases; jo.pOutX = &jx; jo.pOutY = &jy;
    if (!fctx || pQuery(&fctx, &jo.header) != FFX_API_RETURN_OK) jx = jy = 0;
}

// ------------------------------------------------------------------ per-frame camera cbuffer
void fsr_capture_frame_cb(ID3D11DeviceContext *ctx) {
    if (g_failed || !d11) return;
    ID3D11Buffer *b = 0; ctx->VSGetConstantBuffers(1, 1, &b);
    if (!b) return;
    D3D11_BUFFER_DESC d; b->GetDesc(&d);
    if (d.ByteWidth < 4416) { b->Release(); return; }
    if (cbFrameSize != d.ByteWidth) {
        rel(cbFrame[0]); rel(cbFrame[1]); for (auto &x : cbStaging) rel(x);
        D3D11_BUFFER_DESC nd = {d.ByteWidth, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        d11->CreateBuffer(&nd, 0, &cbFrame[0]); d11->CreateBuffer(&nd, 0, &cbFrame[1]);
        D3D11_BUFFER_DESC sd = {d.ByteWidth, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
        for (auto &x : cbStaging) d11->CreateBuffer(&sd, 0, &x);
        cbFrameSize = d.ByteWidth; cbHave[0] = cbHave[1] = false;
        logf("frame cbuffer size %u", cbFrameSize);
    }
    if (cbFrame[cbCur]) { ctx->CopyResource(cbFrame[cbCur], b); cbHave[cbCur] = true; }
    b->Release();
}

static void camera_readback(ID3D11DeviceContext *ctx) {
    // continuous ring: copy this frame's camera cbuffer, read the oldest one that is ready (1-2 frames late)
    if (!cbStaging[0] || !cbHave[cbCur]) return;
    int w = (int)(frameNo % 3);
    ctx->CopyResource(cbStaging[w], cbFrame[cbCur]); stagingFrame[w] = frameNo;
    for (int k = 2; k >= 1; k--) {
        int r = (int)((frameNo + 3 - k) % 3);
        if (!stagingFrame[r]) continue;
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(ctx->Map(cbStaging[r], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) continue;
        const float *f = (const float *)m.pData;
        // view rotation rows at byte 3664 (columns = right, up, forward); camera position at 4400
        for (int i = 0; i < 3; i++) {
            g_bs.right[i] = f[3664 / 4 + i * 4 + 0];
            g_bs.up[i] = f[3664 / 4 + i * 4 + 1];
            g_bs.forward[i] = f[3664 / 4 + i * 4 + 2];
            g_bs.camPos[i] = f[4400 / 4 + i];
        }
        g_bs.camValid = true;
        if ((frameNo % 1200) == 7)
            logf("cam: fwd(%.3f %.3f %.3f) pos(%.3f %.3f %.3f) vp11=%.4f jitter(%.3f %.3f)", g_bs.forward[0], g_bs.forward[1], g_bs.forward[2],
                 g_bs.camPos[0], g_bs.camPos[1], g_bs.camPos[2], f[4336 / 4 + 1], jx, jy);
        ctx->Unmap(cbStaging[r], 0);
        break;
    }
}

static ID3D11ShaderResourceView *depth_srv(ID3D11Texture2D *t) {
    if (t == depthSrvTex && depthSrv) return depthSrv;
    rel(depthSrv); depthSrvTex = t;
    if (!t) return 0;
    D3D11_TEXTURE2D_DESC d; t->GetDesc(&d);
    if (!(d.BindFlags & D3D11_BIND_SHADER_RESOURCE)) { logf("depth texture has no SRV bind flag (fmt %d)", d.Format); return 0; }
    DXGI_FORMAT f = DXGI_FORMAT_UNKNOWN;
    switch (d.Format) {
    case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: f = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; break;
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: f = DXGI_FORMAT_R32_FLOAT; break;
    case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: f = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
    default: logf("unsupported depth format %d", d.Format); return 0;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {}; sd.Format = f; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
    HRESULT hr = d11->CreateShaderResourceView(t, &sd, &depthSrv);
    if (FAILED(hr)) logf("depth SRV failed hr=0x%08lx", (unsigned long)hr);
    else logf("depth SRV created (%ux%u fmt %d)", d.Width, d.Height, d.Format);
    return depthSrv;
}

static void barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b, D3D12_RESOURCE_BARRIER &out) {
    out = {}; out.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; out.Transition.pResource = r;
    out.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; out.Transition.StateBefore = a; out.Transition.StateAfter = b;
}

bool fsr_bridge_init(ID3D11Device *dev, ID3D11DeviceContext *ctx) {
    if (!g_inited) { g_inited = true; init_device(dev, ctx); }
    return !g_failed;
}
BridgeState *fsr_state() {
    g_bs.d12 = d12; g_bs.q12 = q12; g_bs.fence12 = fence12; g_bs.fence11 = fence11; g_bs.ctx4 = ctx4; g_bs.fval = &fval;
    g_bs.ffxCreate = (void *)pCreate; g_bs.ffxDestroy = (void *)pDestroy; g_bs.ffxConfigure = (void *)pConfigure;
    g_bs.ffxQuery = (void *)pQuery; g_bs.ffxDispatch = (void *)pDispatch;
    return &g_bs;
}

void fsr_snapshot_opaque(ID3D11DeviceContext *ctx, ID3D11Texture2D *hdr) {
    if (g_failed || !d11 || !hdr) return;
    D3D11_TEXTURE2D_DESC d; hdr->GetDesc(&d);
    if (!(d.BindFlags & D3D11_BIND_SHADER_RESOURCE) || d.SampleDesc.Count != 1) return;
    if (snapTex) {
        D3D11_TEXTURE2D_DESC sd; snapTex->GetDesc(&sd);
        if (sd.Width != d.Width || sd.Height != d.Height || sd.Format != d.Format) { rel(snapSrv); rel(snapTex); }
    }
    if (!snapTex) {
        D3D11_TEXTURE2D_DESC nd = d; nd.BindFlags = D3D11_BIND_SHADER_RESOURCE; nd.MiscFlags = 0; nd.CPUAccessFlags = 0; nd.Usage = D3D11_USAGE_DEFAULT;
        nd.MipLevels = 1; nd.ArraySize = 1;
        if (FAILED(d11->CreateTexture2D(&nd, 0, &snapTex))) { logf("snapshot texture failed (fmt %d)", d.Format); return; }
        D3D11_SHADER_RESOURCE_VIEW_DESC vd = {}; vd.Format = d.Format; vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; vd.Texture2D.MipLevels = 1;
        if (d.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS) vd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (d.Format == DXGI_FORMAT_R11G11B10_FLOAT || vd.Format != DXGI_FORMAT_UNKNOWN) d11->CreateShaderResourceView(snapTex, &vd, &snapSrv);
        logf("reactive snapshot target %ux%u fmt %d", d.Width, d.Height, d.Format);
    }
    ctx->CopySubresourceRegion(snapTex, 0, 0, 0, 0, hdr, 0, 0);
    if (snapSrc != hdr) { if (snapSrc) snapSrc->Release(); snapSrc = hdr; hdr->AddRef(); }
    snapValid = true;
}

static ID3D11ShaderResourceView *hdr_srv() {
    if (!snapSrc) return 0;
    if (hdrSrvTex == snapSrc && hdrSrv) return hdrSrv;
    rel(hdrSrv); hdrSrvTex = snapSrc;
    D3D11_TEXTURE2D_DESC d; snapSrc->GetDesc(&d);
    D3D11_SHADER_RESOURCE_VIEW_DESC vd = {}; vd.Format = d.Format; vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; vd.Texture2D.MipLevels = 1;
    if (d.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS) vd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    d11->CreateShaderResourceView(snapSrc, &vd, &hdrSrv);
    return hdrSrv;
}

// ------------------------------------------------------------------ frame
bool fsr_run(const FsrFrame &f) {
    if (g_failed || !f.color || !f.output) return false;
    ID3D11DeviceContext *ctx = f.ctx;
    if (!g_inited) {
        ID3D11Device *dev = 0; ctx->GetDevice(&dev);
        bool ok = fsr_bridge_init(dev, ctx);
        dev->Release();  // device outlives us; d11 kept as weak ref
        if (!ok) return false;
    }
    D3D11_TEXTURE2D_DESC cd, od; f.color->GetDesc(&cd); f.output->GetDesc(&od);
    if (g_reinit) { g_reinit = false; gpu_idle(); destroy_sized(); }
    if (cd.Width != rw || cd.Height != rh || od.Width != dw || od.Height != dh)
        if (!init_sized(cd.Width, cd.Height, od.Width, od.Height)) return false;
    frameNo++;

    // 1. color copy (RGBA8 typeless -> RGBA8 unorm, same family)
    ctx->CopyResource(sColor.t11, f.color);

    // 2. depth copy + motion vectors (D3D11 compute)
    int prevIdx = cbCur ^ 1;
    bool valid = cbHave[cbCur] && cbHave[prevIdx];
    ID3D11ShaderResourceView *dsrv = depth_srv(f.depth);
    ID3D11ShaderResourceView *hsrv = (snapValid && g_fsrCfg.reactive) ? hdr_srv() : 0;
    bool useReact = hsrv && snapSrv;
    { static bool logged; if (useReact && !logged) { logged = true; logf("reactive mask active"); } }
    {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(cbParams, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            float *p = (float *)m.pData;
            p[0] = (float)rw; p[1] = (float)rh; p[2] = g_fsrCfg.cockpitDepth; p[3] = valid ? 1.f : 0.f;
            p[4] = 0.025f; p[5] = (float)g_fsrCfg.mv;
            { float a, b; fsr_jitter(a, b); p[6] = g_fsrCfg.jitter ? a : 0; p[7] = g_fsrCfg.jitter ? b : 0; }
            p[8] = useReact ? 1.f : 0.f; p[9] = g_fsrCfg.reactiveScale; p[10] = p[11] = 0;
            ctx->Unmap(cbParams, 0);
        }
        // save CS state
        ID3D11ComputeShader *ocs = 0; ID3D11ClassInstance *oci[256]; UINT onci = 256; ctx->CSGetShader(&ocs, oci, &onci);
        ID3D11ShaderResourceView *osrv[3] = {}; ctx->CSGetShaderResources(0, 3, osrv);
        ID3D11UnorderedAccessView *ouav[3] = {}; ctx->CSGetUnorderedAccessViews(0, 3, ouav);
        ID3D11Buffer *ocb[3] = {}; ctx->CSGetConstantBuffers(0, 3, ocb);

        ID3D11Buffer *cbs[3] = {cbFrame[cbCur], cbFrame[prevIdx], cbParams};
        ID3D11UnorderedAccessView *uavs[3] = {sDepth.uav, sMV.uav, sReactive.uav};
        ID3D11ShaderResourceView *srvs[3] = {dsrv, useReact ? hsrv : 0, useReact ? snapSrv : 0};
        g_o.CSSetShader(ctx, csPrep, 0, 0);
        g_o.CSSetShaderResources(ctx, 0, 3, srvs);
        g_o.CSSetUnorderedAccessViews(ctx, 0, 3, uavs, 0);
        ctx->CSSetConstantBuffers(0, 3, cbs);
        ctx->Dispatch((rw + 7) / 8, (rh + 7) / 8, 1);
        // restore
        ID3D11UnorderedAccessView *nu[3] = {}; g_o.CSSetUnorderedAccessViews(ctx, 0, 3, nu, 0);
        ID3D11ShaderResourceView *ns[3] = {}; g_o.CSSetShaderResources(ctx, 0, 3, ns);
        g_o.CSSetShader(ctx, ocs, onci ? oci : 0, onci);
        g_o.CSSetShaderResources(ctx, 0, 3, osrv);
        UINT keep[3] = {(UINT)-1, (UINT)-1, (UINT)-1};
        g_o.CSSetUnorderedAccessViews(ctx, 0, 3, ouav, keep);
        ctx->CSSetConstantBuffers(0, 3, ocb);
        if (ocs) ocs->Release(); for (UINT i = 0; i < onci; i++) if (oci[i]) oci[i]->Release();
        for (auto *v : osrv) if (v) v->Release(); for (auto *u : ouav) if (u) u->Release(); for (auto *c : ocb) if (c) c->Release();
    }
    camera_readback(ctx);

    // 3. hand over to D3D12
    UINT64 v11 = ++fval;
    ctx4->Signal(fence11, v11);
    ctx->Flush();

    int ai = (int)(frameNo % 3);
    if (fence12->GetCompletedValue() < allocFence[ai]) {
        fence12->SetEventOnCompletion(allocFence[ai], fevt);
        WaitForSingleObject(fevt, INFINITE);
    }
    alloc12[ai]->Reset();
    cl12->Reset(alloc12[ai], 0);
    D3D12_RESOURCE_BARRIER br[5];
    barrier(sReactive.r12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, br[4]);
    barrier(sColor.r12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, br[0]);
    barrier(sDepth.r12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, br[1]);
    barrier(sMV.r12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, br[2]);
    barrier(sOut.r12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, br[3]);
    cl12->ResourceBarrier(5, br);

    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    float dtMs = lastQpc.QuadPart ? (float)((now.QuadPart - lastQpc.QuadPart) * 1000.0 / qpf.QuadPart) : 16.6f;
    lastQpc = now;

    ffxDispatchDescUpscale dd = {};
    dd.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    dd.commandList = cl12;
    dd.color = ffxApiGetResourceDX12(sColor.r12, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dd.depth = ffxApiGetResourceDX12(sDepth.r12, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dd.motionVectors = ffxApiGetResourceDX12(sMV.r12, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dd.output = ffxApiGetResourceDX12(sOut.r12, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    if (useReact) dd.reactive = ffxApiGetResourceDX12(sReactive.r12, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    float sjx, sjy; fsr_jitter(sjx, sjy);
    dd.jitterOffset = {g_fsrCfg.jitter ? sjx : 0.f, g_fsrCfg.jitter ? sjy : 0.f};
    dd.motionVectorScale = {(float)rw, (float)rh};
    dd.renderSize = {rw, rh};
    dd.upscaleSize = {dw, dh};
    dd.enableSharpening = g_fsrCfg.sharpness > 0;
    dd.sharpness = g_fsrCfg.sharpness;
    dd.frameTimeDelta = dtMs;
    dd.preExposure = 1.0f;
    dd.reset = resetNext;
    dd.cameraNear = 0.025f;
    dd.cameraFar = 1.0e8f;
    dd.cameraFovAngleVertical = 0.98f;
    dd.viewSpaceToMetersFactor = 1.0f;
    dd.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB | (g_fsrCfg.debugView ? FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW : 0);
    ffxReturnCode_t rc = pDispatch(&fctx, &dd.header);
    if (rc != FFX_API_RETURN_OK) {
        static int n; if (n++ < 5) logf("ffxDispatch rc=%u", rc);
    }
    for (auto &b : br) std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    cl12->ResourceBarrier(5, br);
    cl12->Close();
    q12->Wait(fence12, v11);
    ID3D12CommandList *lists[] = {cl12};
    q12->ExecuteCommandLists(1, lists);
    UINT64 v12 = ++fval;
    q12->Signal(fence12, v12);
    allocFence[ai] = v12;

    // 4. back to D3D11
    ctx4->Wait(fence11, v12);
    ctx->CopyResource(f.output, sOut.t11);

    if (frameNo == 1) logf("first FSR frame dispatched (rc=%u, depth=%s, mv valid=%d)", rc, dsrv ? "yes" : "NO", valid);
    resetNext = false;
    snapValid = false;
    cbCur ^= 1; cbHave[cbCur] = false;
    g_bs.jx = dd.jitterOffset.x; g_bs.jy = dd.jitterOffset.y; g_bs.dtMs = dtMs;
    g_bs.depth = sDepth.r12; g_bs.mv = sMV.r12; g_bs.rw = rw; g_bs.rh = rh; g_bs.frameNo = frameNo;
    next_jitter();
    return rc == FFX_API_RETURN_OK;
}
