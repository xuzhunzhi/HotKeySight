/*
 * hook_dll.cpp — Injected DLL that intercepts RegisterHotKey
 * Build: g++ -shared -o hook_dll.dll hook_dll.cpp -static -luser32 -O2 -std=c++17
 */
#include <windows.h>
#include <cstdio>
#include <cstring>

// ============================================================
// Minimal x64 inline hook
// ============================================================
// We use a 14-byte trampoline: FF 25 00 00 00 00 <8-byte-addr>
// = jmp qword ptr [rip+0] followed by the absolute target address.

struct HookTrampoline {
    BYTE jmp_rip[6];    // FF 25 00 00 00 00
    DWORD64 target;     // absolute address
    // total 14 bytes
};

static void InstallHook(void* targetFunc, void* hookFunc, BYTE* savedBytes, void** trampolineOut) {
    DWORD oldProtect;
    VirtualProtect(targetFunc, 14, PAGE_EXECUTE_READWRITE, &oldProtect);

    // Save original first 14 bytes
    memcpy(savedBytes, targetFunc, 14);

    // Build trampoline (saved bytes + jmp back to target+14)
    BYTE* tramp = (BYTE*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(tramp, savedBytes, 14);
    // jmp [rip+0]; dq <return_addr>
    tramp[14] = 0xFF; tramp[15] = 0x25;
    tramp[16] = 0x00; tramp[17] = 0x00; tramp[18] = 0x00; tramp[19] = 0x00;
    *(DWORD64*)(tramp + 20) = (DWORD64)((BYTE*)targetFunc + 14);

    // Write hook: jmp [rip+0]; dq <hookFunc>
    HookTrampoline hook;
    hook.jmp_rip[0] = 0xFF; hook.jmp_rip[1] = 0x25;
    hook.jmp_rip[2] = 0x00; hook.jmp_rip[3] = 0x00; hook.jmp_rip[4] = 0x00; hook.jmp_rip[5] = 0x00;
    hook.target = (DWORD64)hookFunc;
    memcpy(targetFunc, &hook, 14);

    VirtualProtect(targetFunc, 14, oldProtect, &oldProtect);
    *trampolineOut = tramp;
}

// ============================================================
// RegisterHotKey signatures
// ============================================================
typedef BOOL (WINAPI *RegisterHotKey_t)(HWND hWnd, int id, UINT fsModifiers, UINT vk);

static RegisterHotKey_t g_OrigRegisterHotKey = nullptr;
static BYTE g_SavedRegHotKey[14];
static void* g_TrampRegHotKey = nullptr;

// ============================================================
// IPC: Send data to main app via named pipe
// ============================================================
static HANDLE g_hPipe = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_cs;

struct HotkeyMsg {
    DWORD pid;
    DWORD tid;
    UINT modifiers;
    UINT vk;
    int id;
    char processName[64];
};

static void SendToMain(HotkeyMsg& msg) {
    EnterCriticalSection(&g_cs);
    if (g_hPipe != INVALID_HANDLE_VALUE) {
        DWORD written;
        if (!WriteFile(g_hPipe, &msg, sizeof(msg), &written, nullptr)) {
            // Pipe broken, close and reconnect
            CloseHandle(g_hPipe);
            g_hPipe = INVALID_HANDLE_VALUE;
        }
    }
    if (g_hPipe == INVALID_HANDLE_VALUE) {
        // Try to reconnect
        g_hPipe = CreateFileA("\\\\.\\pipe\\HotkeyDetectorPipe",
            GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    LeaveCriticalSection(&g_cs);
}

static void FillProcessName(HotkeyMsg& msg) {
    msg.pid = GetCurrentProcessId();
    msg.tid = GetCurrentThreadId();
    // Get process name
    wchar_t path[MAX_PATH];
    DWORD len = MAX_PATH;
    if (QueryFullProcessImageNameW(GetCurrentProcess(), 0, path, &len)) {
        wchar_t* name = wcsrchr(path, L'\\');
        name = name ? name + 1 : path;
        WideCharToMultiByte(CP_UTF8, 0, name, -1, msg.processName, 64, nullptr, nullptr);
    } else {
        strcpy(msg.processName, "unknown");
    }
}

// ============================================================
// Hooked RegisterHotKey
// ============================================================
BOOL WINAPI HookedRegisterHotKey(HWND hWnd, int id, UINT fsModifiers, UINT vk) {
    HotkeyMsg msg = {};
    FillProcessName(msg);
    msg.modifiers = fsModifiers & 0x000F; // strip MOD_NOREPEAT etc.
    msg.vk = vk;
    msg.id = id;
    SendToMain(msg);

    // Call original
    return g_OrigRegisterHotKey(hWnd, id, fsModifiers, vk);
}

// ============================================================
// Hook procedure for SetWindowsHookEx (minimal — real work in DllMain)
// ============================================================
extern "C" __declspec(dllexport) LRESULT CALLBACK GetMsgProc(int nCode, WPARAM wParam, LPARAM lParam) {
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

// ============================================================
// DLL Entry
// ============================================================
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH: {
            // Don't hook our own process
            wchar_t selfPath[MAX_PATH];
            GetModuleFileNameW(nullptr, selfPath, MAX_PATH);
            if (wcsstr(selfPath, L"hotkey_detector") || wcsstr(selfPath, L"HotkeyDetector")) {
                break; // skip self-injection
            }

            InitializeCriticalSection(&g_cs);
            DisableThreadLibraryCalls(hinstDLL);

            // Hook RegisterHotKey in user32.dll
            HMODULE hUser32 = GetModuleHandleA("user32.dll");
            if (hUser32) {
                void* pRegisterHotKey = (void*)GetProcAddress(hUser32, "RegisterHotKey");
                if (pRegisterHotKey) {
                    InstallHook(pRegisterHotKey, (void*)HookedRegisterHotKey,
                        g_SavedRegHotKey, &g_TrampRegHotKey);
                    g_OrigRegisterHotKey = (RegisterHotKey_t)g_TrampRegHotKey;
                }
            }
            break;
        }
        case DLL_PROCESS_DETACH: {
            // Unhook
            if (g_OrigRegisterHotKey && g_TrampRegHotKey) {
                DWORD oldProtect;
                HMODULE hUser32 = GetModuleHandleA("user32.dll");
                void* target = (void*)GetProcAddress(hUser32, "RegisterHotKey");
                VirtualProtect(target, 14, PAGE_EXECUTE_READWRITE, &oldProtect);
                memcpy(target, g_SavedRegHotKey, 14);
                VirtualProtect(target, 14, oldProtect, &oldProtect);
                VirtualFree(g_TrampRegHotKey, 0, MEM_RELEASE);
            }
            if (g_hPipe != INVALID_HANDLE_VALUE) CloseHandle(g_hPipe);
            DeleteCriticalSection(&g_cs);
            break;
        }
    }
    return TRUE;
}
