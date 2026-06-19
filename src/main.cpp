/*
 * Windows Global Hotkey Detector — C++ version
 * Detects occupied global hotkeys via brute-force RegisterHotKey enumeration.
 * Maps hotkeys to processes via WH_KEYBOARD_LL hook monitoring.
 * Reads IME/accessibility hotkeys from registry.
 *
 * Build:  g++ -o hotkey-detector.exe main.cpp -static -luser32 -lgdi32 -mwindows
 *         (add -DCONSOLE_MODE for console output instead of GUI)
 */

#ifndef UNICODE
#define UNICODE
#endif

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>
#include <map>
#include <set>
#include <algorithm>
#include <cstdint>

// ============================================================
// Constants — Windows already defines MOD_ALT/MOD_CONTROL/MOD_SHIFT/MOD_WIN
// as macros, so we undef them and use constexpr for type safety.
// ============================================================
#undef MOD_ALT
#undef MOD_CONTROL
#undef MOD_SHIFT
#undef MOD_WIN
#undef MOD_NOREPEAT
constexpr UINT MOD_ALT      = 0x0001;
constexpr UINT MOD_CONTROL  = 0x0002;
constexpr UINT MOD_SHIFT    = 0x0004;
constexpr UINT MOD_WIN      = 0x0008;
constexpr UINT MOD_NOREPEAT = 0x4000;

// ============================================================
// Hotkey combo structure
// ============================================================
struct HotkeyCombo {
    UINT modifiers;
    UINT vk;
    bool operator<(const HotkeyCombo& other) const {
        if (modifiers != other.modifiers) return modifiers < other.modifiers;
        return vk < other.vk;
    }
    bool operator==(const HotkeyCombo& other) const {
        return modifiers == other.modifiers && vk == other.vk;
    }
};

struct OccupiedHotkey {
    HotkeyCombo combo;
    DWORD lastError;
    std::string ownerProcess; // resolved via monitoring or registry
};

// ============================================================
// Virtual Key -> Name mapping
// ============================================================
const wchar_t* VkToString(UINT vk) {
    switch (vk) {
        case 0x08: return L"Backspace";
        case 0x09: return L"Tab";
        case 0x0D: return L"Enter";
        case 0x13: return L"Pause";
        case 0x1B: return L"Esc";
        case 0x20: return L"Space";
        case 0x21: return L"PageUp";
        case 0x22: return L"PageDown";
        case 0x23: return L"End";
        case 0x24: return L"Home";
        case 0x25: return L"Left";
        case 0x26: return L"Up";
        case 0x27: return L"Right";
        case 0x28: return L"Down";
        case 0x2C: return L"PrintScreen";
        case 0x2D: return L"Insert";
        case 0x2E: return L"Delete";
        case 0x5B: return L"LWin";
        case 0x5C: return L"RWin";
        case 0x5D: return L"Apps";
        case 0x6F: return L"Num/";
        case 0x6A: return L"Num*";
        case 0x6B: return L"Num+";
        case 0x6D: return L"Num-";
        case 0x6E: return L"Num.";
        case 0x90: return L"NumLock";
        case 0x91: return L"ScrollLock";
        case 0xAD: return L"VolMute";
        case 0xAE: return L"VolDown";
        case 0xAF: return L"VolUp";
        case 0xB0: return L"MediaNext";
        case 0xB1: return L"MediaPrev";
        case 0xB2: return L"MediaStop";
        case 0xB3: return L"MediaPlayPause";
        case 0xB4: return L"Mail";
        case 0xB5: return L"MediaSelect";
        case 0xBA: return L";";
        case 0xBB: return L"=";
        case 0xBC: return L",";
        case 0xBD: return L"-";
        case 0xBE: return L".";
        case 0xBF: return L"/";
        case 0xC0: return L"`";
        case 0xDB: return L"[";
        case 0xDC: return L"\\";
        case 0xDD: return L"]";
        case 0xDE: return L"'";
        default: break;
    }
    if (vk >= 0x30 && vk <= 0x39) { static wchar_t buf[2]; buf[0] = L'0' + (vk - 0x30); buf[1] = 0; return buf; }
    if (vk >= 0x41 && vk <= 0x5A) { static wchar_t buf[2]; buf[0] = L'A' + (vk - 0x41); buf[1] = 0; return buf; }
    if (vk >= 0x70 && vk <= 0x7B) { static wchar_t buf[4]; swprintf(buf, 4, L"F%d", vk - 0x70 + 1); return buf; }
    if (vk >= 0x60 && vk <= 0x69) { static wchar_t buf[6]; swprintf(buf, 6, L"Num%d", vk - 0x60); return buf; }
    static wchar_t buf[16];
    swprintf(buf, 16, L"VK_0x%02X", vk);
    return buf;
}

