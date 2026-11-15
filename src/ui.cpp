// ED-FSR in-game settings overlay. See ui.h.
// Dear ImGui 1.91.8 (D:\tools\imgui-1.91.8), Win32 + DX11 backends.
#define DIRECTINPUT_VERSION 0x0800
#include "ui.h"
#include <d3d11_1.h>
#include <dinput.h>
#include <windowsx.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

// ---------------------------------------------------------------- locking
struct Cs {
    CRITICAL_SECTION cs;
    Cs() { InitializeCriticalSection(&cs); }
};
Cs g_cs;    // guards ImGui context / io + mouse emulation state (wndproc, DI poll and render threads)
Cs g_diCs;  // guards DirectInput hook tables
struct Lock {
    CRITICAL_SECTION *c;
    explicit Lock(Cs &x) : c(&x.cs) { EnterCriticalSection(c); }
    ~Lock() { LeaveCriticalSection(c); }
};

// ---------------------------------------------------------------- globals
HWND g_hwnd = nullptr;
WNDPROC g_origProc = nullptr;
ID3D11Device *g_dev = nullptr;
ID3D11DeviceContext *g_ctx = nullptr;
ID3D11DeviceContext1 *g_ctx1 = nullptr;
ID3DDeviceContextState *g_state = nullptr;   // private pipeline state block
bool g_inited = false, g_dead = false, g_gpuReady = false;
volatile bool g_open = false;
bool g_prevHotkey = false, g_prevEsc = false;
ULONGLONG g_toastStart = 0;
bool g_toastDone = false;
float g_scale = 1.0f;
ImFont *g_fontText = nullptr, *g_fontHead = nullptr;

// mouse emulation (client coordinates of g_hwnd)
float g_mx = 0, g_my = 0;
int g_lastMsgX = -100000, g_lastMsgY = -100000;
ULONGLONG g_msgMoveTick = 0, g_rawTick = 0;
bool g_legacySeen = false, g_rawSeen = false;  // which source delivers buttons
BYTE g_diPrevButtons[8] = {};
volatile bool g_kbHold = false;  // after closing by keyboard: keep DI keyboard neutral until all keys released

const float TOAST_SECS = 8.0f;
const ImVec4 C_ORANGE(1.0f, 113 / 255.0f, 0.0f, 1.0f);
const ImVec4 C_ORANGE_LT(1.0f, 0.62f, 0.25f, 1.0f);
const ImVec4 C_TEXT(1.0f, 0.58f, 0.20f, 1.0f);
const ImVec4 C_VALUE(0.93f, 0.93f, 0.93f, 1.0f);
const ImVec4 C_DIM(0.62f, 0.50f, 0.40f, 1.0f);

ImVec4 rgba(float r, float g, float b, float a) { return ImVec4(r, g, b, a); }
ImVec4 orange(float a) { return ImVec4(C_ORANGE.x, C_ORANGE.y, C_ORANGE.z, a); }

bool foreground_is_ours() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

void clamp_mouse() {
    RECT rc;
    if (!g_hwnd || !GetClientRect(g_hwnd, &rc)) return;
    if (g_mx < 0) g_mx = 0;
    if (g_my < 0) g_my = 0;
    if (g_mx > rc.right - 1) g_mx = (float)(rc.right - 1);
    if (g_my > rc.bottom - 1) g_my = (float)(rc.bottom - 1);
}

// relative motion from raw input / DirectInput (only used when WM_MOUSEMOVE does not move)
void add_rel(long dx, long dy) {
    g_mx += (float)dx;
    g_my += (float)dy;
    clamp_mouse();
}

// caller holds g_cs
void set_open(bool o, bool byKeyboard) {
    if (o == g_open) return;
    ImGuiIO &io = ImGui::GetIO();
    io.ClearInputKeys();
    io.ClearInputMouse();
    if (o) {
        g_toastDone = true;
        ClipCursor(nullptr);  // our own import: not patched
        POINT p;
        RECT rc = {};
        GetClientRect(g_hwnd, &rc);
        if (GetCursorPos(&p) && ScreenToClient(g_hwnd, &p) && p.x >= 0 && p.y >= 0 && p.x < rc.right && p.y < rc.bottom) {
            g_mx = (float)p.x; g_my = (float)p.y;
        } else {
            g_mx = rc.right * 0.5f; g_my = rc.bottom * 0.5f;
        }
        g_lastMsgX = g_lastMsgY = -100000;
        g_msgMoveTick = g_rawTick = 0;
        g_legacySeen = g_rawSeen = false;
        memset(g_diPrevButtons, 0, sizeof g_diPrevButtons);
        g_prevEsc = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
        g_open = true;
    } else {
        g_open = false;
        if (byKeyboard) g_kbHold = true;
    }
}

// ---------------------------------------------------------------- IAT hooks (cursor)
typedef BOOL(WINAPI *PFN_SetCursorPos)(int, int);
typedef BOOL(WINAPI *PFN_ClipCursor)(const RECT *);
typedef int(WINAPI *PFN_ShowCursor)(BOOL);
PFN_SetCursorPos g_realSetCursorPos = nullptr;
PFN_ClipCursor g_realClipCursor = nullptr;
PFN_ShowCursor g_realShowCursor = nullptr;

