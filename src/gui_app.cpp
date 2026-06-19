/*
 * Hotkey Detector GUI — EUI-NEO frontend
 */
#include "eui_neo.h"

#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

// ============================================================
// Win32 API
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

static HWND g_hiddenWnd = nullptr;

LRESULT CALLBACK HiddenWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_HOTKEY) return 0;
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

void CreateHiddenWindow() {
    const wchar_t CLASS_NAME[] = L"HotkeyDetectorGUI";
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = HiddenWndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = CLASS_NAME;
    RegisterClassExW(&wc);
    g_hiddenWnd = CreateWindowExW(0, CLASS_NAME, L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, GetModuleHandle(nullptr), nullptr);
}

// ============================================================
// Data
// ============================================================
struct OccupiedHotkey { UINT mod = 0; UINT vk = 0; DWORD error = 0; };
struct MonitorEvent { std::string time, combo, process; };

struct AppState {
    bool scanned = false, scanning = false, monitoring = false;
    int scanProgress = 0;
    std::vector<OccupiedHotkey> occupied;
    std::map<std::string, std::vector<OccupiedHotkey>> categorized;
    std::vector<MonitorEvent> events;
    std::map<std::string, std::set<std::string>> processHotkeys;
    std::string statusText = "就绪。点击扫描启动。";
    std::string registryInfo;
    int eventLogLimit = 200;
    int leftPage = 0;
    int activeTab = 0; // 0=Home, 1=热键监控, 2=结果, 3=设置, 4=关于
    bool darkMode = true;
    bool minimizeToTray = true;
};
static AppState state;
static std::mutex stateMutex;

// Keyboard hook
static HHOOK g_kbHook = nullptr;
static bool g_ctrlDown = false, g_altDown = false, g_shiftDown = false, g_winDown = false;
static std::set<std::pair<UINT, UINT>> g_occupiedSet;

// Event hooks — decoupled from our own process state
static HWINEVENTHOOK g_fgHook = nullptr;   // foreground change
static HWINEVENTHOOK g_wndHook = nullptr;  // window creation
static DWORD g_lastHotkeyTick = 0;
static UINT g_lastHotkeyMod = 0, g_lastHotkeyVk = 0;
static std::set<DWORD> g_resolvedPids; // prevent duplicate events for same hotkey

static const DWORD HOTKEY_WINDOW_MS = 800; // look for response within 800ms

// ============================================================
// Helpers
// ============================================================
const wchar_t* VkToString(UINT vk) {
    switch (vk) {
        case 0x08: return L"BS"; case 0x09: return L"Tab"; case 0x0D: return L"Enter";
        case 0x13: return L"Pause"; case 0x1B: return L"Esc"; case 0x20: return L"空格";
        case 0x21: return L"PgUp"; case 0x22: return L"PgDn"; case 0x23: return L"End"; case 0x24: return L"Home";
        case 0x25: return L"左"; case 0x26: return L"上"; case 0x27: return L"右"; case 0x28: return L"下";
        case 0x2C: return L"PrtSc"; case 0x2D: return L"Ins"; case 0x2E: return L"Del";
        case 0x5B: return L"左Win"; case 0x5C: return L"右Win"; case 0x5D: return L"菜单";
        case 0x6F: return L"Num/"; case 0x6A: return L"Num*"; case 0x6B: return L"Num+"; case 0x6D: return L"Num-";
        case 0x90: return L"NumLk"; case 0x91: return L"ScrLk";
        case 0xAD: return L"音量-"; case 0xAE: return L"音量+"; case 0xAF: return L"静音";
        case 0xB0: return L"下一首"; case 0xB1: return L"上一首"; case 0xB2: return L"停止"; case 0xB3: return L"播放";
        default: break;
    }
    if (vk >= 0x30 && vk <= 0x39) { static wchar_t b[2]; b[0]=L'0'+(vk-0x30); b[1]=0; return b; }
    if (vk >= 0x41 && vk <= 0x5A) { static wchar_t b[2]; b[0]=L'A'+(vk-0x41); b[1]=0; return b; }
    if (vk >= 0x70 && vk <= 0x7B) { static wchar_t b[4]; swprintf(b,4,L"F%d",vk-0x6F); return b; }
    static wchar_t b[16]; swprintf(b,16,L"0x%02X",vk); return b;
}

std::string WsToUtf8(const std::wstring& ws) {
    if (ws.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string r(n - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &r[0], n, nullptr, nullptr);
    return r;
}

std::string ComboStr(UINT mod, UINT vk) {
    std::wstring s;
    if (mod & MOD_CONTROL) s += L"Ctrl+";
    if (mod & MOD_ALT)     s += L"Alt+";
    if (mod & MOD_SHIFT)   s += L"Shift+";
    if (mod & MOD_WIN)     s += L"Win+";
    s += VkToString(vk);
    return WsToUtf8(s);
}

static std::map<std::string, std::string> g_friendlyNames = {
    {"explorer.exe", "Windows 资源管理器"},
    {"Explorer.EXE", "Windows 资源管理器"},
    {"SearchHost.exe", "Windows 搜索"},
    {"StartMenuExperienceHost.exe", "Windows 开始菜单"},
    {"ShellExperienceHost.exe", "Windows Shell"},
    {"SystemSettings.exe", "Windows 设置"},
    {"TextInputHost.exe", "Windows 输入法"},
    {"Taskmgr.exe", "任务管理器"},
    {"snippingtool.exe", "截图工具"},
    {"SnippingTool.exe", "截图工具"},
    {"ScreenClippingHost.exe", "截图工具"},
    {"WeChat.exe", "微信"},
    {"Wechat.exe", "微信"},
    {"QQ.exe", "QQ"},
    {"TIM.exe", "TIM"},
    {"DingTalk.exe", "钉钉"},
    {"Dingtalk.exe", "钉钉"},
    {"msedge.exe", "Microsoft Edge"},
    {"chrome.exe", "Google Chrome"},
    {"firefox.exe", "Mozilla Firefox"},
    {"Code.exe", "VS Code"},
    {"devenv.exe", "Visual Studio"},
    {"notepad.exe", "记事本"},
    {"Notepad.exe", "记事本"},
    {"calc.exe", "计算器"},
    {"ApplicationFrameHost.exe", "Windows 应用"},
    {"WinRAR.exe", "WinRAR"},
    {"7zFM.exe", "7-Zip"},
    {"Honeyview.exe", "Honeyview"},
    {"PotPlayerMini64.exe", "PotPlayer"},
    {"vlc.exe", "VLC"},
    {"spotify.exe", "Spotify"},
    {"Discord.exe", "Discord"},
    {"Telegram.exe", "Telegram"},
    {"Todoist.exe", "Todoist"},
    {"Obsidian.exe", "Obsidian"},
    {"Notion.exe", "Notion"},
    {"typora.exe", "Typora"},
    {"foobar2000.exe", "foobar2000"},
    {"steam.exe", "Steam"},
    {"EpicGamesLauncher.exe", "Epic Games"},
    {"Obs.exe", "OBS Studio"},
    {"obs64.exe", "OBS Studio"},
    {"DiscordPTB.exe", "Discord PTB"},
    {"XboxPcApp.exe", "Xbox"},
    {"GameBar.exe", "Xbox Game Bar"},
    {"PowerToys.exe", "PowerToys"},
    {"PowerToys.Settings.exe", "PowerToys"},
    {"PowerLauncher.exe", "PowerToys Run"},
    {"FancyZonesEditor.exe", "PowerToys FancyZones"},
    {"Everything.exe", "Everything"},
    {"Listary.exe", "Listary"},
    {"Clash.exe", "Clash"},
    {"Clash for Windows.exe", "Clash"},
    {"v2rayN.exe", "v2rayN"},
    {"WireGuard.exe", "WireGuard"},
    {"Snipaste.exe", "Snipaste"},
    {"pixpin.exe", "PixPin"},
    {"ShareX.exe", "ShareX"},
    {"Greenshot.exe", "Greenshot"},
    {"Lightshot.exe", "Lightshot"},
    {"TrafficMonitor.exe", "TrafficMonitor"},
    {"RaiDrive.exe", "RaiDrive"},
    {"Rclone.exe", "Rclone"},
    {"ContextMenuManager.exe", "右键管理"},
    {"KeePass.exe", "KeePass"},
    {"1Password.exe", "1Password"},
    {"Bitwarden.exe", "Bitwarden"},
    {"SumatraPDF.exe", "SumatraPDF"},
    {"Adobe Acrobat.exe", "Adobe Acrobat"},
    {"acrobat.exe", "Adobe Acrobat"},
    {"ONENOTEM.EXE", "OneNote"},
    {"WINWORD.EXE", "Word"},
    {"EXCEL.EXE", "Excel"},
    {"POWERPNT.EXE", "PowerPoint"},
    {"MSACCESS.EXE", "Access"},
    {"OUTLOOK.EXE", "Outlook"},
    {"Ditto.exe", "Ditto"},
    {"CopyQ.exe", "CopyQ"},
    {"AutoHotkey.exe", "AutoHotkey"},
    {"AutoHotkeyU64.exe", "AutoHotkey"},
    {"altsnap.exe", "AltSnap"},
    {"AltSnap.exe", "AltSnap"},
    {"mspaint.exe", "画图"},
    {"SnippingTool.exe", "截图工具"},
    {"wps.exe", "WPS Office"},
    {"wpp.exe", "WPS 演示"},
    {"wpspdf.exe", "WPS PDF"},
    {"et.exe", "WPS 表格"},
    {"mouseinc.exe", "MouseInc"},
    {"Quicker.exe", "Quicker"},
    {"utools.exe", "uTools"},
    {"FlowLauncher.exe", "FlowLauncher"},
    {"Keyviz.exe", "Keyviz"},
    {"Carnac.exe", "Carnac"},
};

static std::string GetProcessName(DWORD pid) {
    if (pid == 0 || pid == 4) return "";
    HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hp) return "";
    char buf[256] = {};
    wchar_t wn[256]; DWORD ws = 256;
    if (QueryFullProcessImageNameW(hp, 0, wn, &ws)) {
        wchar_t* fn = wcsrchr(wn, L'\\');
        WideCharToMultiByte(CP_UTF8, 0, fn ? fn + 1 : wn, -1, buf, 255, nullptr, nullptr);
    }
    CloseHandle(hp);
    std::string raw(buf);
    auto it = g_friendlyNames.find(raw);
    if (it != g_friendlyNames.end()) return it->second;
    return raw;
}

