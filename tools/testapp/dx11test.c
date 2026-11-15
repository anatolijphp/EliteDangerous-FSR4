// Minimal DX11 app mimicking ED pipeline: scene @0.75 res with depth -> compute upscale -> backbuffer.
// Captures one frame itself via RenderDoc in-app API when launched under RenderDoc.
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include "renderdoc_app.h"

static const char *VS =
"cbuffer C:register(b0){float4x4 viewProj;};"
"float4 main(uint id:SV_VertexID):SV_Position{float2 p[3]={float2(-0.6,-0.5),float2(0,0.7),float2(0.6,-0.5)};"
"return mul(viewProj,float4(p[id],0.5,1));}";
static const char *PS = "float4 main(float4 p:SV_Position):SV_Target{return float4(p.x/1440,p.y/810,0.5,1);}";
static const char *CS =
"Texture2D<float4> src:register(t0);SamplerState s:register(s0);RWTexture2D<float4> dst:register(u0);"
"cbuffer c_ffxConst:register(b0){float4 inv;};"
"[numthreads(8,8,1)]void main(uint2 id:SV_DispatchThreadID){dst[id]=src.SampleLevel(s,(id+0.5)*inv.xy,0);}";

static ID3DBlob *compile(const char *src, const char *tgt) {
    ID3DBlob *b = 0, *e = 0;
    if (FAILED(D3DCompile(src, strlen(src), 0, 0, 0, "main", tgt, 0, 0, &b, &e))) {
        printf("compile %s: %s\n", tgt, e ? (char *)ID3D10Blob_GetBufferPointer(e) : "?"); exit(1);
    }
    return b;
}

