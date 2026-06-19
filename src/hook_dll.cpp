/* hook_dll.cpp — injected DLL that intercepts RegisterHotKey */
#include <windows.h>
#include <cstdio>
#include <cstring>

#pragma comment(linker, "/EXPORT:GetMsgProc=_GetMsgProc@12")

// ============================================================
// x64 inline hook — JMP [rip+0]; dq <addr> (14 bytes)
// ============================================================
static BYTE* InstallHook(void* target, void* hook) {
    DWORD old;
    VirtualProtect(target, 14, PAGE_EXECUTE_READWRITE, &old);
    BYTE* tramp = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(tramp, target, 14); // save original
    // jmp back: FF 25 00 00 00 00 <return_addr>
    tramp[14]=0xFF; tramp[15]=0x25; *(DWORD*)(tramp+16)=0; *(DWORD64*)(tramp+20)=(DWORD64)((BYTE*)target+14);
    // write jump to hook
    BYTE* p = (BYTE*)target;
    p[0]=0xFF; p[1]=0x25; *(DWORD*)(p+2)=0; *(DWORD64*)(p+6)=(DWORD64)hook;
    VirtualProtect(target, 14, old, &old);
    return tramp;
}

// ============================================================
// Pipe communication
// ============================================================
static CRITICAL_SECTION g_cs;
static HANDLE g_pipe = INVALID_HANDLE_VALUE;

struct HotkeyMsg { DWORD pid; DWORD tid; UINT modifiers; UINT vk; int id; char name[64]; };

static void SendMsg(UINT mod, UINT vk, int id) {
    HotkeyMsg m = {};
    m.pid = GetCurrentProcessId(); m.tid = GetCurrentThreadId();
    m.modifiers = mod; m.vk = vk; m.id = id;
    wchar_t path[MAX_PATH]; DWORD len = MAX_PATH;
    if (QueryFullProcessImageNameW(GetCurrentProcess(), 0, path, &len)) {
        wchar_t* fn = wcsrchr(path, L'\\');
        WideCharToMultiByte(CP_UTF8, 0, fn ? fn+1 : path, -1, m.name, 64, nullptr, nullptr);
    }
    EnterCriticalSection(&g_cs);
    if (g_pipe == INVALID_HANDLE_VALUE)
        g_pipe = CreateFileA("\\\\.\\pipe\\HotKeySightPipe", GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (g_pipe != INVALID_HANDLE_VALUE) {
        DWORD w; WriteFile(g_pipe, &m, sizeof(m), &w, nullptr);
    }
    LeaveCriticalSection(&g_cs);
}

// ============================================================
// Hooked RegisterHotKey
// ============================================================
typedef BOOL (WINAPI *RHK_t)(HWND, int, UINT, UINT);
static RHK_t g_Orig = nullptr;

BOOL WINAPI HookedRegisterHotKey(HWND hWnd, int id, UINT fsModifiers, UINT vk) {
    SendMsg(fsModifiers & 0x0F, vk, id);
    return g_Orig(hWnd, id, fsModifiers, vk);
}

// ============================================================
// DllMain
// ============================================================
BOOL WINAPI DllMain(HINSTANCE hDll, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        // Don't hook ourselves
        wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
        if (wcsstr(self, L"hotkey_detector") || wcsstr(self, L"HotKeySight")) return TRUE;
        InitializeCriticalSection(&g_cs);
        DisableThreadLibraryCalls(hDll);
        HMODULE u32 = GetModuleHandleA("user32.dll");
        if (u32) {
            void* rhk = (void*)GetProcAddress(u32, "RegisterHotKey");
            if (rhk) { g_Orig = (RHK_t)InstallHook(rhk, (void*)HookedRegisterHotKey); }
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        DeleteCriticalSection(&g_cs);
        if (g_pipe != INVALID_HANDLE_VALUE) CloseHandle(g_pipe);
    }
    return TRUE;
}

// Export for GetProcAddress by SetWindowsHookEx (not used for CreateRemoteThread injection)
extern "C" __declspec(dllexport) LRESULT CALLBACK GetMsgProc(int c, WPARAM w, LPARAM l) {
    return CallNextHookEx(nullptr, c, w, l);
}