std::wstring ModToWString(UINT mod) {
    std::wstring s;
    if (mod & MOD_CONTROL) s += L"Ctrl+";
    if (mod & MOD_ALT)     s += L"Alt+";
    if (mod & MOD_SHIFT)   s += L"Shift+";
    if (mod & MOD_WIN)     s += L"Win+";
    return s;
}

std::string WstrToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string result(len - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &result[0], len, nullptr, nullptr);
    return result;
}

void PrintCombo(const HotkeyCombo& c) {
    std::wstring ws = ModToWString(c.modifiers) + VkToString(c.vk);
    printf("%-30s", WstrToUtf8(ws).c_str());
}

// ============================================================
// Hidden window for RegisterHotKey
// ============================================================
static HWND g_hWnd = nullptr;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_HOTKEY) {
        // We register hotkeys only for scanning, not actual use
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

bool CreateHiddenWindow(HINSTANCE hInstance) {
    const wchar_t CLASS_NAME[] = L"HotkeyDetectorClass";

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;

    RegisterClassExW(&wc);

    g_hWnd = CreateWindowExW(0, CLASS_NAME, L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, hInstance, nullptr);

    if (!g_hWnd) {
        printf("[ERROR] CreateWindowEx failed: %lu\n", GetLastError());
        return false;
    }

    printf("[INFO] Message window created: 0x%p\n", g_hWnd);
    return true;
}

// ============================================================
// Brute-force hotkey scanner
// ============================================================
struct ScanResult {
    std::vector<OccupiedHotkey> occupied;
    int tried = 0;
    int skipped = 0;
};

ScanResult BruteForceScan() {
    ScanResult result;

    // All 16 modifier combinations
    UINT modCombos[] = {
        0,
        MOD_ALT, MOD_CONTROL, MOD_SHIFT, MOD_WIN,
        MOD_ALT | MOD_CONTROL, MOD_ALT | MOD_SHIFT, MOD_ALT | MOD_WIN,
        MOD_CONTROL | MOD_SHIFT, MOD_CONTROL | MOD_WIN, MOD_SHIFT | MOD_WIN,
        MOD_ALT | MOD_CONTROL | MOD_SHIFT, MOD_ALT | MOD_CONTROL | MOD_WIN,
        MOD_ALT | MOD_SHIFT | MOD_WIN, MOD_CONTROL | MOD_SHIFT | MOD_WIN,
        MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN
    };
    constexpr int NUM_MODS = sizeof(modCombos) / sizeof(modCombos[0]);

    // Collect virtual keys to test
    std::vector<UINT> vkList;
    for (UINT vk = 0x30; vk <= 0x39; vk++) vkList.push_back(vk); // 0-9
    for (UINT vk = 0x41; vk <= 0x5A; vk++) vkList.push_back(vk); // A-Z
    for (UINT vk = 0x70; vk <= 0x7B; vk++) vkList.push_back(vk); // F1-F12
    for (UINT vk = 0x60; vk <= 0x69; vk++) vkList.push_back(vk); // Numpad 0-9

    UINT specialKeys[] = {
        0x08, 0x09, 0x0D, 0x13, 0x1B, 0x20, 0x21, 0x22, 0x23, 0x24,
        0x25, 0x26, 0x27, 0x28, 0x2C, 0x2D, 0x2E,
        0x6F, 0x6A, 0x6B, 0x6D, 0x6E,
        0x90, 0x91,
        0xAD, 0xAE, 0xAF, 0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7,
        0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF, 0xC0,
        0xDB, 0xDC, 0xDD, 0xDE
    };
    for (UINT k : specialKeys) vkList.push_back(k);

    printf("[SCAN] Starting brute-force: %d x %zu = %zu combinations...\n",
        NUM_MODS, vkList.size(), (size_t)NUM_MODS * vkList.size());

    int idCounter = 1;
    DWORD startTick = GetTickCount();

    for (UINT mod : modCombos) {
        for (UINT vk : vkList) {
            // Skip modifier-maps-to-itself combos
            if ((mod & MOD_WIN) && (vk == 0x5B || vk == 0x5C)) { result.skipped++; continue; }
            if ((mod & MOD_ALT) && (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU)) { result.skipped++; continue; }
            if ((mod & MOD_CONTROL) && (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL)) { result.skipped++; continue; }
            if ((mod & MOD_SHIFT) && (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT)) { result.skipped++; continue; }

            // Also skip LWin/RWin/LAlt/RAlt/etc. with their own modifiers (already covered)
            if ((mod & MOD_WIN) && vk == VK_LWIN) { result.skipped++; continue; }
            if ((mod & MOD_WIN) && vk == VK_RWIN) { result.skipped++; continue; }

            result.tried++;
            int id = idCounter++;

            if (id > 0xBFFF) {
                printf("[WARN] Max hotkey ID reached\n");
                goto done;
            }

            BOOL ok = RegisterHotKey(g_hWnd, id, mod | MOD_NOREPEAT, vk);
            DWORD err = ok ? 0 : GetLastError();

            if (ok) {
                UnregisterHotKey(g_hWnd, id);
            } else {
                OccupiedHotkey oh;
                oh.combo = {mod, vk};
                oh.lastError = err;
                result.occupied.push_back(oh);
            }
        }
    }

done:
    DWORD elapsed = GetTickCount() - startTick;
    printf("[SCAN] Done! %d tried, %d skipped, %zu occupied (%.1fs)\n",
        result.tried, result.skipped, result.occupied.size(), elapsed / 1000.0);

    return result;
}

// ============================================================
// Registry: Read IME and Accessibility hotkeys
// ============================================================
std::map<HotkeyCombo, std::string> ReadRegistryHotkeys() {
    std::map<HotkeyCombo, std::string> result;

    // IME hotkeys
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Control Panel\\Input Method\\Hot Keys", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {

        DWORD index = 0;
        wchar_t subkeyName[64];
        DWORD nameLen = 64;

        while (RegEnumKeyExW(hKey, index++, subkeyName, &nameLen, nullptr,
            nullptr, nullptr, nullptr) == ERROR_SUCCESS) {

            HKEY hSubKey;
            if (RegOpenKeyExW(hKey, subkeyName, 0, KEY_READ, &hSubKey) == ERROR_SUCCESS) {
                DWORD keyModifiers[4] = {0};
                DWORD virtualKey[4] = {0};
                DWORD modSize = sizeof(keyModifiers);
                DWORD vkSize = sizeof(virtualKey);

                RegQueryValueExW(hSubKey, L"Key Modifiers", nullptr, nullptr,
                    (LPBYTE)keyModifiers, &modSize);
                RegQueryValueExW(hSubKey, L"Virtual Key", nullptr, nullptr,
                    (LPBYTE)virtualKey, &vkSize);

                if (keyModifiers[0] != 0 || virtualKey[0] != 0) {
                    HotkeyCombo hk;
                    hk.modifiers = (UINT)keyModifiers[0];
                    hk.vk = (UINT)virtualKey[0];
                    result[hk] = "IME (输入法)";
                }
                RegCloseKey(hSubKey);
            }
            nameLen = 64;
        }
        RegCloseKey(hKey);
    }

    // Accessibility hotkeys (StickyKeys, FilterKeys, ToggleKeys)
    const wchar_t* accKeys[] = {L"StickyKeys", L"FilterKeys", L"ToggleKeys"};
    for (const wchar_t* accName : accKeys) {
        wchar_t path[128];
        swprintf(path, 128, L"Control Panel\\Accessibility\\%s", accName);
        HKEY hAcc;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_READ, &hAcc) == ERROR_SUCCESS) {
            DWORD flags = 0, hotkey = 0;
            DWORD size = sizeof(DWORD);
            RegQueryValueExW(hAcc, L"Flags", nullptr, nullptr, (LPBYTE)&flags, &size);
            size = sizeof(DWORD);
            RegQueryValueExW(hAcc, L"HotKey", nullptr, nullptr, (LPBYTE)&hotkey, &size);

            if (flags & 1) { // "On" flag
                HotkeyCombo hk;
                hk.modifiers = (hotkey >> 8) & 0xF;
                hk.vk = hotkey & 0xFF;
                char owner[128];
                snprintf(owner, 128, "Accessibility (%S)", accName);
                result[hk] = owner;
            }
            RegCloseKey(hAcc);
        }
    }

    return result;
}