BOOL WINAPI hk_SetCursorPos(int x, int y) {
    if (g_open) return TRUE;
    return g_realSetCursorPos ? g_realSetCursorPos(x, y) : SetCursorPos(x, y);
}
BOOL WINAPI hk_ClipCursor(const RECT *r) {
    if (g_open) return TRUE;
    return g_realClipCursor ? g_realClipCursor(r) : ClipCursor(r);
}
int WINAPI hk_ShowCursor(BOOL show) {
    if (g_open) return show ? 0 : -1;  // ends typical "while (ShowCursor(x) ...)" loops
    return g_realShowCursor ? g_realShowCursor(show) : ShowCursor(show);
}

void *iat_patch(HMODULE mod, const char *dll, const char *fn, void *hook) {
    BYTE *base = (BYTE *)mod;
    if (!base) return nullptr;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size) return nullptr;
    HMODULE hdll = GetModuleHandleA(dll);
    void *real = hdll ? (void *)GetProcAddress(hdll, fn) : nullptr;
    void *orig = nullptr;
    for (IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp((const char *)(base + imp->Name), dll) != 0) continue;
        IMAGE_THUNK_DATA *ft = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        IMAGE_THUNK_DATA *oft = imp->OriginalFirstThunk ? (IMAGE_THUNK_DATA *)(base + imp->OriginalFirstThunk) : nullptr;
        for (; ft->u1.Function; ++ft) {
            bool match;
            if (oft && !IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal)) {
                IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)(base + oft->u1.AddressOfData);
                match = strcmp((const char *)ibn->Name, fn) == 0;
            } else {
                match = real && (void *)ft->u1.Function == real;
            }
            if (oft) ++oft;
            if (!match) continue;
            DWORD old;
            if (!VirtualProtect(&ft->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) continue;
            if ((void *)ft->u1.Function != hook) {
                orig = (void *)ft->u1.Function;
                ft->u1.Function = (ULONG_PTR)hook;
            }
            VirtualProtect(&ft->u1.Function, sizeof(void *), old, &old);
        }
    }
    return orig;
}

// ---------------------------------------------------------------- raw input / DI mouse feeding (caller holds g_cs)
void raw_buttons(USHORT f, USHORT data) {
    ImGuiIO &io = ImGui::GetIO();
    static const USHORT dn[5] = {RI_MOUSE_BUTTON_1_DOWN, RI_MOUSE_BUTTON_2_DOWN, RI_MOUSE_BUTTON_3_DOWN, RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_5_DOWN};
    static const USHORT up[5] = {RI_MOUSE_BUTTON_1_UP, RI_MOUSE_BUTTON_2_UP, RI_MOUSE_BUTTON_3_UP, RI_MOUSE_BUTTON_4_UP, RI_MOUSE_BUTTON_5_UP};
    for (int i = 0; i < 5; i++) {
        if (f & dn[i]) io.AddMouseButtonEvent(i, true);
        if (f & up[i]) io.AddMouseButtonEvent(i, false);
    }
    if (f & RI_MOUSE_WHEEL) io.AddMouseWheelEvent(0.0f, (float)(SHORT)data / (float)WHEEL_DELTA);
    if (f & RI_MOUSE_HWHEEL) io.AddMouseWheelEvent((float)(SHORT)data / (float)WHEEL_DELTA, 0.0f);
}

void process_raw(LPARAM l) {
    alignas(8) BYTE buf[512];
    UINT sz = 0;
    if (GetRawInputData((HRAWINPUT)l, RID_INPUT, nullptr, &sz, sizeof(RAWINPUTHEADER)) != 0 || sz == 0 || sz > sizeof buf) return;
    if (GetRawInputData((HRAWINPUT)l, RID_INPUT, buf, &sz, sizeof(RAWINPUTHEADER)) == (UINT)-1) return;
    RAWINPUT *ri = (RAWINPUT *)buf;
    if (ri->header.dwType != RIM_TYPEMOUSE) return;
    const RAWMOUSE &m = ri->data.mouse;
    Lock lk(g_cs);
    if (!g_open) return;
    ULONGLONG now = GetTickCount64();
    if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE) && (m.lLastX || m.lLastY)) {
        g_rawTick = now;
        if (now - g_msgMoveTick > 100) add_rel(m.lLastX, m.lLastY);  // OS cursor pinned / no WM_MOUSEMOVE
    }
    if (!g_legacySeen && m.usButtonFlags) {
        g_rawSeen = true;
        raw_buttons(m.usButtonFlags, m.usButtonData);
    }
}

// caller holds g_cs
void di_mouse_buttons(const BYTE *b, int n) {
    if (g_legacySeen || g_rawSeen) return;
    ImGuiIO &io = ImGui::GetIO();
    for (int i = 0; i < n && i < 5; i++) {
        bool d = (b[i] & 0x80) != 0;
        if (d != ((g_diPrevButtons[i] & 0x80) != 0)) io.AddMouseButtonEvent(i, d);
        g_diPrevButtons[i] = b[i];
    }
}
bool di_motion_allowed() {
    ULONGLONG now = GetTickCount64();
    return now - g_msgMoveTick > 100 && now - g_rawTick > 100;
}

