// dinput8.dll proxy: loads RenderDoc into Elite Dangerous before D3D11 device creation,
// forwards DirectInput exports to the real System32\dinput8.dll. Diagnostic use only.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdbool.h>
#include "renderdoc_app.h"

static HMODULE real;
static FARPROC get(const char *n) {
    if (!real) {
        wchar_t p[MAX_PATH];
        GetSystemDirectoryW(p, MAX_PATH);
        wcscat(p, L"\\dinput8.dll");
        real = LoadLibraryW(p);
    }
    return real ? GetProcAddress(real, n) : 0;
}

static void logf(const char *msg) {
    FILE *f = fopen("D:\\ed-fsr\\captures\\rdproxy.log", "a");
    if (f) { SYSTEMTIME t; GetLocalTime(&t);
        fprintf(f, "%02d:%02d:%02d pid %lu %s\n", t.wHour, t.wMinute, t.wSecond, GetCurrentProcessId(), msg); fclose(f); }
}

static void load_renderdoc(void) {
    char exe[MAX_PATH]; GetModuleFileNameA(0, exe, MAX_PATH);
    if (!strstr(exe, "EliteDangerous64.exe") && !strstr(exe, "dx11test.exe")) { logf("not game process, skip"); return; }
    if (GetFileAttributesA("D:\\ed-fsr\\captures\\rdproxy.disable") != INVALID_FILE_ATTRIBUTES) { logf("disabled"); return; }
    HMODULE m = LoadLibraryW(L"D:\\tools\\renderdoc\\renderdoc.dll");
    if (!m) { logf("renderdoc.dll load FAILED"); return; }
    pRENDERDOC_GetAPI g = (pRENDERDOC_GetAPI)GetProcAddress(m, "RENDERDOC_GetAPI");
    RENDERDOC_API_1_1_2 *api = 0;
    if (g && g(eRENDERDOC_API_Version_1_1_2, (void **)&api) && api) {
        api->SetCaptureFilePathTemplate("D:\\ed-fsr\\captures\\ed");
        RENDERDOC_InputButton keys[] = {eRENDERDOC_Key_F12, eRENDERDOC_Key_PrtScrn};
        api->SetCaptureKeys(keys, 2);
        logf("renderdoc loaded, API ok");
    } else logf("renderdoc loaded, API FAILED");
}

// ---- Block Epic overlay (EOSOVH) — it hooks DXGI Present and crashes together with RenderDoc.
typedef HMODULE (WINAPI *LLA)(LPCSTR);
typedef HMODULE (WINAPI *LLW)(LPCWSTR);
static LLA prevA; static LLW prevW;
static HMODULE WINAPI hookA(LPCSTR n) {
    if (n && strstr(n, "EOSOVH")) { logf("blocked EOS overlay (A)"); SetLastError(ERROR_MOD_NOT_FOUND); return 0; }
    return prevA(n);
}
static HMODULE WINAPI hookW(LPCWSTR n) {
    if (n && wcsstr(n, L"EOSOVH")) { logf("blocked EOS overlay (W)"); SetLastError(ERROR_MOD_NOT_FOUND); return 0; }
    return prevW(n);
}
static int patch_iat(HMODULE mod) {
    BYTE *b = (BYTE *)mod; int n = 0;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(b + dir.VirtualAddress); d->Name; d++) {
        if (_stricmp((char *)(b + d->Name), "KERNEL32.dll") || !d->OriginalFirstThunk) continue;
        IMAGE_THUNK_DATA *on = (IMAGE_THUNK_DATA *)(b + d->OriginalFirstThunk), *ft = (IMAGE_THUNK_DATA *)(b + d->FirstThunk);
        for (; on->u1.AddressOfData; on++, ft++) {
            if (IMAGE_SNAP_BY_ORDINAL(on->u1.Ordinal)) continue;
            const char *fn = (const char *)((IMAGE_IMPORT_BY_NAME *)(b + on->u1.AddressOfData))->Name;
            void *rep = 0;
            if (!strcmp(fn, "LoadLibraryA")) { prevA = (LLA)ft->u1.Function; rep = hookA; }
            else if (!strcmp(fn, "LoadLibraryW")) { prevW = (LLW)ft->u1.Function; rep = hookW; }
            if (rep) { DWORD old; VirtualProtect(&ft->u1.Function, 8, PAGE_READWRITE, &old);
                       ft->u1.Function = (ULONG_PTR)rep; VirtualProtect(&ft->u1.Function, 8, old, &old); n++; }
        }
    }
    return n;
}
static void block_eos_overlay(void) {
    HMODULE eos = GetModuleHandleA("EOSSDK-Win64-Shipping.dll");
    char m[96]; sprintf(m, "EOSSDK %s, patched %d imports", eos ? "found" : "NOT found", eos ? patch_iat(eos) : 0); logf(m);
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID p) {
    if (r == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); load_renderdoc(); block_eos_overlay(); }
    return TRUE;
}

typedef HRESULT (WINAPI *DI8C)(HINSTANCE, DWORD, REFIID, LPVOID *, void *);
HRESULT WINAPI DirectInput8Create(HINSTANCE a, DWORD b, REFIID c, LPVOID *d, void * e) {
    DI8C f = (DI8C)get("DirectInput8Create"); return f ? f(a, b, c, d, e) : E_FAIL;
}
typedef HRESULT (WINAPI *V)(void);
HRESULT WINAPI DllCanUnloadNow(void) { V f = (V)get("DllCanUnloadNow"); return f ? f() : S_FALSE; }
HRESULT WINAPI DllRegisterServer(void) { V f = (V)get("DllRegisterServer"); return f ? f() : E_FAIL; }
HRESULT WINAPI DllUnregisterServer(void) { V f = (V)get("DllUnregisterServer"); return f ? f() : E_FAIL; }
typedef HRESULT (WINAPI *GCO)(REFCLSID, REFIID, LPVOID *);
HRESULT WINAPI DllGetClassObject(REFCLSID a, REFIID b, LPVOID *c) {
    GCO f = (GCO)get("DllGetClassObject"); return f ? f(a, b, c) : E_FAIL;
}
typedef LPCVOID (WINAPI *GDJ)(void);
LPCVOID WINAPI GetdfDIJoystick(void) { GDJ f = (GDJ)get("GetdfDIJoystick"); return f ? f() : 0; }