// ============================================================
// Keyboard Hook Monitor
// ============================================================
static HHOOK g_hHook = nullptr;
static std::set<HotkeyCombo> g_occupiedSet;
static std::map<std::string, std::set<HotkeyCombo>> g_processHotkeyUsage;
static DWORD g_monitorStartTick = 0;

// Track modifier state
static bool g_ctrlDown = false, g_altDown = false, g_shiftDown = false, g_winDown = false;

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT* kb = (KBDLLHOOKSTRUCT*)lParam;

        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            UINT vk = kb->vkCode;

            // Update modifier state
            if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) g_ctrlDown = true;
            else if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) g_altDown = true;
            else if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) g_shiftDown = true;
            else if (vk == VK_LWIN || vk == VK_RWIN) g_winDown = true;
            else {
                // Non-modifier key — check if this combo is occupied
                UINT mod = 0;
                if (g_ctrlDown) mod |= MOD_CONTROL;
                if (g_altDown)  mod |= MOD_ALT;
                if (g_shiftDown) mod |= MOD_SHIFT;
                if (g_winDown)  mod |= MOD_WIN;

                HotkeyCombo combo = {mod, vk};
                if (g_occupiedSet.count(combo)) {
                    // Get foreground process
                    HWND fg = GetForegroundWindow();
                    DWORD pid = 0;
                    GetWindowThreadProcessId(fg, &pid);

                    char procName[256] = "<background>";
                    if (pid) {
                        HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
                        if (hProc) {
                            wchar_t wname[256];
                            DWORD wsize = 256;
                            if (QueryFullProcessImageNameW(hProc, 0, wname, &wsize)) {
                                // Extract filename from path
                                wchar_t* fname = wcsrchr(wname, L'\\');
                                fname = fname ? fname + 1 : wname;
                                WideCharToMultiByte(CP_UTF8, 0, fname, -1, procName, 255, nullptr, nullptr);
                            }
                            CloseHandle(hProc);
                        }
                    }

                    DWORD elapsed = GetTickCount() - g_monitorStartTick;
                    std::wstring comboStr = ModToWString(combo.modifiers) + VkToString(combo.vk);
                    printf("[%02lu:%02lu:%02lu] HOTKEY: %-30s <- %s\n",
                        (elapsed / 3600000) % 24,
                        (elapsed / 60000) % 60,
                        (elapsed / 1000) % 60,
                        WstrToUtf8(comboStr).c_str(),
                        procName);

                    g_processHotkeyUsage[std::string(procName)].insert(combo);
                }
            }
        }
        else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
            UINT vk = kb->vkCode;
            if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) g_ctrlDown = false;
            else if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) g_altDown = false;
            else if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) g_shiftDown = false;
            else if (vk == VK_LWIN || vk == VK_RWIN) g_winDown = false;
        }
    }
    return CallNextHookEx(g_hHook, nCode, wParam, lParam);
}