// ---------------------------------------------------------------- window proc
// returns true if the message is consumed (result in *r)
bool handle_open_msg(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT *r) {
    switch (m) {
    case WM_INPUT:
        process_raw(l);
        *r = DefWindowProcW(h, m, w, l);  // lets the system clean up the raw input handle
        return true;
    case WM_MOUSEMOVE: {
        Lock lk(g_cs);
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        g_legacySeen = true;
        if (x != g_lastMsgX || y != g_lastMsgY) {
            g_lastMsgX = x; g_lastMsgY = y;
            g_mx = (float)x; g_my = (float)y;
            g_msgMoveTick = GetTickCount64();
        }
        *r = 0;
        return true;
    }
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: {
        Lock lk(g_cs);
        g_legacySeen = true;
        ImGui_ImplWin32_WndProcHandler(h, m, w, l);
        *r = (m == WM_XBUTTONDOWN || m == WM_XBUTTONUP || m == WM_XBUTTONDBLCLK) ? TRUE : 0;
        return true;
    }
    case WM_MOUSELEAVE: case WM_MOUSEHOVER:
        *r = 0;
        return true;
    case WM_SYSKEYDOWN:
        if (w == VK_F4) return false;  // keep Alt+F4 working
        // fallthrough
    case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYUP:
    case WM_CHAR: case WM_SYSCHAR: case WM_DEADCHAR: case WM_SYSDEADCHAR: case WM_UNICHAR: {
        Lock lk(g_cs);
        ImGui_ImplWin32_WndProcHandler(h, m, w, l);
        *r = (m == WM_UNICHAR && w == UNICODE_NOCHAR) ? TRUE : 0;
        return true;
    }
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
            SetCursor(nullptr);  // ImGui draws its own cursor
            *r = TRUE;
            return true;
        }
        return false;
    case WM_KILLFOCUS: {
        Lock lk(g_cs);
        ImGui::GetIO().AddFocusEvent(false);
        return false;
    }
    case WM_SETFOCUS: {
        Lock lk(g_cs);
        ImGui::GetIO().AddFocusEvent(true);
        return false;
    }
    }
    return false;
}

LRESULT CALLBACK ui_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g_open && g_inited && !g_dead) {
        LRESULT r = 0;
        if (handle_open_msg(h, m, w, l, &r)) return r;
    }
    return CallWindowProcW(g_origProc, h, m, w, l);
}

// ---------------------------------------------------------------- style / fonts
void apply_style() {
    ImGuiStyle &s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowRounding = s.ChildRounding = s.FrameRounding = s.PopupRounding = 0;
    s.ScrollbarRounding = s.GrabRounding = s.TabRounding = 0;
    s.WindowBorderSize = 1; s.FrameBorderSize = 1; s.PopupBorderSize = 1; s.ChildBorderSize = 1;
    s.WindowPadding = ImVec2(18, 16);
    s.FramePadding = ImVec2(8, 5);
    s.ItemSpacing = ImVec2(10, 8);
    s.ItemInnerSpacing = ImVec2(8, 6);
    s.GrabMinSize = 12;
    s.WindowTitleAlign = ImVec2(0, 0.5f);
    s.SeparatorTextBorderSize = 1;
    s.ScaleAllSizes(g_scale);
    s.WindowBorderSize = s.FrameBorderSize = s.PopupBorderSize = s.ChildBorderSize = 1;  // keep borders 1px

    ImVec4 *c = s.Colors;
    c[ImGuiCol_Text] = C_TEXT;
    c[ImGuiCol_TextDisabled] = rgba(0.45f, 0.36f, 0.30f, 1);
    c[ImGuiCol_WindowBg] = rgba(0, 0, 0, 0.85f);
    c[ImGuiCol_ChildBg] = rgba(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = rgba(0.02f, 0.02f, 0.02f, 0.95f);
    c[ImGuiCol_Border] = orange(0.85f);
    c[ImGuiCol_BorderShadow] = rgba(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = orange(0.08f);
    c[ImGuiCol_FrameBgHovered] = orange(0.22f);
    c[ImGuiCol_FrameBgActive] = orange(0.32f);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBgCollapsed] = orange(0.15f);
    c[ImGuiCol_CheckMark] = C_ORANGE;
    c[ImGuiCol_SliderGrab] = C_ORANGE;
    c[ImGuiCol_SliderGrabActive] = C_ORANGE_LT;
    c[ImGuiCol_Button] = orange(0.16f);
    c[ImGuiCol_ButtonHovered] = orange(0.55f);
    c[ImGuiCol_ButtonActive] = orange(0.85f);
    c[ImGuiCol_Header] = orange(0.14f);
    c[ImGuiCol_HeaderHovered] = orange(0.35f);
    c[ImGuiCol_HeaderActive] = orange(0.55f);
    c[ImGuiCol_Separator] = orange(0.55f);
    c[ImGuiCol_SeparatorHovered] = C_ORANGE_LT;
    c[ImGuiCol_SeparatorActive] = C_ORANGE_LT;
    c[ImGuiCol_ScrollbarBg] = rgba(0, 0, 0, 0.3f);
    c[ImGuiCol_ScrollbarGrab] = orange(0.5f);
    c[ImGuiCol_ScrollbarGrabHovered] = orange(0.75f);
    c[ImGuiCol_ScrollbarGrabActive] = C_ORANGE_LT;
    c[ImGuiCol_ResizeGrip] = orange(0.2f);
    c[ImGuiCol_ResizeGripHovered] = orange(0.6f);
    c[ImGuiCol_ResizeGripActive] = C_ORANGE_LT;
    c[ImGuiCol_NavHighlight] = C_ORANGE_LT;
}

void build_fonts(unsigned h) {
    g_scale = h / 1440.0f;
    if (g_scale < 0.75f) g_scale = 0.75f;
    if (g_scale > 2.5f) g_scale = 2.5f;
    ImGuiIO &io = ImGui::GetIO();
    io.Fonts->Clear();
    g_fontText = g_fontHead = nullptr;
    const char *path = "C:\\Windows\\Fonts\\bahnschrift.ttf";
    DWORD attr = GetFileAttributesA(path);
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        cfg.OversampleV = 1;
        g_fontText = io.Fonts->AddFontFromFileTTF(path, floorf(18.0f * g_scale), &cfg);
        if (g_fontText) g_fontHead = io.Fonts->AddFontFromFileTTF(path, floorf(26.0f * g_scale), &cfg);
    }
    if (!g_fontText || !g_fontHead) {
        io.Fonts->Clear();
        ImFontConfig a;
        a.SizePixels = floorf(13.0f * g_scale);
        g_fontText = io.Fonts->AddFontDefault(&a);
        ImFontConfig b;
        b.SizePixels = floorf(20.0f * g_scale);
        g_fontHead = io.Fonts->AddFontDefault(&b);
    }
    io.FontDefault = g_fontText;
    apply_style();
}