int main(void) {
    const int W = 1920, H = 1080, LW = 1440, LH = 810;
    if (getenv("EDFSR_PROXY")) LoadLibraryA(getenv("EDFSR_PROXY"));
    RENDERDOC_API_1_1_2 *rdoc = 0;
    HMODULE m = GetModuleHandleA("renderdoc.dll");
    if (m) { pRENDERDOC_GetAPI g = (pRENDERDOC_GetAPI)GetProcAddress(m, "RENDERDOC_GetAPI"); g(eRENDERDOC_API_Version_1_1_2, (void **)&rdoc); }

    HWND hwnd = CreateWindowA("STATIC", "dx11test", WS_OVERLAPPEDWINDOW, 0, 0, W, H, 0, 0, 0, 0);
    DXGI_SWAP_CHAIN_DESC sd = {0};
    sd.BufferCount = 2; sd.BufferDesc.Width = W; sd.BufferDesc.Height = H;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_UNORDERED_ACCESS;
    sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain *sc; ID3D11Device *dev; ID3D11DeviceContext *ctx;
    if (FAILED(D3D11CreateDeviceAndSwapChain(0, D3D_DRIVER_TYPE_HARDWARE, 0, 0, 0, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, 0, &ctx))) { puts("device fail"); return 1; }

    D3D11_TEXTURE2D_DESC td = {0};
    td.Width = LW; td.Height = LH; td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D *scene; ID3D11Device_CreateTexture2D(dev, &td, 0, &scene);
    td.Format = DXGI_FORMAT_D32_FLOAT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ID3D11Texture2D *depth; ID3D11Device_CreateTexture2D(dev, &td, 0, &depth);
    ID3D11RenderTargetView *rtv; ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)scene, 0, &rtv);
    ID3D11DepthStencilView *dsv; ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *)depth, 0, &dsv);
    ID3D11ShaderResourceView *srv; ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)scene, 0, &srv);
    ID3D11Texture2D *bb; IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&bb);
    ID3D11UnorderedAccessView *uav; ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)bb, 0, &uav);

    ID3DBlob *vb = compile(VS, "vs_5_0"), *pb = compile(PS, "ps_5_0"), *cb = compile(CS, "cs_5_0");
    ID3D11VertexShader *vs; ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vb), ID3D10Blob_GetBufferSize(vb), 0, &vs);
    ID3D11PixelShader *ps; ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(pb), ID3D10Blob_GetBufferSize(pb), 0, &ps);
    ID3D11ComputeShader *cs; ID3D11Device_CreateComputeShader(dev, ID3D10Blob_GetBufferPointer(cb), ID3D10Blob_GetBufferSize(cb), 0, &cs);
    float m4[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    D3D11_BUFFER_DESC bd = {64, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA init = {m4, 0, 0};
    ID3D11Buffer *cam; ID3D11Device_CreateBuffer(dev, &bd, &init, &cam);
    float inv[4] = {1.0f / W, 1.0f / H, 0, 0}; bd.ByteWidth = 16; init.pSysMem = inv;
    ID3D11Buffer *ffx; ID3D11Device_CreateBuffer(dev, &bd, &init, &ffx);
    D3D11_SAMPLER_DESC smd = {D3D11_FILTER_MIN_MAG_MIP_LINEAR, 3, 3, 3, 0, 1, D3D11_COMPARISON_NEVER, {0}, 0, D3D11_FLOAT32_MAX};
    ID3D11SamplerState *smp; ID3D11Device_CreateSamplerState(dev, &smd, &smp);
    D3D11_DEPTH_STENCIL_DESC dd = {TRUE, D3D11_DEPTH_WRITE_MASK_ALL, D3D11_COMPARISON_LESS};
    ID3D11DepthStencilState *dss; ID3D11Device_CreateDepthStencilState(dev, &dd, &dss);

    for (int f = 0; f < 3; f++) {
        printf("frame %d ctx vt=%p dispatch=%p cssetshader=%p\n", f, *(void **)ctx, (*(void ***)ctx)[41], (*(void ***)ctx)[69]);
        if (f == 1 && rdoc) rdoc->StartFrameCapture(0, 0);
        float clr[4] = {0.05f, 0.05f, 0.1f, 1};
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, clr);
        ID3D11DeviceContext_ClearDepthStencilView(ctx, dsv, D3D11_CLEAR_DEPTH, 1, 0);
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, dsv);
        ID3D11DeviceContext_OMSetDepthStencilState(ctx, dss, 0);
        D3D11_VIEWPORT vp = {0, 0, LW, LH, 0, 1}; ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(ctx, vs, 0, 0); ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cam);
        ID3D11DeviceContext_PSSetShader(ctx, ps, 0, 0);
        ID3D11DeviceContext_Draw(ctx, 3, 0);
        ID3D11RenderTargetView *nr = 0; ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &nr, 0);
        ID3D11DeviceContext_CSSetShader(ctx, cs, 0, 0); ID3D11DeviceContext_CSSetShaderResources(ctx, 0, 1, &srv);
        ID3D11DeviceContext_CSSetSamplers(ctx, 0, 1, &smp); ID3D11DeviceContext_CSSetConstantBuffers(ctx, 0, 1, &ffx);
        ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &uav, 0);
        ID3D11DeviceContext_Dispatch(ctx, W / 8, H / 8, 1);
        ID3D11ShaderResourceView *ns = 0; ID3D11UnorderedAccessView *nu = 0;
        ID3D11DeviceContext_CSSetShaderResources(ctx, 0, 1, &ns); ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &nu, 0);
        if (f < 2) IDXGISwapChain_Present(sc, 0, 0);
        if (f == 1 && rdoc) rdoc->EndFrameCapture(0, 0);
    }

    { D3D11_TEXTURE2D_DESC sdsc; bb->lpVtbl->GetDesc(bb, &sdsc); sdsc.Usage = D3D11_USAGE_STAGING; sdsc.BindFlags = 0; sdsc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sdsc.MiscFlags = 0;
      ID3D11Texture2D *st; ID3D11Device_CreateTexture2D(dev, &sdsc, 0, &st); ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)bb);
      D3D11_MAPPED_SUBRESOURCE ms; ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &ms);
      unsigned *px = (unsigned *)ms.pData; printf("px(5,5)=%08x px(960,400)=%08x\n", px[5 * (ms.RowPitch / 4) + 5], px[400 * (ms.RowPitch / 4) + 960]);
      ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0); }

    puts(rdoc ? "captured" : "no renderdoc");
    return 0;
}