bool StartMonitor(const std::vector<OccupiedHotkey>& occupied) {
    // Build occupied set
    g_occupiedSet.clear();
    for (auto& oh : occupied) {
        g_occupiedSet.insert(oh.combo);
    }

    g_processHotkeyUsage.clear();
    g_monitorStartTick = GetTickCount();
    g_ctrlDown = g_altDown = g_shiftDown = g_winDown = false;

    g_hHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
        GetModuleHandle(nullptr), 0);

    if (!g_hHook) {
        printf("[ERROR] SetWindowsHookEx failed: %lu\n", GetLastError());
        printf("  Hint: Try running as Administrator\n");
        return false;
    }

    printf("[MONITOR] Hook installed. Monitoring keyboard events...\n");
    printf("  Press known hotkeys to see which process responds.\n");
    return true;
}

void StopMonitor() {
    if (g_hHook) {
        UnhookWindowsHookEx(g_hHook);
        g_hHook = nullptr;
        printf("[MONITOR] Hook removed.\n");
    }
}

// ============================================================
// UI: Print results
// ============================================================
void PrintResults(const ScanResult& sr,
                  const std::map<HotkeyCombo, std::string>& regMap)
{
    printf("\n======================================================================\n");
    printf("                     SCAN RESULTS: %zu hotkeys occupied\n",
        sr.occupied.size());
    printf("======================================================================\n\n");

    if (sr.occupied.empty()) {
        printf("  No occupied hotkeys found.\n");
        return;
    }

    // Categorize
    std::vector<const OccupiedHotkey*> systemHk, appHk, singleModHk;

    for (auto& oh : sr.occupied) {
        // Well-known system hotkeys
        bool isSystem = false;
        UINT mod = oh.combo.modifiers;
        UINT vk = oh.combo.vk;

        if (mod == MOD_WIN) isSystem = true;
        if (mod == (MOD_CONTROL | MOD_ALT) && (vk == VK_DELETE || vk == VK_TAB)) isSystem = true;
        if (mod == (MOD_CONTROL | MOD_SHIFT) && vk == VK_ESCAPE) isSystem = true;
        if (mod == MOD_ALT && (vk == VK_TAB || vk == VK_F4)) isSystem = true;
        if (mod == (MOD_ALT | MOD_SHIFT) && vk == VK_TAB) isSystem = true;
        if (mod == MOD_WIN && vk == VK_TAB) isSystem = true;
        if (mod == MOD_CONTROL && vk == VK_ESCAPE) isSystem = true;

        if (isSystem) systemHk.push_back(&oh);
        else if (mod == 0 || mod == MOD_ALT || mod == MOD_CONTROL || mod == MOD_SHIFT)
            singleModHk.push_back(&oh);
        else
            appHk.push_back(&oh);
    }

    auto printGroup = [&](const char* title, const std::vector<const OccupiedHotkey*>& list) {
        if (list.empty()) return;
        printf("  [%s] (%zu):\n    ", title, list.size());
        int col = 0;
        for (auto oh : list) {
            std::wstring s = ModToWString(oh->combo.modifiers) + VkToString(oh->combo.vk);
            printf("%-22S", s.c_str());
            if (++col >= 3) { printf("\n    "); col = 0; }
        }
        if (col != 0) printf("\n");
        printf("\n");
    };

    printGroup("System Hotkeys (Win+*, Ctrl+Alt+Del, etc.)", systemHk);
    printGroup("Likely App Hotkeys (multi-modifier)", appHk);
    printGroup("Single/No Modifier (app or driver)", singleModHk);

    // Show registry mappings
    if (!regMap.empty()) {
        printf("  [Registry-identified Hotkeys]:\n");
        for (auto& kv : regMap) {
            printf("    ");
            PrintCombo(kv.first);
            printf(" -> %s\n", kv.second.c_str());
        }
    }
}