// ---------------------------------------------------------------- drawing
const char *nz(const char *s) { return (s && *s) ? s : "-"; }

void section(const char *label) {
    ImGui::Dummy(ImVec2(0, 4 * g_scale));
    ImGui::TextColored(C_ORANGE, "%s", label);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y - 3 * g_scale), ImVec2(p.x + w, p.y - 3 * g_scale),
                                        ImGui::GetColorU32(orange(0.45f)), 1.0f);
    ImGui::Dummy(ImVec2(0, 1));
}

void info_row(const char *label, const char *fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ImGui::TextColored(C_DIM, "%s", label);
    ImGui::SameLine(150 * g_scale);
    ImGui::TextColored(C_VALUE, "%s", buf);
}

void draw_toast(const UiState &st, float t, unsigned w) {
    float a = 1.0f;
    if (t < 0.5f) a = t / 0.5f;
    else if (t > TOAST_SECS - 1.0f) a = (TOAST_SECS - t) / 1.0f;
    if (a < 0) a = 0;
    if (a > 1) a = 1;
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, a);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * g_scale, 10 * g_scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * g_scale, 3 * g_scale));
    float margin = 32 * g_scale;
    ImGui::SetNextWindowPos(ImVec2((float)w - margin, margin * 2.5f), ImGuiCond_Always, ImVec2(1, 0));
    ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                          ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize |
                          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##edfsr_toast", nullptr, fl)) {
        ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
        ImGui::GetWindowDrawList()->AddRectFilled(wp, ImVec2(wp.x + 4 * g_scale, wp.y + ws.y), ImGui::GetColorU32(C_ORANGE));
        ImGui::PushFont(g_fontHead);
        ImGui::TextColored(C_ORANGE, "ED-FSR");
        ImGui::PopFont();
        ImGui::TextColored(C_VALUE, "Press CTRL+F8 to configure FSR");
        ImGui::TextColored(C_DIM, "%s  |  FG: %s", nz(st.upscalerName), nz(st.fgName));
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
}