// ============================================================
// Scanner
// ============================================================
void FinishScan() {
    state.categorized.clear();
    std::vector<OccupiedHotkey> systemHk, appHk, singleHk;

    for (auto& oh : state.occupied) {
        UINT mod = oh.mod, vk = oh.vk;
        bool isSys = false;
        // System: any Win+*, Alt+Tab/F4, Ctrl+Alt+Del/Tab, Ctrl+Esc, Ctrl+Shift+Esc
        if (mod & MOD_WIN) isSys = true;
        else if (mod == MOD_ALT && (vk == 0x09 || vk == 0x73)) isSys = true;         // Alt+Tab, Alt+F4
        else if (mod == (MOD_ALT|MOD_SHIFT) && vk == 0x09) isSys = true;              // Alt+Shift+Tab
        else if (mod == MOD_WIN && vk == 0x09) isSys = true;                          // Win+Tab
        else if (mod == (MOD_CONTROL|MOD_ALT) && (vk == 0x2E || vk == 0x09)) isSys = true; // Ctrl+Alt+Del, Ctrl+Alt+Tab
        else if (mod == MOD_CONTROL && vk == 0x1B) isSys = true;                      // Ctrl+Esc
        else if (mod == (MOD_CONTROL|MOD_SHIFT) && vk == 0x1B) isSys = true;          // Ctrl+Shift+Esc

        if (isSys) systemHk.push_back(oh);
        else if (mod == 0 || mod == MOD_ALT || mod == MOD_CONTROL || mod == MOD_SHIFT)
            singleHk.push_back(oh);
        else appHk.push_back(oh);
    }
    state.categorized["系统热键"] = systemHk;
    state.categorized["应用热键 (多修饰键)"] = appHk;
    state.categorized["单键 (单修饰键或无修饰)"] = singleHk;

    // Add known Windows built-in shortcuts (not all are RegisterHotKey-based)
    struct WinShortcut { UINT mod; UINT vk; const char* desc; };
    static const WinShortcut winBuiltin[] = {
        // Win key combos
        {MOD_WIN, 0x44, "Win+D (显示桌面)"},
        {MOD_WIN, 0x45, "Win+E (文件资源管理器)"},
        {MOD_WIN, 0x52, "Win+R (运行)"},
        {MOD_WIN, 0x4C, "Win+L (锁屏)"},
        {MOD_WIN, 0x4D, "Win+M (最小化全部)"},
        {MOD_WIN, 0x49, "Win+I (设置)"},
        {MOD_WIN, 0x58, "Win+X (快速链接菜单)"},
        {MOD_WIN, 0x41, "Win+A (操作中心)"},
        {MOD_WIN, 0x56, "Win+V (剪贴板历史)"},
        {MOD_WIN, 0x48, "Win+H (语音输入)"},
        {MOD_WIN, 0x4B, "Win+K (投屏)"},
        {MOD_WIN, 0x2E, "Win+Delete (删除确认)"},
        {MOD_WIN, 0x51, "Win+Q (搜索)"},
        {MOD_WIN, 0x53, "Win+S (搜索)"},
        {MOD_WIN, 0x57, "Win+W (小组件)"},
        {MOD_WIN, 0x5A, "Win+Z (窗口布局)"},
        {MOD_WIN, 0x43, "Win+C (Copilot)"},
        {MOD_WIN, 0x4E, "Win+N (通知)"},
        {MOD_WIN, 0x54, "Win+T (任务栏切换)"},
        {MOD_WIN, 0x55, "Win+U (辅助功能设置)"},
        {MOD_WIN, 0x50, "Win+P (投影)"},
        {MOD_WIN, 0x47, "Win+G (游戏栏)"},
        {MOD_WIN, 0x46, "Win+F (反馈中心)"},
        {MOD_WIN, 0x42, "Win+B (通知区域)"},
        {MOD_WIN, 0x4F, "Win+O (设备方向锁)"},
        {MOD_WIN, 0x09, "Win+Tab (任务视图)"},
        {MOD_WIN, 0x20, "Win+空格 (输入法切换)"},
        {MOD_WIN, 0x08, "Win+Backspace (撤销)"},
        {MOD_WIN, 0x0D, "Win+Enter (讲述人)"},
        {MOD_WIN, 0x13, "Win+Pause (系统属性)"},
        {MOD_WIN, 0x21, "Win+PageUp (虚拟桌面左)"},
        {MOD_WIN, 0x22, "Win+PageDown (虚拟桌面右)"},
        {MOD_WIN, 0x24, "Win+Home (最小化其他)"},
        {MOD_WIN, 0x25, "Win+左 (窗口靠左)"},
        {MOD_WIN, 0x26, "Win+上 (最大化)"},
        {MOD_WIN, 0x27, "Win+右 (窗口靠右)"},
        {MOD_WIN, 0x28, "Win+下 (最小化/恢复)"},
        {MOD_WIN, 0x2C, "Win+PrtSc (截图保存)"},
        {MOD_WIN, 0xBC, "Win+, (速览桌面)"},
        {MOD_WIN, 0xBE, "Win+. (表情面板)"},
        {MOD_WIN, 0x70, "Win+F1 (帮助)"},
        // Win+数字 (任务栏)
        {MOD_WIN, 0x30, "Win+0 (任务栏第10个)"},
        {MOD_WIN, 0x31, "Win+1 (任务栏第1个)"},
        {MOD_WIN, 0x32, "Win+2 (任务栏第2个)"},
        {MOD_WIN, 0x33, "Win+3 (任务栏第3个)"},
        {MOD_WIN, 0x34, "Win+4 (任务栏第4个)"},
        {MOD_WIN, 0x35, "Win+5 (任务栏第5个)"},
        {MOD_WIN, 0x36, "Win+6 (任务栏第6个)"},
        {MOD_WIN, 0x37, "Win+7 (任务栏第7个)"},
        {MOD_WIN, 0x38, "Win+8 (任务栏第8个)"},
        {MOD_WIN, 0x39, "Win+9 (任务栏第9个)"},
        // Win+Ctrl+*
        {MOD_WIN|MOD_CONTROL, 0x44, "Win+Ctrl+D (新建虚拟桌面)"},
        {MOD_WIN|MOD_CONTROL, 0x46, "Win+Ctrl+F4 (关闭虚拟桌面)"},
        {MOD_WIN|MOD_CONTROL, 0x25, "Win+Ctrl+左 (切换桌面)"},
        {MOD_WIN|MOD_CONTROL, 0x27, "Win+Ctrl+右 (切换桌面)"},
        {MOD_WIN|MOD_CONTROL, 0x51, "Win+Ctrl+Q (快速助手)"},
        {MOD_WIN|MOD_CONTROL, 0x4D, "Win+Ctrl+M (放大镜设置)"},
        {MOD_WIN|MOD_CONTROL, 0x4F, "Win+Ctrl+O (屏幕键盘)"},
        {MOD_WIN|MOD_CONTROL, 0x0D, "Win+Ctrl+Enter (讲述人)"},
        // Win+Shift+*
        {MOD_WIN|MOD_SHIFT, 0x53, "Win+Shift+S (截图工具)"},
        {MOD_WIN|MOD_SHIFT, 0x4D, "Win+Shift+M (还原最小化)"},
        {MOD_WIN|MOD_SHIFT, 0x25, "Win+Shift+左 (移窗口到左屏)"},
        {MOD_WIN|MOD_SHIFT, 0x27, "Win+Shift+右 (移窗口到右屏)"},
        {MOD_WIN|MOD_SHIFT, 0x26, "Win+Shift+上 (拉伸窗口)"},
        // Alt combos
        {MOD_ALT, 0x09, "Alt+Tab (切换窗口)"},
        {MOD_ALT, 0x73, "Alt+F4 (关闭窗口)"},
        {MOD_ALT, 0x20, "Alt+空格 (窗口系统菜单)"},
        {MOD_ALT, 0x0D, "Alt+Enter (属性)"},
        {MOD_ALT, 0x2E, "Alt+Delete (删除)"},
        {MOD_ALT, 0x25, "Alt+左 (后退)"},
        {MOD_ALT, 0x27, "Alt+右 (前进)"},
        {MOD_ALT, 0x26, "Alt+上 (上级目录)"},
        {MOD_ALT, 0x22, "Alt+PageDown (下一页)"},
        {MOD_ALT, 0x21, "Alt+PageUp (上一页)"},
        {MOD_ALT, 0x1B, "Alt+Esc (切换窗口)"},
        {MOD_ALT|MOD_SHIFT, 0x09, "Alt+Shift+Tab (反向切换)"},
        // Ctrl combos
        {MOD_CONTROL, 0x1B, "Ctrl+Esc (开始菜单)"},
        {MOD_CONTROL, 0x20, "Ctrl+空格 (输入法)"},
        {MOD_CONTROL, 0x08, "Ctrl+Backspace (删词)"},
        {MOD_CONTROL, 0x2E, "Ctrl+Delete (删词)"},
        {MOD_CONTROL, 0x43, "Ctrl+C (复制)"},
        {MOD_CONTROL, 0x56, "Ctrl+V (粘贴)"},
        {MOD_CONTROL, 0x58, "Ctrl+X (剪切)"},
        {MOD_CONTROL, 0x5A, "Ctrl+Z (撤销)"},
        {MOD_CONTROL, 0x59, "Ctrl+Y (重做)"},
        {MOD_CONTROL, 0x41, "Ctrl+A (全选)"},
        {MOD_CONTROL, 0x46, "Ctrl+F (查找)"},
        {MOD_CONTROL, 0x48, "Ctrl+H (替换)"},
        {MOD_CONTROL, 0x4E, "Ctrl+N (新建)"},
        {MOD_CONTROL, 0x4F, "Ctrl+O (打开)"},
        {MOD_CONTROL, 0x50, "Ctrl+P (打印)"},
        {MOD_CONTROL, 0x53, "Ctrl+S (保存)"},
        {MOD_CONTROL, 0x57, "Ctrl+W (关闭标签)"},
        {MOD_CONTROL, 0x09, "Ctrl+Tab (切换标签)"},
        {MOD_CONTROL|MOD_SHIFT, 0x09, "Ctrl+Shift+Tab (反向切换标签)"},
        {MOD_CONTROL|MOD_SHIFT, 0x54, "Ctrl+Shift+T (恢复标签)"},
        {MOD_CONTROL|MOD_SHIFT, 0x4E, "Ctrl+Shift+N (新建文件夹)"},
        {MOD_CONTROL|MOD_SHIFT, 0x1B, "Ctrl+Shift+Esc (任务管理器)"},
        // Ctrl+Alt
        {MOD_CONTROL|MOD_ALT, 0x2E, "Ctrl+Alt+Delete (安全选项)"},
        {MOD_CONTROL|MOD_ALT, 0x09, "Ctrl+Alt+Tab (切换面板)"},
        // Function keys
        {0, 0x70, "F1 (帮助)"},
        {0, 0x71, "F2 (重命名)"},
        {0, 0x72, "F3 (查找)"},
        {0, 0x73, "F4 (地址栏)"},
        {0, 0x74, "F5 (刷新)"},
        {0, 0x75, "F6 (地址栏切换)"},
        {0, 0x7A, "F10 (菜单栏)"},
        {0, 0x7B, "F11 (全屏)"},
        {MOD_ALT, 0x73, ""},  // skip — already listed above
        // Media
        {0, 0xAD, "静音"}, {0, 0xAE, "音量-"}, {0, 0xAF, "音量+"},
        {0, 0xB0, "下一首"}, {0, 0xB1, "上一首"}, {0, 0xB2, "停止"}, {0, 0xB3, "播放/暂停"},
        {0, 0xB4, "邮件"}, {0, 0xB5, "媒体选择"}, {0, 0xB6, "计算器"}, {0, 0xB7, "浏览器"},
    };
    std::vector<OccupiedHotkey> builtinHk;
    for (auto& ws : winBuiltin) {
        if (ws.desc[0] == 0) continue; // skip empty entries
        // Don't duplicate if already in scan results or already in builtin
        bool dup = false;
        for (auto& oh : state.occupied) {
            if (oh.mod == ws.mod && oh.vk == ws.vk) { dup = true; break; }
        }
        if (!dup) {
            for (auto& bh : builtinHk) {
                if (bh.mod == ws.mod && bh.vk == ws.vk) { dup = true; break; }
            }
        }
        if (!dup) {
            builtinHk.push_back({ws.mod, ws.vk, 0});
        }
    }
    if (!builtinHk.empty()) {
        state.categorized["Windows 系统内置 (非RegisterHotKey)"] = builtinHk;
    }

    g_occupiedSet.clear();
    for (auto& oh : state.occupied) g_occupiedSet.insert({oh.mod, oh.vk});

    state.scanning = false;
    char buf[128];
    snprintf(buf, 128, "扫描完成: 发现 %zu 个被占用的热键。", state.occupied.size());
    state.statusText = buf;

    // Persist to file
    std::string savePath;
    {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        wchar_t* lastSlash = wcsrchr(exePath, L'\\');
        if (lastSlash) *(lastSlash + 1) = 0;
        char exeDir[512];
        WideCharToMultiByte(CP_UTF8, 0, exePath, -1, exeDir, 512, nullptr, nullptr);
        savePath = std::string(exeDir) + "scan_results.txt";
    }
    FILE* f = fopen(savePath.c_str(), "w");
    if (f) {
        time_t now = time(nullptr);
        char timeBuf[32];
        strftime(timeBuf, 32, "%Y-%m-%d %H:%M:%S", localtime(&now));
        fprintf(f, "# Hotkey Detector scan results — %s\n", timeBuf);
        fprintf(f, "# %zu occupied hotkeys\n", state.occupied.size());
        for (auto& oh : state.occupied) {
            std::string s = ComboStr(oh.mod, oh.vk);
            fprintf(f, "%u,%u,%s\n", oh.mod, oh.vk, s.c_str());
        }
        fclose(f);
    }
}