// ============================================================
// Main
// ============================================================
int main() {
    // Set console to UTF-8
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    printf("======================================================================\n");
    printf("  Windows Global Hotkey Detector v1.0 (C++)\n");
    printf("  Brute-force scan + Keyboard hook monitor + Registry lookup\n");
    printf("======================================================================\n\n");

    HINSTANCE hInstance = GetModuleHandle(nullptr);

    // Check admin
    BOOL isAdmin = FALSE;
    PSID adminGroup;
    SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&ntAuth, 2, SECURITY_BUILTIN_DOMAIN_RID,
        DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(nullptr, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    if (!isAdmin) {
        printf("[WARN] Not running as Administrator.\n");
        printf("  Scan works, but keyboard hook may need admin rights.\n\n");
    }

    // Phase 1: Create hidden window
    printf("[PHASE 1] Creating message window...\n");
    if (!CreateHiddenWindow(hInstance)) {
        printf("[FATAL] Cannot create window. Exiting.\n");
        return 1;
    }

    // Phase 2: Brute-force scan
    printf("\n[PHASE 2] Brute-force scanning registered hotkeys...\n");
    ScanResult scanResult = BruteForceScan();

    // Phase 3: Registry lookup
    printf("\n[PHASE 3] Reading registry hotkeys...\n");
    auto regMap = ReadRegistryHotkeys();
    printf("  Found %zu IME/accessibility hotkeys in registry.\n", regMap.size());

    // Phase 4: Print results
    PrintResults(scanResult, regMap);

    // Phase 5: Keyboard hook monitor
    printf("\n[PHASE 5] Starting keyboard hook monitor...\n");
    printf("  Press known hotkeys to see which process responds.\n");
    printf("  Press ESC to exit monitoring and show summary.\n\n");

    if (!StartMonitor(scanResult.occupied)) {
        printf("[ERROR] Monitor failed to start.\n");
        DestroyWindow(g_hWnd);
        return 1;
    }

    // Message loop
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);

        // Check for ESC to exit
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            // Wait for ESC release to avoid immediate re-trigger
            while (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
                Sleep(10);
            }
            printf("\n[EXIT] ESC pressed, stopping monitor...\n");
            break;
        }
    }

    // Phase 6: Monitor summary
    StopMonitor();

    printf("\n======================================================================\n");
    printf("                     MONITOR SUMMARY\n");
    printf("======================================================================\n\n");

    if (g_processHotkeyUsage.empty()) {
        printf("  No hotkeys detected during monitoring.\n");
        printf("  Try pressing Win+E, Ctrl+Alt+Del, Win+D etc.\n");
    } else {
        for (auto& kv : g_processHotkeyUsage) {
            printf("  [%s] — %zu hotkeys:\n", kv.first.c_str(), kv.second.size());
            for (auto& hk : kv.second) {
                printf("    - ");
                std::wstring s = ModToWString(hk.modifiers) + VkToString(hk.vk);
                printf("%S\n", s.c_str());
            }
            printf("\n");
        }
    }

    // Cleanup
    printf("[CLEANUP] Destroying window...\n");
    DestroyWindow(g_hWnd);

    printf("\nPress Enter to exit...");
    getchar();
    return 0;
}