// returns true if the panel asked to close
bool draw_panel(UiState &st, unsigned w, unsigned h) {
    bool close = false;
    float pw = 540 * g_scale;
    ImGui::SetNextWindowPos(ImVec2(w * 0.5f, h * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(pw, 0), ImGuiCond_Always);
    ImGuiWindowFlags fl = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
    if (ImGui::Begin("ED-FSR##panel", nullptr, fl)) {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
        // top accent bar
        dl->AddRectFilled(wp, ImVec2(wp.x + ws.x, wp.y + 3 * g_scale), ImGui::GetColorU32(C_ORANGE));

        ImGui::PushFont(g_fontHead);
        ImGui::TextColored(C_ORANGE, "ED-FSR // UPSCALING");
        ImGui::PopFont();
        ImGui::SameLine();
        {
            const char *hint = "CTRL+F8";
            float tw = ImGui::CalcTextSize(hint).x;
            ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - tw);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6 * g_scale);
            ImGui::TextColored(C_DIM, "%s", hint);
        }
        ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddLine(ImVec2(wp.x, p.y), ImVec2(wp.x + ws.x, p.y), ImGui::GetColorU32(C_ORANGE), 1.0f);
        ImGui::Dummy(ImVec2(0, 2 * g_scale));

        // --- upscaler
        section("UPSCALER");
        {
            ImGui::BeginDisabled(!st.fsr4Available);
            if (ImGui::RadioButton("FSR 4 (ML)", st.upscaler == 1)) { st.upscaler = 1; st.dirty = true; }
            ImGui::EndDisabled();
            ImGui::SameLine(0, 24 * g_scale);
            if (ImGui::RadioButton("FSR 3.1", st.upscaler == 2)) { st.upscaler = 2; st.dirty = true; }
            ImGui::SameLine(0, 24 * g_scale);
            if (ImGui::RadioButton("GAME FSR1", st.upscaler == 0)) { st.upscaler = 0; st.dirty = true; }
            if (!st.fsr4Available) ImGui::TextColored(C_DIM, "FSR 4 REQUIRES AN RDNA 4 GPU / DRIVER SUPPORT");
        }

        section("SHARPNESS");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderFloat("##sharp", &st.sharpness, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp)) st.dirty = true;
        ImGui::TextUnformatted("QUALITY MODE");
        ImGui::SameLine(160 * g_scale);
        ImGui::SetNextItemWidth(-FLT_MIN);
        {
            static const char *kModes[] = {"GAME SETTING", "ULTRA QUALITY (77%)", "QUALITY (67%)", "BALANCED (59%)",
                                           "PERFORMANCE (50%)", "ULTRA PERFORMANCE (33%)", "NATIVE AA (100%)"};
            if (ImGui::Combo("##qmode", &st.qualityMode, kModes, 7)) st.dirty = true;
            if (st.qualityMode != st.qualityModeActive)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "SAVE AND RESTART THE GAME TO APPLY");
                ImGui::TextColored(ImVec4(0.75f, 0.75f, 0.75f, 1.0f), "LIVE: GAME OPTIONS > GRAPHICS > SUPERSAMPLING");
            }
        }

        section("FRAME GENERATION");
        if (ImGui::Checkbox("ENABLE FRAME GENERATION", &st.framegen)) st.dirty = true;
        if (!st.framegenAvailable) {
            ImGui::PushStyleColor(ImGuiCol_Text, C_DIM);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted("Not active in this session: restart the game with frame generation enabled "
                                   "(the setting is saved for the next start).");
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }

        ImGui::Dummy(ImVec2(0, 4 * g_scale));
        ImGui::PushStyleColor(ImGuiCol_Text, C_ORANGE);
        bool adv = ImGui::CollapsingHeader("ADVANCED");
        ImGui::PopStyleColor();
        if (adv) {
            ImGui::Indent(8 * g_scale);
            if (ImGui::Checkbox("CAMERA JITTER", &st.jitter)) st.dirty = true;
            ImGui::SameLine(250 * g_scale);
            if (ImGui::Checkbox("MOTION VECTORS", &st.motionVectors)) st.dirty = true;
            ImGui::TextUnformatted("COCKPIT DEPTH");
            ImGui::SameLine(160 * g_scale);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::SliderFloat("##cockpit", &st.cockpitDepth, 0.0f, 20.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp)) st.dirty = true;
            if (ImGui::Checkbox("HUD / TRANSPARENCY MASK", &st.reactive)) st.dirty = true;
            ImGui::TextUnformatted("MASK STRENGTH");
            ImGui::SameLine(160 * g_scale);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::SliderFloat("##rscale", &st.reactiveScale, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp)) st.dirty = true;
            if (ImGui::Checkbox("FSR DEBUG VIEW", &st.debugView)) st.dirty = true;
            ImGui::Unindent(8 * g_scale);
        }

        section("STATUS");
        info_row("RESOLUTION", "%ux%u  ->  %ux%u", st.renderW, st.renderH, st.outW, st.outH);
        info_row("UPSCALER", "%s", nz(st.upscalerName));
        info_row("FRAME GEN", "%s", nz(st.fgName));
        info_row("FPS", "%.1f", st.fps);

        ImGui::Dummy(ImVec2(0, 6 * g_scale));
        float bw = 130 * g_scale, sp = ImGui::GetStyle().ItemSpacing.x;
        ImGui::TextColored(C_DIM, "ESC TO CLOSE");
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 2 * bw - sp);
        ImGui::PushStyleColor(ImGuiCol_Text, rgba(1.0f, 0.93f, 0.85f, 1));
        if (ImGui::Button("SAVE", ImVec2(bw, 0))) st.saveRequested = true;
        ImGui::SameLine();
        if (ImGui::Button("CLOSE", ImVec2(bw, 0))) close = true;
        ImGui::PopStyleColor();
    }
    ImGui::End();
    return close;
}

void render_draw_data(ID3D11RenderTargetView *rtv) {
    ImDrawData *dd = ImGui::GetDrawData();
    if (!dd || dd->CmdListsCount == 0) return;
    if (g_ctx1 && g_state) {
        ID3DDeviceContextState *prev = nullptr;
        g_ctx1->SwapDeviceContextState(g_state, &prev);
        g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(dd);
        g_ctx->ClearState();  // drop our references (RTV etc.) from the private state block
        g_ctx1->SwapDeviceContextState(prev, nullptr);
        if (prev) prev->Release();
    } else {
        // fallback: ImGui's backend saves/restores everything it touches except OM render targets
        ID3D11RenderTargetView *oldRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
        ID3D11DepthStencilView *oldDsv = nullptr;
        g_ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, &oldDsv);
        g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(dd);
        g_ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, oldDsv);
        for (auto *v : oldRtv) if (v) v->Release();
        if (oldDsv) oldDsv->Release();
    }
}