static bool LoadCachedResults() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (lastSlash) *(lastSlash + 1) = 0;
    char exeDir[512];
    WideCharToMultiByte(CP_UTF8, 0, exePath, -1, exeDir, 512, nullptr, nullptr);
    std::string loadPath = std::string(exeDir) + "scan_results.txt";

    FILE* f = fopen(loadPath.c_str(), "r");
    if (!f) return false;

    state.occupied.clear();
    char line[256];
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue; // skip comments
        UINT mod = 0, vk = 0;
        if (sscanf(line, "%u,%u", &mod, &vk) == 2) {
            state.occupied.push_back({mod, vk, 0x581});
            count++;
        }
    }
    fclose(f);

    if (count > 0) {
        state.scanned = true;
        FinishScan();
        char buf[128];
        snprintf(buf, 128, "已加载缓存: %d 个被占用的热键。点击扫描以刷新。", count);
        state.statusText = buf;
    }

    // Load monitoring results
    std::string monPath = std::string(exeDir) + "monitor_results.txt";
    FILE* fm = fopen(monPath.c_str(), "r");
    if (fm) {
        char mline[512];
        std::string currentProc;
        while (fgets(mline, sizeof(mline), fm)) {
            // Trim newline
            size_t len = strlen(mline);
            while (len > 0 && (mline[len-1]=='\n'||mline[len-1]=='\r')) mline[--len] = 0;
            if (mline[0] == '#') continue;
            if (mline[0] == ' ' && mline[1] == ' ' && mline[2] == '[') {
                // Process header: "  [procName]"
                currentProc = std::string(mline + 3);
                if (!currentProc.empty() && currentProc.back() == ']')
                    currentProc.pop_back();
            } else if (mline[0] == ' ' && mline[1] == ' ' && mline[2] == ' ' && mline[3] == ' ') {
                // Hotkey: "    combo"
                std::string hk = std::string(mline + 4);
                if (!currentProc.empty() && !hk.empty())
                    state.processHotkeys[currentProc].insert(hk);
            }
        }
        fclose(fm);
    }

    return count > 0;
}