// ---------------------------------------------------------------- DirectInput hooks
typedef HRESULT(WINAPI *PFN_DI_CreateDevice)(void *self, REFGUID g, void **out, IUnknown *outer);
typedef HRESULT(WINAPI *PFN_DID_GetCaps)(void *self, DIDEVCAPS *caps);
typedef HRESULT(WINAPI *PFN_DID_GetState)(void *self, DWORD cb, void *data);
typedef HRESULT(WINAPI *PFN_DID_GetData)(void *self, DWORD cbObj, DIDEVICEOBJECTDATA *rg, DWORD *inout, DWORD flags);

struct DiVt { void **vt; PFN_DI_CreateDevice create; };
struct DevVt { void **vt; PFN_DID_GetState state; PFN_DID_GetData data; };
struct DevType { void *dev; int type; };
DiVt g_diVt[4];
int g_diVtN = 0;
DevVt g_devVt[8];
int g_devVtN = 0;
DevType g_devs[64];
int g_devsN = 0;
enum { DEV_OTHER = 0, DEV_MOUSE = 1, DEV_KEYBOARD = 2 };

bool patch_slot(void **vt, int idx, void *hook) {
    DWORD old;
    if (!VirtualProtect(&vt[idx], sizeof(void *), PAGE_READWRITE, &old)) return false;
    vt[idx] = hook;
    VirtualProtect(&vt[idx], sizeof(void *), old, &old);
    return true;
}

int dev_type(void *dev) {
    Lock lk(g_diCs);
    for (int i = 0; i < g_devsN; i++)
        if (g_devs[i].dev == dev) return g_devs[i].type;
    return DEV_OTHER;
}

HRESULT WINAPI hk_GetDeviceState(void *self, DWORD cb, void *data) {
    PFN_DID_GetState orig = nullptr;
    {
        Lock lk(g_diCs);
        void **vt = *(void ***)self;
        for (int i = 0; i < g_devVtN; i++)
            if (g_devVt[i].vt == vt) orig = g_devVt[i].state;
    }
    if (!orig) return E_FAIL;
    HRESULT hr = orig(self, cb, data);
    if (!data || !cb) return hr;
    bool open = g_open;
    if (!open && !g_kbHold) return hr;
    int type = dev_type(self);
    if (open) {
        if (type == DEV_MOUSE) {
            if (SUCCEEDED(hr) && cb >= sizeof(DIMOUSESTATE) && g_inited) {
                const DIMOUSESTATE *ms = (const DIMOUSESTATE *)data;
                Lock lk(g_cs);
                if (g_open) {
                    if ((ms->lX || ms->lY) && di_motion_allowed()) add_rel(ms->lX, ms->lY);
                    if (ms->lZ && !g_legacySeen && !g_rawSeen) ImGui::GetIO().AddMouseWheelEvent(0, ms->lZ / (float)WHEEL_DELTA);
                    di_mouse_buttons(ms->rgbButtons, cb >= sizeof(DIMOUSESTATE2) ? 8 : 4);
                }
            }
            memset(data, 0, cb);
        } else if (type == DEV_KEYBOARD) {
            memset(data, 0, cb);
        }
    } else if (type == DEV_KEYBOARD && SUCCEEDED(hr)) {
        bool any = false;
        for (DWORD i = 0; i < cb; i++)
            if (((BYTE *)data)[i] & 0x80) { any = true; break; }
        if (any) memset(data, 0, cb);
        else g_kbHold = false;
    }
    return hr;
}

HRESULT WINAPI hk_GetDeviceData(void *self, DWORD cbObj, DIDEVICEOBJECTDATA *rg, DWORD *inout, DWORD flags) {
    PFN_DID_GetData orig = nullptr;
    {
        Lock lk(g_diCs);
        void **vt = *(void ***)self;
        for (int i = 0; i < g_devVtN; i++)
            if (g_devVt[i].vt == vt) orig = g_devVt[i].data;
    }
    if (!orig) return E_FAIL;
    HRESULT hr = orig(self, cbObj, rg, inout, flags);
    if (!g_open || !inout) return hr;
    int type = dev_type(self);
    if (type != DEV_MOUSE && type != DEV_KEYBOARD) return hr;
    if (type == DEV_MOUSE && SUCCEEDED(hr) && rg && cbObj >= sizeof(DWORD) * 2 && !(flags & DIGDD_PEEK) && g_inited) {
        Lock lk(g_cs);
        if (g_open) {
            long dx = 0, dy = 0;
            for (DWORD i = 0; i < *inout; i++) {
                const DIDEVICEOBJECTDATA *d = (const DIDEVICEOBJECTDATA *)((const BYTE *)rg + (size_t)i * cbObj);
                if (d->dwOfs == DIMOFS_X) dx += (LONG)d->dwData;
                else if (d->dwOfs == DIMOFS_Y) dy += (LONG)d->dwData;
                else if (d->dwOfs == DIMOFS_Z) {
                    if (!g_legacySeen && !g_rawSeen) ImGui::GetIO().AddMouseWheelEvent(0, (LONG)d->dwData / (float)WHEEL_DELTA);
                } else if (d->dwOfs >= DIMOFS_BUTTON0 && d->dwOfs <= DIMOFS_BUTTON4) {
                    BYTE b[5];
                    memcpy(b, g_diPrevButtons, 5);
                    b[d->dwOfs - DIMOFS_BUTTON0] = (BYTE)d->dwData;
                    di_mouse_buttons(b, 5);
                }
            }
            if ((dx || dy) && di_motion_allowed()) add_rel(dx, dy);
        }
    }
    *inout = 0;  // game sees an empty buffer (events were drained)
    return hr;
}

void hook_device(void *dev) {
    void **vt = *(void ***)dev;
    Lock lk(g_diCs);
    for (int i = 0; i < g_devVtN; i++)
        if (g_devVt[i].vt == vt) return;
    if (g_devVtN >= (int)(sizeof g_devVt / sizeof g_devVt[0])) return;
    if ((void *)vt[9] == (void *)hk_GetDeviceState || (void *)vt[10] == (void *)hk_GetDeviceData) return;
    DevVt &e = g_devVt[g_devVtN];
    e.vt = vt;
    e.state = (PFN_DID_GetState)vt[9];
    e.data = (PFN_DID_GetData)vt[10];
    g_devVtN++;  // publish originals before patching
    patch_slot(vt, 9, (void *)hk_GetDeviceState);
    patch_slot(vt, 10, (void *)hk_GetDeviceData);
}

const GUID kSysMouse = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
const GUID kSysKeyboard = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

HRESULT WINAPI hk_CreateDevice(void *self, REFGUID g, void **out, IUnknown *outer) {
    PFN_DI_CreateDevice orig = nullptr;
    {
        Lock lk(g_diCs);
        void **vt = *(void ***)self;
        for (int i = 0; i < g_diVtN; i++)
            if (g_diVt[i].vt == vt) orig = g_diVt[i].create;
    }
    if (!orig) return E_FAIL;
    HRESULT hr = orig(self, g, out, outer);
    if (FAILED(hr) || !out || !*out) return hr;
    void *dev = *out;
    int type = DEV_OTHER;
    if (IsEqualGUID(g, kSysMouse)) type = DEV_MOUSE;
    else if (IsEqualGUID(g, kSysKeyboard)) type = DEV_KEYBOARD;
    else {
        DIDEVCAPS caps;
        memset(&caps, 0, sizeof caps);
        caps.dwSize = sizeof caps;
        PFN_DID_GetCaps gc = (PFN_DID_GetCaps)(*(void ***)dev)[3];
        if (SUCCEEDED(gc(dev, &caps))) {
            BYTE t = GET_DIDEVICE_TYPE(caps.dwDevType);
            if (t == DI8DEVTYPE_MOUSE) type = DEV_MOUSE;
            else if (t == DI8DEVTYPE_KEYBOARD) type = DEV_KEYBOARD;
        }
    }
    {
        Lock lk(g_diCs);
        int slot = -1;
        for (int i = 0; i < g_devsN; i++)
            if (g_devs[i].dev == dev) slot = i;
        if (slot < 0 && g_devsN < (int)(sizeof g_devs / sizeof g_devs[0])) slot = g_devsN++;
        if (slot >= 0) { g_devs[slot].dev = dev; g_devs[slot].type = type; }
    }
    hook_device(dev);
    return hr;
}

}  // namespace

// ================================================================ public API
bool ui_init(HWND gameWindow, ID3D11Device *dev, ID3D11DeviceContext *ctx) {
    if (g_inited) return true;
    if (!gameWindow || !dev || !ctx) return false;
    Lock lk(g_cs);
    g_hwnd = gameWindow;
    g_dev = dev; g_dev->AddRef();
    g_ctx = ctx; g_ctx->AddRef();

    // NOTE: SwapDeviceContextState is deliberately NOT used: the D3D11 runtime rewrites the immediate context's
    // vtable slots on every state swap, which removes the mod's CSSetShader/OM/RS hooks (FSR stops working).
    // The fallback path (ImGui's own state backup + OM render targets) is used instead.
    ID3D11Device1 *dev1 = nullptr;
    if (false && SUCCEEDED(dev->QueryInterface(__uuidof(ID3D11Device1), (void **)&dev1)) && dev1) {
        if (SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void **)&g_ctx1)) && g_ctx1) {
            D3D_FEATURE_LEVEL fl = dev->GetFeatureLevel();
            UINT flags = (dev->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
            if (FAILED(dev1->CreateDeviceContextState(flags, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, &g_state)))
                g_state = nullptr;
        }
        dev1->Release();
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange | ImGuiConfigFlags_NoMouse;
    if (!ImGui_ImplWin32_Init(gameWindow) || !ImGui_ImplDX11_Init(dev, ctx)) {
        g_dead = true;
        return false;
    }

    // cursor manipulation by the game is neutralized while the panel is open
    HMODULE exe = GetModuleHandleW(nullptr);
    HMODULE u32 = GetModuleHandleA("user32.dll");
    g_realSetCursorPos = (PFN_SetCursorPos)GetProcAddress(u32, "SetCursorPos");
    g_realClipCursor = (PFN_ClipCursor)GetProcAddress(u32, "ClipCursor");
    g_realShowCursor = (PFN_ShowCursor)GetProcAddress(u32, "ShowCursor");
    if (void *o = iat_patch(exe, "user32.dll", "SetCursorPos", (void *)hk_SetCursorPos)) g_realSetCursorPos = (PFN_SetCursorPos)o;
    if (void *o = iat_patch(exe, "user32.dll", "ClipCursor", (void *)hk_ClipCursor)) g_realClipCursor = (PFN_ClipCursor)o;
    if (void *o = iat_patch(exe, "user32.dll", "ShowCursor", (void *)hk_ShowCursor)) g_realShowCursor = (PFN_ShowCursor)o;

    g_origProc = (WNDPROC)SetWindowLongPtrW(gameWindow, GWLP_WNDPROC, (LONG_PTR)ui_wndproc);
    if (!g_origProc) {
        g_dead = true;
        return false;
    }
    g_inited = true;

    char env[8];
    if (GetEnvironmentVariableA("EDFSR_UI_OPEN", env, sizeof env) && env[0] == '1') set_open(true, false);
    return true;
}