// ============================================================
// Keyboard hook — only detects combo, does NOT check foreground
// ============================================================
LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && state.monitoring) {
        auto* kb = (KBDLLHOOKSTRUCT*)lParam;
        UINT vk = kb->vkCode;
        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) g_ctrlDown = true;
            else if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) g_altDown = true;
            else if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) g_shiftDown = true;
            else if (vk == VK_LWIN || vk == VK_RWIN) g_winDown = true;
            else {
                UINT mod = 0;
                if (g_ctrlDown) mod |= MOD_CONTROL;
                if (g_altDown)  mod |= MOD_ALT;
                if (g_shiftDown) mod |= MOD_SHIFT;
                if (g_winDown)  mod |= MOD_WIN;
                if (mod != 0) {
                    bool known = g_occupiedSet.count({mod, vk}) != 0;
                    // Well-known system combos
                    bool isSys =
                        (mod == MOD_ALT && (vk == 0x09 || vk == 0x73)) ||
                        (mod == (MOD_ALT|MOD_SHIFT) && vk == 0x09) ||
                        (mod & MOD_WIN) ||
                        (mod == (MOD_CONTROL|MOD_ALT) && (vk == 0x2E || vk == 0x09)) ||
                        (mod == MOD_CONTROL && vk == 0x1B) ||
                        (mod == (MOD_CONTROL|MOD_SHIFT) && vk == 0x1B);

                    g_lastHotkeyMod = mod;
                    g_lastHotkeyVk = vk;
                    g_lastHotkeyTick = GetTickCount();
                    g_resolvedPids.clear();

                    if (isSys) {
                        MonitorEvent ev;
                        time_t tnow = time(nullptr); char tb[16]; strftime(tb, 16, "%H:%M:%S", localtime(&tnow));
                        ev.time = tb;
                        ev.combo = ComboStr(mod, vk);
                        ev.process = "Windows 系统";
                        {
                            std::lock_guard<std::mutex> lock(stateMutex);
                            state.events.push_back(ev);
                            if ((int)state.events.size() > state.eventLogLimit) state.events.erase(state.events.begin());
                            state.processHotkeys[ev.process].insert(ev.combo);
                        }
                        g_lastHotkeyTick = 0;
                    } else if (known) {
                        // Let foreground/window hooks resolve
                    }
                    // else: unregistered combo — still let hooks try to resolve
                }
            }
        } else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
            if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) g_ctrlDown = false;
            else if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) g_altDown = false;
            else if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) g_shiftDown = false;
            else if (vk == VK_LWIN || vk == VK_RWIN) g_winDown = false;
        }
    }
    return CallNextHookEx(g_kbHook, nCode, wParam, lParam);
}

// ============================================================
// Response detection hooks — foreground change OR new window created
// ============================================================
static void ResolveHotkeyOwner(DWORD pid, const char* method) {
    if (pid == 0 || pid == 4 || pid == GetCurrentProcessId()) return;
    if (g_resolvedPids.count(pid)) return;
    g_resolvedPids.insert(pid);

    std::string procName = GetProcessName(pid);
    if (procName.empty()) procName = "(系统)";
    procName += " [" + std::string(method) + "]";

    MonitorEvent ev;
    time_t tnow = time(nullptr); char tb[16]; strftime(tb, 16, "%H:%M:%S", localtime(&tnow));
    ev.time = tb;
    ev.combo = ComboStr(g_lastHotkeyMod, g_lastHotkeyVk);
    ev.process = procName;

    {
        std::lock_guard<std::mutex> lock(stateMutex);
        state.events.push_back(ev);
        if ((int)state.events.size() > state.eventLogLimit)
            state.events.erase(state.events.begin());
        state.processHotkeys[procName].insert(ev.combo);
    }

    g_lastHotkeyTick = 0; // consumed — prevent timeout from adding duplicate
}

// Foreground change
VOID CALLBACK ForegroundHook(HWINEVENTHOOK hWinEventHook, DWORD event, HWND hwnd,
                              LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime) {
    if (g_lastHotkeyTick == 0) return;
    DWORD elapsed = GetTickCount() - g_lastHotkeyTick;
    if (elapsed > HOTKEY_WINDOW_MS) return;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    ResolveHotkeyOwner(pid, "前台");
}