void ui_frame(ID3D11RenderTargetView *rtv, unsigned w, unsigned h, UiState &st) {
    st.dirty = false;
    st.saveRequested = false;
    if (!g_inited || g_dead || !rtv || !w || !h) return;

    // hotkeys (edge detected, only while one of our windows has focus)
    bool hk = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_F8) & 0x8000);
    bool hkEdge = hk && !g_prevHotkey;
    g_prevHotkey = hk;
    if (hkEdge && foreground_is_ours()) {
        Lock lk(g_cs);
        set_open(!g_open, true);
    }
    if (g_open) {
        bool esc = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
        bool escEdge = esc && !g_prevEsc;
        g_prevEsc = esc;
        if (escEdge && foreground_is_ours()) {
            Lock lk(g_cs);
            set_open(false, true);
        }
    }

    ULONGLONG now = GetTickCount64();
    if (!g_toastStart) g_toastStart = now;
    float t = (now - g_toastStart) / 1000.0f;
    if (!g_toastDone && t >= TOAST_SECS) g_toastDone = true;
    bool open = g_open;
    if (!open && g_toastDone) return;  // nothing to draw: no D3D calls

    if (g_dev->GetDeviceRemovedReason() != S_OK) {
        g_dead = true;
        return;
    }

    Lock lk(g_cs);
    ImGuiIO &io = ImGui::GetIO();
    if (!g_gpuReady) {
        build_fonts(h);
        if (!ImGui_ImplDX11_CreateDeviceObjects() || !io.Fonts->TexID) {
            g_dead = true;
            return;
        }
        g_gpuReady = true;
    }

    if (open) io.ConfigFlags &= ~(ImGuiConfigFlags_NoMouseCursorChange | ImGuiConfigFlags_NoMouse);
    else io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange | ImGuiConfigFlags_NoMouse;
    io.MouseDrawCursor = open;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    io.DisplaySize = ImVec2((float)w, (float)h);
    if (open) {
        RECT rc = {};
        GetClientRect(g_hwnd, &rc);
        float sx = rc.right > 0 ? (float)w / rc.right : 1.0f;
        float sy = rc.bottom > 0 ? (float)h / rc.bottom : 1.0f;
        io.AddMousePosEvent(g_mx * sx, g_my * sy);
    } else {
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }

    ImGui::NewFrame();
    bool closeReq = false;
    if (open) {
        closeReq = draw_panel(st, w, h);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            closeReq = true;
            g_kbHold = true;
        }
    } else {
        draw_toast(st, t, w);
    }
    ImGui::Render();
    render_draw_data(rtv);
    if (closeReq) set_open(false, g_kbHold);
}

bool ui_is_open() { return g_open; }

void ui_on_resize() {
    // Nothing size-dependent is cached: the RTV is passed every frame and is unbound (ClearState on the private
    // state block / restore of the game's OM targets) before ui_frame returns, so ResizeBuffers can succeed.
    Lock lk(g_cs);
    if (g_inited && g_open) clamp_mouse();
}

void ui_hook_dinput(void *pDirectInput8) {
    if (!pDirectInput8) return;
    void **vt = *(void ***)pDirectInput8;
    Lock lk(g_diCs);
    for (int i = 0; i < g_diVtN; i++)
        if (g_diVt[i].vt == vt) return;
    if (g_diVtN >= (int)(sizeof g_diVt / sizeof g_diVt[0])) return;
    if ((void *)vt[3] == (void *)hk_CreateDevice) return;
    g_diVt[g_diVtN].vt = vt;
    g_diVt[g_diVtN].create = (PFN_DI_CreateDevice)vt[3];
    g_diVtN++;
    patch_slot(vt, 3, (void *)hk_CreateDevice);
}