// New window created (catches screenshot tools, popups, etc.)
VOID CALLBACK WindowCreateHook(HWINEVENTHOOK hWinEventHook, DWORD event, HWND hwnd,
                                LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime) {
    if (g_lastHotkeyTick == 0) return;
    if (idObject != OBJID_WINDOW) return;
    DWORD elapsed = GetTickCount() - g_lastHotkeyTick;
    if (elapsed > HOTKEY_WINDOW_MS) return;

    // Only care about top-level windows
    if (GetParent(hwnd) != nullptr) return;
    if (!IsWindowVisible(hwnd)) return;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    ResolveHotkeyOwner(pid, "新窗口");
}

bool StartMonitor() {
    if (g_kbHook) return true;

    // Keyboard hook
    g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, GetModuleHandle(nullptr), 0);
    if (!g_kbHook) return false;

    // Foreground change detection
    g_fgHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        nullptr, ForegroundHook, 0, 0, WINEVENT_OUTOFCONTEXT);

    // New window creation detection (screenshot tools, popups, etc.)
    g_wndHook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_CREATE,
        nullptr, WindowCreateHook, 0, 0, WINEVENT_OUTOFCONTEXT);

    g_ctrlDown = g_altDown = g_shiftDown = g_winDown = false;
    g_lastHotkeyTick = 0;
    g_resolvedPids.clear();
    return true;
}

void StopMonitor() {
    if (g_fgHook) { UnhookWinEvent(g_fgHook); g_fgHook = nullptr; }
    if (g_wndHook) { UnhookWinEvent(g_wndHook); g_wndHook = nullptr; }
    if (g_kbHook) { UnhookWindowsHookEx(g_kbHook); g_kbHook = nullptr; }

    // Save monitoring results
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (lastSlash) *(lastSlash + 1) = 0;
    char exeDir[512];
    WideCharToMultiByte(CP_UTF8, 0, exePath, -1, exeDir, 512, nullptr, nullptr);
    std::string path = std::string(exeDir) + "monitor_results.txt";

    FILE* f = fopen(path.c_str(), "w");
    if (f) {
        time_t now = time(nullptr);
        char tb[32]; strftime(tb, 32, "%Y-%m-%d %H:%M:%S", localtime(&now));
        fprintf(f, "# Monitor results — %s\n", tb);
        for (auto& kv : state.processHotkeys) {
            fprintf(f, "  [%s]\n", kv.first.c_str());
            for (auto& hk : kv.second)
                fprintf(f, "    %s\n", hk.c_str());
        }
        fclose(f);
    }
}

// ============================================================
// Registry
// ============================================================
std::string ReadRegistryHotkeys() {
    std::string result;
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Input Method\\Hot Keys", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD idx = 0; wchar_t name[64]; DWORD nl = 64;
        while (RegEnumKeyExW(hKey, idx++, name, &nl, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
            HKEY hSub;
            if (RegOpenKeyExW(hKey, name, 0, KEY_READ, &hSub) == ERROR_SUCCESS) {
                DWORD km[4]={0}, vk[4]={0}; DWORD s = sizeof(km);
                RegQueryValueExW(hSub, L"Key Modifiers", nullptr, nullptr, (LPBYTE)km, &s);
                s = sizeof(vk); RegQueryValueExW(hSub, L"Virtual Key", nullptr, nullptr, (LPBYTE)vk, &s);
                if (km[0] || vk[0]) result += "IME: " + ComboStr(km[0], vk[0]) + "\n";
                RegCloseKey(hSub);
            }
            nl = 64;
        }
        RegCloseKey(hKey);
    }
    return result.empty() ? "(无)" : result;
}

// ============================================================
// Theme
// ============================================================
struct Palette { eui::Color bg, surface, surfaceHover, surfacePressed, text, muted, border, strong, strongText, accent, accentText; eui::Shadow panelShadow; };
Palette palette() {
    if (state.darkMode) {
        return {
            {0.055f, 0.058f, 0.068f}, {0.080f, 0.084f, 0.098f}, {0.105f, 0.110f, 0.128f},
            {0.130f, 0.135f, 0.155f}, {0.940f, 0.945f, 0.955f}, {0.550f, 0.565f, 0.590f},
            {0.180f, 0.190f, 0.210f}, {0.120f, 0.180f, 0.330f}, {0.880f, 0.920f, 0.980f},
            {0.180f, 0.340f, 0.620f}, {0.940f, 0.960f, 0.990f},
            {true, {0.0f, 8.0f}, 28.0f, 0.0f, {0.0f, 0.0f, 0.0f, 0.35f}}
        };
    }
    return {
        {0.965f, 0.966f, 0.970f}, {1.000f, 1.000f, 1.000f}, {0.930f, 0.935f, 0.945f},
        {0.880f, 0.890f, 0.905f}, {0.040f, 0.042f, 0.048f}, {0.550f, 0.565f, 0.590f},
        {0.840f, 0.850f, 0.870f}, {0.120f, 0.180f, 0.330f}, {0.980f, 0.982f, 0.985f},
        {0.180f, 0.340f, 0.620f}, {0.035f, 0.037f, 0.044f},
        {true, {0.0f, 6.0f}, 24.0f, 0.0f, {0.100f, 0.120f, 0.180f, 0.12f}}
    };
}
eui::Color mix(eui::Color a, eui::Color b, float t) { return {a.r+(b.r-a.r)*t, a.g+(b.g-a.g)*t, a.b+(b.b-a.b)*t, a.a+(b.a-a.a)*t}; }
components::theme::ThemeColorTokens themeTokens() { Palette p = palette(); return {p.bg, p.strong, p.surface, p.surfaceHover, p.surfacePressed, p.text, p.border, true}; }

// ============================================================
// UI helpers
// ============================================================
void label(eui::Ui& ui, const std::string& id, float x, float y, float w, float h,
           const std::string& text, float fs, eui::Color c, eui::HorizontalAlign ha = eui::HorizontalAlign::Left) {
    ui.text(id).x(x).y(y).size(w,h).text(text).fontSize(fs).lineHeight(h).color(c).horizontalAlign(ha).verticalAlign(eui::VerticalAlign::Center).build();
}
void panel(eui::Ui& ui, const std::string& id, float x, float y, float w, float h, float r, eui::Color fill, eui::Color border = {0,0,0,0}, eui::Shadow shadow = {}) {
    auto b = ui.rect(id).x(x).y(y).size(w,h).color(fill).radius(r);
    if (border.a > 0.001f) b.border(1.0f, border);
    if (shadow.enabled && shadow.color.a > 0.001f) b.shadow(shadow);
    b.build();
}

// ============================================================
// App entry
// ============================================================
namespace app {

const DslAppConfig& dslAppConfig() {
    static DslAppConfig config = DslAppConfig{}
        .title("全局热键检测器")
        .pageId("hotkey")
        .clearColor(palette().bg)
        .windowSize(1440, 960)
        .fps(60.0);
    return config;
}

// System tray
static NOTIFYICONDATAW g_nid = {};
static const UINT WM_TRAYICON = WM_USER + 1;
static HWND g_trayHwnd = nullptr;
static bool g_trayReady = false;

LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_TRAYICON) {
        if (LOWORD(lParam) == WM_LBUTTONDBLCLK || LOWORD(lParam) == WM_LBUTTONUP) {
            // Restore window
            HWND fg = GetForegroundWindow();
            ShowWindow(fg, SW_RESTORE);
            SetForegroundWindow(fg);
        } else if (LOWORD(lParam) == WM_RBUTTONUP) {
            // Show context menu
            POINT pt; GetCursorPos(&pt);
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 2, L"显示窗口");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, 1, L"退出");
            SetForegroundWindow(hwnd);
            int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(menu);
            if (cmd == 1) exit(0);
            if (cmd == 2) { HWND fg = GetForegroundWindow(); ShowWindow(fg, SW_RESTORE); SetForegroundWindow(fg); }
        }
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static void InitTray() {
    const wchar_t CLASS[] = L"HDTrayClass";
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = CLASS;
    RegisterClassExW(&wc);
    g_trayHwnd = CreateWindowExW(0, CLASS, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);

    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_trayHwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    // Load custom icon from assets, fall back to default
    wchar_t iconPath[MAX_PATH];
    GetModuleFileNameW(nullptr, iconPath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(iconPath, L'\\');
    if (lastSlash) *(lastSlash + 1) = 0;
    wcscat(iconPath, L"assets\\icon.ico");
    g_nid.hIcon = (HICON)LoadImageW(nullptr, iconPath, IMAGE_ICON, 16, 16, LR_LOADFROMFILE);
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    wcscpy(g_nid.szTip, L"全局热键检测器");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_trayReady = true;
}

static void RemoveTray() {
    if (g_trayReady) Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_trayHwnd) DestroyWindow(g_trayHwnd);
}

void compose(eui::Ui& ui, const eui::Screen& screen) {
    // Load cached results on first frame
    static bool firstFrame = true;
    if (firstFrame) {
        firstFrame = false;
        LoadCachedResults();
        InitTray();
    }

    Palette p = palette();
    float m = 24.0f;
    float cw = std::min(1400.0f, screen.width - m*2);
    float cx = (screen.width - cw)*0.5f;

    std::lock_guard<std::mutex> lock(stateMutex);

    ui.stack("root").size(screen.width, screen.height).content([&] {
        ui.rect("bg").size(screen.width, screen.height).color(p.bg).build();

        // === Top bar ===
        float ty = 12.0f;
        label(ui, "title", cx, ty, 400, 44, "全局热键检测器", 36.0f, p.text);
        label(ui, "status", cx, ty+46, cw, 22, state.statusText, 16.0f, p.muted);

        // Buttons
        float bx = cx + cw - 340.0f;
        float bh = 38.0f;
        ui.rect("scan.btn").x(bx).y(ty+8).size(150, bh)
            .states(p.accent, mix(p.accent,{1,1,1},0.1f), mix(p.accent,{0,0,0},0.1f))
            .radius(10.0f).disabled(state.scanning)
            .onClick([] {
                std::thread([] {
                    { std::lock_guard<std::mutex> lk(stateMutex); state.scanning=true; state.scanProgress=0; state.occupied.clear(); state.scanned=false; state.statusText="正在扫描..."; }
                    CreateHiddenWindow();
                    UINT mods[]={0,MOD_ALT,MOD_CONTROL,MOD_SHIFT,MOD_WIN,
                        MOD_ALT|MOD_CONTROL,MOD_ALT|MOD_SHIFT,MOD_ALT|MOD_WIN,
                        MOD_CONTROL|MOD_SHIFT,MOD_CONTROL|MOD_WIN,MOD_SHIFT|MOD_WIN,
                        MOD_ALT|MOD_CONTROL|MOD_SHIFT,MOD_ALT|MOD_CONTROL|MOD_WIN,
                        MOD_ALT|MOD_SHIFT|MOD_WIN,MOD_CONTROL|MOD_SHIFT|MOD_WIN,
                        MOD_ALT|MOD_CONTROL|MOD_SHIFT|MOD_WIN};
                    std::vector<UINT> vks;
                    for(UINT v=0x30;v<=0x39;v++)vks.push_back(v);
                    for(UINT v=0x41;v<=0x5A;v++)vks.push_back(v);
                    for(UINT v=0x70;v<=0x7B;v++)vks.push_back(v);
                    for(UINT v=0x60;v<=0x69;v++)vks.push_back(v);
                    UINT sk[]={0x08,0x09,0x0D,0x13,0x1B,0x20,0x21,0x22,0x23,0x24,
                        0x25,0x26,0x27,0x28,0x2C,0x2D,0x2E,0x6F,0x6A,0x6B,0x6D,0x6E,
                        0x90,0x91,0xAD,0xAE,0xAF,0xB0,0xB1,0xB2,0xB3,0xB4,0xB5,0xB6,0xB7,
                        0xBA,0xBB,0xBC,0xBD,0xBE,0xBF,0xC0,0xDB,0xDC,0xDD,0xDE};
                    for(UINT k:sk)vks.push_back(k);
                    int total=16*(int)vks.size(), id=1, tried=0;
                    for(UINT mod:mods){for(UINT vk:vks){
                        if((mod&MOD_WIN)&&(vk==0x5B||vk==0x5C))continue;
                        if((mod&MOD_ALT)&&(vk==VK_MENU||vk==VK_LMENU||vk==VK_RMENU))continue;
                        if((mod&MOD_CONTROL)&&(vk==VK_CONTROL||vk==VK_LCONTROL||vk==VK_RCONTROL))continue;
                        if((mod&MOD_SHIFT)&&(vk==VK_SHIFT||vk==VK_LSHIFT||vk==VK_RSHIFT))continue;
                        tried++; if(id>0xBFFF)goto done;
                        BOOL ok=RegisterHotKey(g_hiddenWnd,id++,mod|MOD_NOREPEAT,vk);
                        DWORD err=ok?0:GetLastError();
                        if(ok)UnregisterHotKey(g_hiddenWnd,id-1);
                        else{std::lock_guard<std::mutex> lk(stateMutex);state.occupied.push_back({mod,vk,err});}
                        if(tried%200==0){std::lock_guard<std::mutex> lk(stateMutex);state.scanProgress=tried;char buf[64];snprintf(buf,64,"正在扫描... %d/%d",tried,total);state.statusText=buf;}
                    }}
                    done: { std::lock_guard<std::mutex> lk(stateMutex); state.scanned=true; FinishScan(); state.registryInfo=ReadRegistryHotkeys(); }
                    DestroyWindow(g_hiddenWnd); g_hiddenWnd=nullptr;
                }).detach();
            }).build();
        ui.text("scan.label").x(bx).y(ty+8).size(150,bh)
            .text(state.scanning?"...":"扫描").fontSize(19.0f).lineHeight(22.0f)
            .color(p.accentText).horizontalAlign(eui::HorizontalAlign::Center)
            .verticalAlign(eui::VerticalAlign::Center).build();

        float mx = bx + 168.0f;
        ui.rect("mon.btn").x(mx).y(ty+8).size(168,bh)
            .states(state.monitoring?p.accent:p.surface, p.surfaceHover, p.surfacePressed)
            .radius(10.0f).border(1.0f, state.monitoring?p.accent:p.border)
            .onClick([] {
                std::lock_guard<std::mutex> lk(stateMutex);
                if(state.monitoring){StopMonitor();state.monitoring=false;state.statusText="监控已停止。";}
                else{if(StartMonitor()){state.monitoring=true;state.statusText="监控中。按下热键检测进程归属。";state.events.clear();}
                else state.statusText="监控启动失败 (需要管理员权限?)";}
            }).build();
        ui.text("mon.label").x(mx).y(ty+8).size(168,bh)
            .text(state.monitoring?"停止监控":"开始监控").fontSize(19.0f).lineHeight(22.0f)
            .color(state.monitoring?p.accentText:p.text)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .verticalAlign(eui::VerticalAlign::Center).build();

        // === Content area: sidebar + tab content (full width) ===
        float ly = ty + 100.0f;
        float lh = screen.height - ly - 20.0f;
        float sideW = 120.0f;
        float cxW = cw - sideW - 10.0f;
        float ctnX = cx + sideW + 10.0f;

        // Sidebar
        panel(ui, "side.bg", cx, ly, sideW, lh, 14.0f, p.surface, p.border);
        float tabY = ly + 14.0f;
        auto tabBtn = [&](int tab, const std::string& txt) {
            bool sel = state.activeTab == tab;
            ui.rect("tab."+std::to_string(tab)).x(cx+8).y(tabY).size(sideW-16,34)
                .states(sel?p.accent:p.surface, p.surfaceHover, p.surfacePressed)
                .radius(8).border(sel?0.0f:1.0f, sel?eui::Color{0,0,0,0}:p.border)
                .onClick([tab]{std::lock_guard<std::mutex> lk(stateMutex);state.activeTab=tab;state.leftPage=0;}).build();
            ui.text("tab."+std::to_string(tab)+".t").x(cx+8).y(tabY).size(sideW-16,34)
                .text(txt).fontSize(16).lineHeight(18)
                .color(sel?p.accentText:p.text)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center).build();
            tabY += 42.0f;
        };
        tabBtn(0, "Home");
        tabBtn(1, "热键监控");
        tabBtn(2, "结果");

        // Separator
        float sepY = ly + lh - 104.0f;
        ui.rect("side.sep").x(cx+12).y(sepY).size(sideW-24,1).color(p.border).build();

        // Bottom tabs: 设置, 关于
        tabY = sepY + 12.0f;
        tabBtn(3, "设置");
        tabBtn(4, "关于");

        // Content
        float cty = ly;
        panel(ui, "ctn.bg", ctnX, cty, cxW, lh, 14.0f, p.surface, p.border);
        float ciy = cty + 14.0f;

        if (state.activeTab == 0) {
            // ====== Home ======
            label(ui, "hm.title", ctnX+24, ciy+20, cxW-48, 44, "全局热键检测器", 36.0f, p.text);
            label(ui, "hm.sub", ctnX+24, ciy+70, cxW-48, 28, "检测系统中被占用的全局快捷键，识别进程归属", 18.0f, p.muted);
            char buf[256];
            if (state.scanned) {
                snprintf(buf, 256, "已扫描到 %zu 个被占用的热键", state.occupied.size());
                label(ui, "hm.stat1", ctnX+24, ciy+130, cxW-48, 28, buf, 20.0f, p.text);
                int sysCnt=0, appCnt=0, sglCnt=0;
                for (auto& c : state.categorized) {
                    if (c.first.find("系统")!=std::string::npos) sysCnt=(int)c.second.size();
                    if (c.first.find("应用")!=std::string::npos) appCnt=(int)c.second.size();
                    if (c.first.find("单键")!=std::string::npos) sglCnt=(int)c.second.size();
                }
                snprintf(buf, 256, "系统热键: %d   应用热键: %d   单键: %d", sysCnt, appCnt, sglCnt);
                label(ui, "hm.stat2", ctnX+24, ciy+165, cxW-48, 26, buf, 16.0f, p.muted);
            } else {
                label(ui, "hm.noscan", ctnX+24, ciy+130, cxW-48, 28, "尚未扫描 — 点击右上角「扫描」按钮开始", 20.0f, p.muted);
            }
            if (!state.processHotkeys.empty()) {
                snprintf(buf, 256, "监控已记录 %zu 个进程的热键使用", state.processHotkeys.size());
                label(ui, "hm.mon", ctnX+24, ciy+210, cxW-48, 26, buf, 16.0f, p.muted);
            }
            label(ui, "hm.usage", ctnX+24, ciy+280, cxW-48, 200,
                "使用方法\n\n"
                "1. 点击「扫描」— 暴力枚举所有 RegisterHotKey 注册的热键\n"
                "2. 点击「开始监控」— 按下热键，观察哪个进程响应\n"
                "3. 切换到「热键监控」标签查看实时日志\n"
                "4. 切换到「结果」标签查看进程→热键汇总", 16.0f, p.muted);

        } else if (state.activeTab == 1) {
            // ====== 热键监控: split left=scan results, right=monitor log ======
            float halfW = (cxW - 24.0f) * 0.5f;
            float rightX = ctnX + halfW + 12.0f;

            // Left: scan results
            if (!state.scanned && !state.scanning) {
                label(ui, "sp.empty", ctnX+16, ciy+40, halfW-32, 60,
                    "点击右上角「扫描」", 18.0f, p.muted, eui::HorizontalAlign::Center);
            } else if (state.scanning) {
                label(ui, "sp.scanning", ctnX+16, ciy+40, halfW-32, 60,
                    "扫描中... " + std::to_string(state.scanProgress), 18.0f, p.muted, eui::HorizontalAlign::Center);
            } else {
                struct RowSlot { int type; std::string text; };
                std::vector<RowSlot> rows;
                for (auto& cat : state.categorized) {
                    if (cat.second.empty()) continue;
                    rows.push_back({-1, cat.first + " (" + std::to_string(cat.second.size()) + ")"});
                    for (int i=0; i<(int)cat.second.size(); i++)
                        rows.push_back({i%3, ComboStr(cat.second[i].mod, cat.second[i].vk)});
                }
                if (!state.registryInfo.empty() && state.registryInfo != "(无)") {
                    rows.push_back({-1, "注册表:"}); rows.push_back({0, state.registryInfo});
                }
                const float rh=26.0f, hh=32.0f, btnH=46.0f;
                int rpp=(int)((lh-btnH-8)/rh); if(rpp<2)rpp=2;
                int tp=((int)rows.size()+rpp-1)/rpp; if(tp<1)tp=1;
                if(state.leftPage>=tp)state.leftPage=0;
                int st=state.leftPage*rpp, ed=std::min(st+rpp,(int)rows.size());
                float y=ciy;
                for(int i=st;i<ed;i++){
                    auto& r=rows[i];
                    if(r.type==-1){
                        label(ui,"sp.h."+std::to_string(i),ctnX+16,y,halfW-32,hh,r.text,19.0f,p.text); y+=hh;
                    }else{
                        float cw=(halfW-64)/3;
                        label(ui,"sp.k."+std::to_string(i),ctnX+28+r.type*cw,y,cw,24,r.text,16.0f,p.muted);
                        if(r.type==2||i+1>=ed||rows[i+1].type==-1||rows[i+1].type==0)y+=rh;
                    }
                }
                float by=cty+lh-40;
                label(ui,"sp.pg",ctnX+8,by,halfW-100,26,std::to_string(state.leftPage+1)+"/"+std::to_string(tp),15.0f,p.muted,eui::HorizontalAlign::Center);
                auto mk=[&](const std::string& id,float bx,const std::string& t,bool en,std::function<void()>cb){
                    ui.rect(id).x(bx).y(by).size(44,26).states(en?p.surface:p.surface,p.surfaceHover,p.surfacePressed).radius(5).border(1,p.border).disabled(!en).onClick(cb).build();
                    ui.text(id+".t").x(bx).y(by).size(44,26).text(t).fontSize(13).lineHeight(15).color(en?p.text:p.muted).horizontalAlign(eui::HorizontalAlign::Center).verticalAlign(eui::VerticalAlign::Center).build();
                };
                mk("pg.p",ctnX+halfW-108,"<",state.leftPage>0,[]{std::lock_guard<std::mutex> lk(stateMutex);state.leftPage--;});
                mk("pg.n",ctnX+halfW-58,">",state.leftPage<tp-1,[]{std::lock_guard<std::mutex> lk(stateMutex);state.leftPage++;});
            }

            // Right: monitor log
            label(ui,"sp.log.title",rightX+8,ciy,halfW-16,28,"监控日志",22.0f,p.text);
            float lgy=ciy+32;
            if(state.events.empty()&&!state.monitoring){
                label(ui,"sp.log.empty",rightX+8,lgy+30,halfW-16,80,"点击「开始监控」后按热键",16.0f,p.muted,eui::HorizontalAlign::Center);
            }else if(state.monitoring&&state.events.empty()){
                label(ui,"sp.log.wait",rightX+8,lgy+30,halfW-16,60,"等待热键按下...",16.0f,p.muted,eui::HorizontalAlign::Center);
            }else{
                int evc=(int)state.events.size(),show=std::min(evc,16);
                for(int i=0;i<show;i++){
                    auto& ev=state.events[evc-show+i];
                    std::string ln="["+ev.time+"] "+ev.combo+" -> "+ev.process;
                    float a=(i==show-1)?1.0f:0.55f;
                    label(ui,"ev."+std::to_string(i),rightX+8,lgy+i*28.0f,halfW-16,26,ln,15.0f,{p.text.r*a,p.text.g*a,p.text.b*a,p.text.a});
                }
                if(evc>16){char c[32];snprintf(c,32,"... %d more",evc-16);label(ui,"ev.more",rightX+8,lgy+16*28.0f,halfW-16,24,c,14.0f,p.muted);}
            }
        } else if (state.activeTab == 2) {
            // ====== 结果: process summary ======
            label(ui,"rs.title",ctnX+20,ciy+4,cxW-40,32,"进程 → 热键汇总",22.0f,p.text);
            float yy=ciy+42;
            if(state.processHotkeys.empty()){
                label(ui,"rs.empty",ctnX+20,yy+30,cxW-40,60,"暂无数据。开始监控并按下热键后，\n检测到的进程→热键映射会出现在这里。",17.0f,p.muted,eui::HorizontalAlign::Center);
            }else{
                for(auto& kv:state.processHotkeys){
                    if(yy>cty+lh-30)break;
                    label(ui,"rs.p."+kv.first,ctnX+20,yy,cxW-40,26,kv.first,18.0f,p.text); yy+=28;
                    std::string hs;for(auto& h:kv.second){if(!hs.empty())hs+="  ";hs+=h;}
                    label(ui,"rs.h."+kv.first,ctnX+36,yy,cxW-56,22,hs,15.0f,p.muted); yy+=26;
                }
            }
        } else if (state.activeTab == 3) {
            // ====== 设置 ======
            label(ui,"st.title",ctnX+20,ciy+4,cxW-40,32,"设置",22.0f,p.text);
            float sy=ciy+52;

            // Dark mode toggle
            {
                bool dm=state.darkMode;
                ui.rect("st.dark.btn").x(ctnX+cxW-120).y(sy+2).size(80,28)
                    .states(dm?p.accent:p.surface,p.surfaceHover,p.surfacePressed)
                    .radius(14).border(1,dm?eui::Color{0,0,0,0}:p.border)
                    .onClick([]{std::lock_guard<std::mutex> lk(stateMutex);state.darkMode=!state.darkMode;}).build();
                ui.text("st.dark.t").x(ctnX+cxW-120).y(sy+2).size(80,28)
                    .text(dm?"ON":"OFF").fontSize(16).lineHeight(18)
                    .color(dm?p.accentText:p.text)
                    .horizontalAlign(eui::HorizontalAlign::Center).verticalAlign(eui::VerticalAlign::Center).build();
                label(ui,"st.dark.lbl",ctnX+20,sy,cxW-200,30,"深色模式",18.0f,p.text);
                sy+=46;
            }
            // Tray toggle
            {
                bool tr=state.minimizeToTray;
                ui.rect("st.tray.btn").x(ctnX+cxW-120).y(sy+2).size(80,28)
                    .states(tr?p.accent:p.surface,p.surfaceHover,p.surfacePressed)
                    .radius(14).border(1,tr?eui::Color{0,0,0,0}:p.border)
                    .onClick([]{std::lock_guard<std::mutex> lk(stateMutex);state.minimizeToTray=!state.minimizeToTray;}).build();
                ui.text("st.tray.t").x(ctnX+cxW-120).y(sy+2).size(80,28)
                    .text(tr?"ON":"OFF").fontSize(16).lineHeight(18)
                    .color(tr?p.accentText:p.text)
                    .horizontalAlign(eui::HorizontalAlign::Center).verticalAlign(eui::VerticalAlign::Center).build();
                label(ui,"st.tray.lbl",ctnX+20,sy,cxW-200,30,"最小化到系统托盘",18.0f,p.text);
                sy+=46;
            }
            sy+=10;

            label(ui,"st.sponsor",ctnX+20,sy,cxW-40,28,"\xe2\x9d\xa4 支持开发",18.0f,p.text);
            sy+=32;
            label(ui,"st.sponsor.info",ctnX+20,sy,cxW-40,80,
                "如果这个工具对你有帮助，欢迎赞助支持后续开发。\n\n赞助方式: [待补充]",15.0f,p.muted);
            sy+=100;

            char vbuf[128];
            snprintf(vbuf,128,"版本: 1.0.0  |  已扫描: %zu 个热键  |  进程映射: %zu 个",state.occupied.size(),state.processHotkeys.size());
            label(ui,"st.ver",ctnX+20,sy,cxW-40,24,vbuf,15.0f,p.muted);
        } else {
            // ====== 关于 ======
            label(ui,"ab.title",ctnX+20,ciy+4,cxW-40,32,"关于",22.0f,p.text);
            float ay=ciy+52;

            label(ui,"ab.name",ctnX+20,ay,cxW-40,36,"全局热键检测器",28.0f,p.text);
            ay+=42;
            label(ui,"ab.ver",ctnX+20,ay,cxW-40,26,"版本 1.0.0  |  Build 2026-06-19",16.0f,p.muted);
            ay+=36;
            label(ui,"ab.desc",ctnX+20,ay,cxW-40,80,
                "检测 Windows 系统中被占用的全局快捷键，\n识别每个快捷键归属于哪个进程。\n\n"
                "技术方案: RegisterHotKey 暴力枚举 + WH_KEYBOARD_LL 键盘钩子\n"
                "UI 框架: EUI-NEO (OpenGL + GLFW)\n"
                "开发语言: C++17",15.0f,p.muted);
            ay+=110;

            label(ui,"ab.sponsor.title",ctnX+20,ay,cxW-40,28,"\xe2\x9d\xa4 赞助支持",20.0f,p.text);
            ay+=36;
            label(ui,"ab.sponsor.info",ctnX+20,ay,cxW-40,120,
                "如果这个工具帮到了你，欢迎赞助一杯咖啡！\n\n"
                "赞助方式: [待补充]\n\n"
                "你的支持是我继续开发的动力。\n"
                "感谢每一位赞助者！\n\n"
                "赞助者名单:\n"
                "  (虚位以待)",15.0f,p.muted);
            ay+=140;

            label(ui,"ab.thanks",ctnX+20,ay,cxW-40,24,"\xe2\x80\xa2 感谢 EUI-NEO 作者 Aino 提供的优秀 UI 框架",15.0f,p.muted);
        }

        // Timeout resolution for hotkeys that foreground/window hooks didn't catch
        if (g_lastHotkeyTick != 0) {
            DWORD elapsed = GetTickCount() - g_lastHotkeyTick;
            if (elapsed > HOTKEY_WINDOW_MS) {
                bool known = g_occupiedSet.count({g_lastHotkeyMod, g_lastHotkeyVk}) != 0;
                if (g_resolvedPids.empty()) {
                    MonitorEvent ev;
                    time_t tnow = time(nullptr); char tb[16]; strftime(tb, 16, "%H:%M:%S", localtime(&tnow));
                    ev.time = tb;
                    ev.combo = ComboStr(g_lastHotkeyMod, g_lastHotkeyVk);
                    ev.process = known ? "(已注册热键 — 后台响应)" : "(未注册 — 可能用键盘钩子)";
                    state.events.push_back(ev);
                    if ((int)state.events.size() > state.eventLogLimit) state.events.erase(state.events.begin());
                    state.processHotkeys[ev.process].insert(ev.combo);
                }
                g_lastHotkeyTick = 0;
                g_resolvedPids.clear();
            }
        }

    }).build();
}

} // namespace app
