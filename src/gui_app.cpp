/*
 * Hotkey Detector GUI — EUI-NEO frontend
 */
#include "eui_neo.h"

#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
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
    const wchar_t CLASS_NAME[] = L"HotKeySightGUI";
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
    std::string statusType = "info"; // "info", "warn", "error"
    std::string registryInfo;
    int eventLogLimit = 200;
    int leftPage = 0;
    int activeTab = 0; // 0=Home, 1=热键监控, 2=结果, 3=设置, 4=关于
    bool darkMode = true;
    bool minimizeToTray = true;
    bool isAdmin = false;
    bool scanError = false;
    bool monitorError = false;
    std::string scanErrorMsg;
    std::string monitorErrorMsg;
    int accentPresetDark = 0;
    int accentPresetLight = 0;
    bool useCustomAccentDark = false;
    bool useCustomAccentLight = false;
    float customRgbDark[3] = {0.0f,0.471f,0.831f};
    float customRgbLight[3] = {0.0f,0.471f,0.831f};
    bool colorPickerOpen = false;
    std::string processDetectStatus;
    bool processDetecting = false;
    std::map<std::string, std::set<std::string>> hotkeyConflicts; // combo -> {processes}
};
static AppState state;
static std::mutex stateMutex;

static void TrackConflict(const std::string& proc, const std::string& combo) {
    auto& owners = state.hotkeyConflicts[combo];
    owners.insert(proc);
}
static std::string ConflictBadge(const std::string& combo) {
    auto it = state.hotkeyConflicts.find(combo);
    if (it != state.hotkeyConflicts.end() && it->second.size() > 1) {
        std::string s = " \xe2\x9a\xa0";
        int n=0; for(auto& p:it->second){if(n++)s+=",";s+=p;}
        return s;
    }
    return "";
}

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
    {"miservice.exe", "小米电脑管家"},
    {"miclipboard.exe", "小米电脑管家"},
    {"xiaomipcmanager.exe", "小米电脑管家"},
    {"micloudservice.exe", "小米云服务"},
    {"qmlauncher.exe", "QQ电脑管家"},
    {"qqpcmgr.exe", "QQ电脑管家"},
    {"qqmusic.exe", "QQ音乐"},
    {"cloudmusic.exe", "网易云音乐"},
    {"Snipaste.exe", "Snipaste"},
    {"snipaste.exe", "Snipaste"},
};

static std::string NormalizeProcName(std::string raw) {
    // lowercase + strip .exe
    std::string s = raw;
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    if (s.size() > 4 && s.substr(s.size()-4) == ".exe")
        s = s.substr(0, s.size()-4);
    // Lookup friendly name
    for (auto& kv : g_friendlyNames) {
        std::string k = kv.first;
        for (auto& c : k) c = (char)tolower((unsigned char)c);
        if (k.size() > 4 && k.substr(k.size()-4) == ".exe")
            k = k.substr(0, k.size()-4);
        if (k == s) return kv.second;
    }
    return raw; // fallback: return original
}

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
    return NormalizeProcName(std::string(buf));
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
        // Well-known third-party app hotkeys (may use keyboard hooks, not RegisterHotKey)
        // Snipaste
        {0, 0x70, "Snipaste: F1 (截图)"},
        {0, 0x72, "Snipaste: F3 (贴图)"},
        // QQ / WeChat
        {MOD_CONTROL|MOD_ALT, 0x41, "QQ/微信: Ctrl+Alt+A (截图)"},
        {MOD_CONTROL|MOD_ALT, 0x57, "微信: Ctrl+Alt+W (显示)"},
        {MOD_CONTROL|MOD_ALT, 0x5A, "QQ: Ctrl+Alt+Z (显示)"},
        {MOD_CONTROL|MOD_ALT, 0x58, "QQ: Ctrl+Alt+X (提取文字)"},
        // Other tools
        {MOD_CONTROL|MOD_SHIFT, 0x45, "Everything: Ctrl+Shift+E (搜索)"},
        {MOD_ALT, 0x20, "PowerToys Run: Alt+Space (启动器)"},
        {0, 0x2C, "截图工具: PrintScreen"},
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
                currentProc = NormalizeProcName(std::string(mline + 3));
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
    state.monitorError = false;
    std::string errs;

    g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, GetModuleHandle(nullptr), 0);
    if (!g_kbHook) {
        DWORD e = GetLastError();
        char buf[128];
        snprintf(buf, 128, "键盘钩子失败 (错误 %lu)。请以管理员运行。", e);
        errs += buf;
    }

    g_fgHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        nullptr, ForegroundHook, 0, 0, WINEVENT_OUTOFCONTEXT);
    if (!g_fgHook) errs += "前台检测钩子失败。";

    // Catch both CREATE and SHOW — some popups (e.g., clipboard) are created hidden then shown
    g_wndHook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_SHOW,
        nullptr, WindowCreateHook, 0, 0, WINEVENT_OUTOFCONTEXT);
    if (!g_wndHook) errs += "窗口创建检测钩子失败。";

    if (!g_kbHook) {
        // Critical failure — clean up and report
        if (g_fgHook) { UnhookWinEvent(g_fgHook); g_fgHook = nullptr; }
        if (g_wndHook) { UnhookWinEvent(g_wndHook); g_wndHook = nullptr; }
        state.monitorError = true;
        state.monitorErrorMsg = errs;
        return false;
    }

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
struct AccentPreset { const char* name; eui::Color darkAccent; eui::Color lightAccent; };
#define C(r,g,b) eui::Color{(r)/255.0f,(g)/255.0f,(b)/255.0f,1.0f}
static const AccentPreset kAccentPresets[] = {
    // Windows 11 official accent palette (48 colors from system settings)
    {"#FFB900", C(255,185,0), C(255,185,0)},
    {"#FF8C00", C(255,140,0), C(255,140,0)},
    {"#F7630C", C(247,99,12), C(247,99,12)},
    {"#CA5010", C(202,80,16), C(202,80,16)},
    {"#DA3B01", C(218,59,1), C(218,59,1)},
    {"#EF6950", C(239,105,80), C(239,105,80)},
    {"#D13438", C(209,52,56), C(209,52,56)},
    {"#FF4343", C(255,67,67), C(255,67,67)},
    {"#E74856", C(231,72,86), C(231,72,86)},
    {"#E81123", C(232,17,35), C(232,17,35)},
    {"#EA005E", C(234,0,94), C(234,0,94)},
    {"#C30052", C(195,0,82), C(195,0,82)},
    {"#E3008C", C(227,0,140), C(227,0,140)},
    {"#BF0077", C(191,0,119), C(191,0,119)},
    {"#C239B3", C(194,57,179), C(194,57,179)},
    {"#9A0089", C(154,0,137), C(154,0,137)},
    {"#0078D4", C(0,120,212), C(0,120,212)},
    {"#0064B4", C(0,100,180), C(0,100,180)},
    {"#106EBE", C(16,110,190), C(16,110,190)},
    {"#004E8C", C(0,78,140), C(0,78,140)},
    {"#0078D7", C(0,120,215), C(0,120,215)},
    {"#0013A0", C(0,19,160), C(0,19,160)},
    {"#005A9E", C(0,90,158), C(0,90,158)},
    {"#0099BC", C(0,153,188), C(0,153,188)},
    {"#2D7D9A", C(45,125,154), C(45,125,154)},
    {"#00B7C3", C(0,183,195), C(0,183,195)},
    {"#038387", C(3,131,135), C(3,131,135)},
    {"#00B294", C(0,178,148), C(0,178,148)},
    {"#018574", C(1,133,116), C(1,133,116)},
    {"#00CC6A", C(0,204,106), C(0,204,106)},
    {"#10893E", C(16,137,62), C(16,137,62)},
    {"#007C3C", C(0,124,60), C(0,124,60)},
    {"#107C10", C(16,124,16), C(16,124,16)},
    {"#498205", C(73,130,5), C(73,130,5)},
    {"#647C64", C(100,124,100), C(100,124,100)},
    {"#515C6B", C(81,92,107), C(81,92,107)},
    {"#567C73", C(86,124,115), C(86,124,115)},
    {"#8764B8", C(135,100,184), C(135,100,184)},
    {"#881798", C(136,23,152), C(136,23,152)},
    {"#744DA9", C(116,77,169), C(116,77,169)},
    {"#B146C2", C(177,70,194), C(177,70,194)},
    {"#8E8CD8", C(142,140,216), C(142,140,216)},
    {"#6B69D6", C(107,105,214), C(107,105,214)},
    {"#69797E", C(105,121,126), C(105,121,126)},
    {"#7A7574", C(122,117,116), C(122,117,116)},
    {"#4A5459", C(74,84,89), C(74,84,89)},
    {"#8A8A8A", C(138,138,138), C(138,138,138)},
    {"#767676", C(118,118,118), C(118,118,118)},
};
#undef C
constexpr int kAccentPresetCount = sizeof(kAccentPresets)/sizeof(kAccentPresets[0]);

eui::Color GetAccent() {
    bool useCustom = state.darkMode ? state.useCustomAccentDark : state.useCustomAccentLight;
    if (useCustom) {
        float* c = state.darkMode ? state.customRgbDark : state.customRgbLight;
        return {c[0], c[1], c[2], 1.0f};
    }
    int idx = state.darkMode ? state.accentPresetDark : state.accentPresetLight;
    if (idx < 0 || idx >= kAccentPresetCount) idx = 0;
    return state.darkMode ? kAccentPresets[idx].darkAccent : kAccentPresets[idx].lightAccent;
}

Palette palette() {
    eui::Color accent = GetAccent();
    eui::Color accentText = {0.94f, 0.96f, 0.99f, 1.0f};
    if (state.darkMode) {
        return {
            {0.055f, 0.058f, 0.068f}, {0.080f, 0.084f, 0.098f}, {0.105f, 0.110f, 0.128f},
            {0.130f, 0.135f, 0.155f}, {0.940f, 0.945f, 0.955f}, {0.550f, 0.565f, 0.590f},
            {0.180f, 0.190f, 0.210f}, {0.120f, 0.180f, 0.330f}, {0.880f, 0.920f, 0.980f},
            accent, accentText,
            {true, {0.0f, 8.0f}, 28.0f, 0.0f, {0.0f, 0.0f, 0.0f, 0.35f}}
        };
    }
    return {
        {0.965f, 0.966f, 0.970f}, {1.000f, 1.000f, 1.000f}, {0.930f, 0.935f, 0.945f},
        {0.880f, 0.890f, 0.905f}, {0.040f, 0.042f, 0.048f}, {0.550f, 0.565f, 0.590f},
        {0.840f, 0.850f, 0.870f}, {0.120f, 0.180f, 0.330f}, {0.980f, 0.982f, 0.985f},
        accent, accentText,
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
    float lh = fs * 1.5f; // wider Chinese line height to prevent overlap
    ui.text(id).x(x).y(y).size(w,h).text(text).fontSize(fs).lineHeight(lh).color(c).horizontalAlign(ha).verticalAlign(eui::VerticalAlign::Center).build();
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
        .title("HotKeySight")
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
    wcscpy(g_nid.szTip, L"HotKeySight");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_trayReady = true;
}

static void RemoveTray() {
    if (g_trayReady) Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_trayHwnd) DestroyWindow(g_trayHwnd);
}

static void SaveTheme() {
    wchar_t exePath[MAX_PATH]; GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (lastSlash) *(lastSlash + 1) = 0;
    char exeDir[512]; WideCharToMultiByte(CP_UTF8, 0, exePath, -1, exeDir, 512, nullptr, nullptr);
    std::string path = std::string(exeDir) + "config.txt";
    FILE* f = fopen(path.c_str(), "w");
    if (f) {
        fprintf(f, "darkMode=%d\n", state.darkMode?1:0);
        fprintf(f, "accentDark=%d\n", state.accentPresetDark);
        fprintf(f, "accentLight=%d\n", state.accentPresetLight);
        fprintf(f, "minimizeToTray=%d\n", state.minimizeToTray?1:0);
        fclose(f);
    }
}
static void LoadTheme() {
    wchar_t exePath[MAX_PATH]; GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (lastSlash) *(lastSlash + 1) = 0;
    char exeDir[512]; WideCharToMultiByte(CP_UTF8, 0, exePath, -1, exeDir, 512, nullptr, nullptr);
    std::string path = std::string(exeDir) + "config.txt";
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return;
    char line[64]; int val;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "darkMode=%d", &val)==1) state.darkMode = val!=0;
        else if (sscanf(line, "accentDark=%d", &val)==1) state.accentPresetDark = val;
        else if (sscanf(line, "accentLight=%d", &val)==1) state.accentPresetLight = val;
        else if (sscanf(line, "minimizeToTray=%d", &val)==1) state.minimizeToTray = val!=0;
    }
    fclose(f);
}

// DLL injection engine — inject hook_dll into target process via CreateRemoteThread
static HANDLE g_injectPipe = INVALID_HANDLE_VALUE;
static std::thread g_injectThread;
static volatile bool g_injectRunning = false;

static void InjectDll(DWORD pid) {
    char dllPath[512];
    wchar_t exePath[MAX_PATH]; GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (lastSlash) *(lastSlash + 1) = 0;
    wcscat(exePath, L"hook_dll.dll");
    WideCharToMultiByte(CP_UTF8, 0, exePath, -1, dllPath, 512, nullptr, nullptr);

    HANDLE hProc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!hProc) return;

    size_t pathLen = strlen(dllPath) + 1;
    void* remoteMem = VirtualAllocEx(hProc, nullptr, pathLen, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) { CloseHandle(hProc); return; }

    WriteProcessMemory(hProc, remoteMem, dllPath, pathLen, nullptr);
    HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0,
        (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA"),
        remoteMem, 0, nullptr);
    if (hThread) {
        WaitForSingleObject(hThread, 5000);
        CloseHandle(hThread);
    }
    VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
    CloseHandle(hProc);
}

struct PipeHotkeyMsg { DWORD pid, tid; UINT modifiers, vk; int id; char name[64]; };

static void InjectPipeServer() {
    while (g_injectRunning) {
        HANDLE hPipe = CreateNamedPipeA("\\\\.\\pipe\\HotKeySightPipe",
            PIPE_ACCESS_INBOUND, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            4, sizeof(PipeHotkeyMsg), sizeof(PipeHotkeyMsg), 100, nullptr);
        if (hPipe == INVALID_HANDLE_VALUE) { Sleep(500); continue; }
        if (ConnectNamedPipe(hPipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
            PipeHotkeyMsg msg;
            DWORD read;
            while (ReadFile(hPipe, &msg, sizeof(msg), &read, nullptr) && read == sizeof(msg)) {
                std::string procName = NormalizeProcName(msg.name);
                std::string combo = ComboStr(msg.modifiers, msg.vk);
                TrackConflict(procName, combo);
                {
                    std::lock_guard<std::mutex> lock(stateMutex);
                    MonitorEvent ev;
                    time_t tnow = time(nullptr); char tb[16]; strftime(tb, 16, "%H:%M:%S", localtime(&tnow));
                    ev.time = tb; ev.combo = combo; ev.process = procName + " [已确认]";
                    state.events.push_back(ev);
                    if ((int)state.events.size() > state.eventLogLimit) state.events.erase(state.events.begin());
                    state.processHotkeys[procName].insert(combo);
                }
            }
        }
        CloseHandle(hPipe);
    }
}

static void StartInjectPipe() {
    if (g_injectRunning) return;
    g_injectRunning = true;
    g_injectThread = std::thread(InjectPipeServer);
}

static void StopInjectPipe() {
    g_injectRunning = false;
    if (g_injectThread.joinable()) g_injectThread.join();
}

// Suspend-and-rescan: find which process owns a hotkey
static std::string DetectProcessOwner(const std::string& procName, UINT testMod, UINT testVk) {
    // 1. Find process PIDs
    std::vector<DWORD> pids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return "无法创建进程快照";
    PROCESSENTRY32W pe = {sizeof(pe)};
    if (Process32FirstW(snap, &pe)) {
        do {
            std::string name = WsToUtf8(pe.szExeFile);
            if (name == procName) pids.push_back(pe.th32ProcessID);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    if (pids.empty()) return "未找到进程: " + procName;

    // 2. Check if the hotkey is currently occupied
    if (!g_hiddenWnd) CreateHiddenWindow();
    if (!g_hiddenWnd) return "无扫描窗口";
    bool wasOccupied = !RegisterHotKey(g_hiddenWnd, 0x4000, testMod | MOD_NOREPEAT, testVk);
    if (!wasOccupied) { UnregisterHotKey(g_hiddenWnd, 0x4000); return "该热键当前未被占用"; }

    // 3. Suspend all threads of all matching processes
    struct Suspended { HANDLE hThread; DWORD tid; };
    std::vector<Suspended> suspended;
    for (DWORD pid : pids) {
        HANDLE thSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (thSnap == INVALID_HANDLE_VALUE) continue;
        THREADENTRY32 te = {sizeof(te)};
        if (Thread32First(thSnap, &te)) {
            do {
                if (te.th32OwnerProcessID == pid && te.th32ThreadID != GetCurrentThreadId()) {
                    HANDLE hTh = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                    if (hTh) { SuspendThread(hTh); suspended.push_back({hTh, te.th32ThreadID}); }
                }
            } while (Thread32Next(thSnap, &te));
        }
        CloseHandle(thSnap);
    }

    // 4. Re-scan the hotkey
    Sleep(50);
    bool stillOccupied = !RegisterHotKey(g_hiddenWnd, 0x4001, testMod | MOD_NOREPEAT, testVk);
    if (stillOccupied) UnregisterHotKey(g_hiddenWnd, 0x4001);

    // 5. Resume all threads
    for (auto& s : suspended) { ResumeThread(s.hThread); CloseHandle(s.hThread); }

    if (!stillOccupied) return "确认为 " + procName + " 占用";
    return procName + " 未占用该热键 (暂停后仍被占用)";
}

void compose(eui::Ui& ui, const eui::Screen& screen) {
    static bool firstFrame = true;
    // Single-instance check via named mutex
    static HANDLE hMutex = nullptr;
    if (firstFrame) {
        firstFrame = false;
        hMutex = CreateMutexW(nullptr, TRUE, L"HotKeySight_SingleInstance");
        if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
            // Another instance is running — bring it to front and exit
            HWND existing = FindWindowW(nullptr, L"HotKeySight");
            if (existing) {
                ShowWindow(existing, SW_RESTORE);
                SetForegroundWindow(existing);
            }
            CloseHandle(hMutex);
            exit(0);
        }
        LoadTheme();
        // Admin check
        BOOL isAdmin = FALSE;
        PSID adminGroup;
        SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
        if (AllocateAndInitializeSid(&ntAuth, 2, SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminGroup)) {
            CheckTokenMembership(nullptr, adminGroup, &isAdmin);
            FreeSid(adminGroup);
        }
        state.isAdmin = isAdmin != FALSE;
        if (!state.isAdmin) {
            state.statusText = "警告: 未以管理员运行，键盘钩子可能受限。";
            state.statusType = "warn";
        }

        LoadCachedResults();
        InitTray();
        // Register cleanup
        std::atexit([]{ StopMonitor(); RemoveTray(); });
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
        label(ui, "title", cx, ty, 400, 44, "HotKeySight", 36.0f, p.text);
        eui::Color statusColor = p.muted;
        if (state.statusType == "warn") statusColor = {0.95f, 0.75f, 0.18f, 1.0f};
        else if (state.statusType == "error") statusColor = {0.95f, 0.28f, 0.30f, 1.0f};
        label(ui, "status", cx, ty+46, cw, 22, state.statusText, 16.0f, statusColor);

        // Buttons
        float bx = cx + cw - 340.0f;
        float bh = 38.0f;
        ui.rect("scan.btn").x(bx).y(ty+8).size(150, bh)
            .states(p.accent, mix(p.accent,{1,1,1},0.1f), mix(p.accent,{0,0,0},0.1f))
            .radius(10.0f).disabled(state.scanning)
            .onClick([] {
                std::thread([] {
                    { std::lock_guard<std::mutex> lk(stateMutex); state.scanning=true; state.scanProgress=0; state.occupied.clear(); state.scanned=false; state.scanError=false; state.statusText="正在扫描..."; state.statusType="info"; }
                    CreateHiddenWindow();
                    if(!g_hiddenWnd){
                        std::lock_guard<std::mutex> lk(stateMutex);
                        state.scanning=false; state.scanError=true;
                        state.scanErrorMsg="无法创建扫描窗口 (RegisterClassEx/CreateWindowEx 失败)";
                        state.statusText=state.scanErrorMsg; state.statusType="error";
                        return;
                    }
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
                        tried++; if(id>0xBFFF)goto scan_done;
                        BOOL ok=RegisterHotKey(g_hiddenWnd,id++,mod|MOD_NOREPEAT,vk);
                        DWORD err=ok?0:GetLastError();
                        if(ok)UnregisterHotKey(g_hiddenWnd,id-1);
                        else{std::lock_guard<std::mutex> lk(stateMutex);state.occupied.push_back({mod,vk,err});}
                        if(tried%200==0){std::lock_guard<std::mutex> lk(stateMutex);state.scanProgress=tried;char buf[64];snprintf(buf,64,"正在扫描... %d/%d",tried,total);state.statusText=buf;}
                    }}
                    scan_done: { std::lock_guard<std::mutex> lk(stateMutex); state.scanned=true; state.scanError=false; FinishScan(); state.registryInfo=ReadRegistryHotkeys(); }
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
                if(state.monitoring){StopMonitor();state.monitoring=false;state.statusText="监控已停止。";state.statusType="info";}
                else{
                    if(StartMonitor()){
                        state.monitoring=true;state.statusText="监控中。按下热键检测进程归属。";state.statusType="info";state.events.clear();
                    } else {
                        state.statusText=state.monitorError?state.monitorErrorMsg:"监控启动失败 (需要管理员权限?)";
                        state.statusType="error";
                    }
                }
            }).build();
        ui.text("mon.label").x(mx).y(ty+8).size(168,bh)
            .text(state.monitoring?"停止监控":"开始监控").fontSize(19.0f).lineHeight(22.0f)
            .color(state.monitoring?p.accentText:p.text)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .verticalAlign(eui::VerticalAlign::Center).build();

        // === Content area: sidebar + tab content (full width) ===
        float ly = ty + 100.0f;
        float lh = screen.height - ly - 20.0f;
        float sideW = 130.0f;
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
            label(ui, "hm.title", ctnX+24, ciy+20, cxW-48, 44, "HotKeySight", 36.0f, p.text);
            label(ui, "hm.sub", ctnX+24, ciy+70, cxW-48, 28, "检测系统中被占用的全局快捷键，识别进程归属", 18.0f, p.muted);

            // Warning/error banners
            float bannerY = ciy + 105;
            if (!state.isAdmin) {
                label(ui, "hm.warn.admin", ctnX+24, bannerY, cxW-48, 22,
                    "\xe2\x9a\xa0 未以管理员运行 — 键盘钩子可能受限，部分进程识别功能不可用", 14.0f,
                    {0.95f, 0.75f, 0.18f, 1.0f});
                bannerY += 26;
            }
            if (state.scanError) {
                label(ui, "hm.err.scan", ctnX+24, bannerY, cxW-48, 22,
                    "扫描失败: " + state.scanErrorMsg, 14.0f, {0.95f, 0.28f, 0.30f, 1.0f});
                bannerY += 26;
            }
            if (state.monitorError) {
                label(ui, "hm.err.mon", ctnX+24, bannerY, cxW-48, 22,
                    "监控失败: " + state.monitorErrorMsg, 14.0f, {0.95f, 0.28f, 0.30f, 1.0f});
                bannerY += 26;
            }

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
            label(ui, "hm.usage", ctnX+24, ciy+250, cxW-48, 160,
                "使用方法\n\n"
                "1. 点击「扫描」— 枚举所有 RegisterHotKey 热键\n"
                "2. 点击「开始监控」— 按下热键观察进程响应\n"
                "3. 切换「热键监控」标签查看实时日志\n"
                "4. 切换「结果」标签查看进程→热键汇总\n"
                "5. 主题颜色在「设置」中调整", 16.0f, p.muted);

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
            label(ui,"sp.log.title",rightX+8,ciy,halfW-60,28,"监控日志",22.0f,p.text);
            if(!state.events.empty()){
                ui.rect("sp.log.clear").x(rightX+halfW-52).y(ciy+2).size(44,24).states(p.surface,p.surfaceHover,p.surfacePressed).radius(6).border(1,p.border)
                    .onClick([]{std::lock_guard<std::mutex> lk(stateMutex);state.events.clear();state.processHotkeys.clear();}).build();
                ui.text("sp.log.clear.t").x(rightX+halfW-52).y(ciy+2).size(44,24).text("清空").fontSize(13).lineHeight(15).color(p.text)
                    .horizontalAlign(eui::HorizontalAlign::Center).verticalAlign(eui::VerticalAlign::Center).build();
            }
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
                label(ui,"rs.empty",ctnX+20,yy+40,cxW-40,60,"暂无数据。\n开始监控并按下热键后，检测到的进程→热键映射会出现在这里。",17.0f,p.muted,eui::HorizontalAlign::Center);
            }else{
                if(!state.hotkeyConflicts.empty()){
                    label(ui,"rs.conflict.title",ctnX+20,yy,cxW-40,26,"⚠ 检测到的热键冲突",18.0f,{0.95f,0.75f,0.18f,1.0f}); yy+=30;
                    for(auto& kv2:state.hotkeyConflicts){
                        if(kv2.second.size()<2)continue;
                        if(yy>cty+lh-30)break;
                        std::string owners;
                        for(auto& o:kv2.second){if(!owners.empty())owners+=" vs ";owners+=o;}
                        label(ui,"rs.conf."+kv2.first,ctnX+20,yy,cxW-40,22,kv2.first+"  →  "+owners,15.0f,{0.95f,0.75f,0.18f,1.0f}); yy+=24;
                    }
                    yy+=10;
                }
                for(auto& kv:state.processHotkeys){
                    if(yy>cty+lh-60)break;
                    std::string pname = kv.first;
                    // Strip [] tags for display
                    std::string dispName = pname;
                    auto bp = dispName.find(" [");
                    if (bp != std::string::npos) dispName = dispName.substr(0, bp);
                    label(ui,"rs.p."+pname,ctnX+20,yy,cxW-40,26,dispName,18.0f,p.text); yy+=28;
                    std::string hs;for(auto& h:kv.second){if(!hs.empty())hs+="  ";hs+=h;}
                    label(ui,"rs.h."+pname,ctnX+36,yy,cxW-56,22,hs,15.0f,p.muted); yy+=26;
                }
            }
            // Process injection tool for background hotkeys
            yy += 10;
            label(ui,"rs.inject.title",ctnX+20,yy,cxW-40,24,"注入检测 (后台热键)",16.0f,p.text); yy+=28;
            label(ui,"rs.inject.hint",ctnX+20,yy,cxW-40,40,
                "对于不弹窗的后台热键(如QQ音乐暂停)，注入DLL到目标进程\n可100%确认。点击进程旁的「注入」按钮即可。",14.0f,p.muted);
            yy+=44;
            if (!state.processHotkeys.empty()) {
                for (auto& kv : state.processHotkeys) {
                    if (yy > cty + lh - 40) break;
                    std::string pn = kv.first;
                    // Only show non-confirmed processes
                    if (pn.find("[已确认]") != std::string::npos) continue;
                    auto bp = pn.find(" [");
                    std::string cleanName = (bp != std::string::npos) ? pn.substr(0, bp) : pn;
                    if (cleanName == "Windows 系统") continue;
                    label(ui,"rs.inj.p."+cleanName,ctnX+20,yy,cxW-160,24,cleanName,16.0f,p.text);
                    ui.rect("rs.inj.btn."+cleanName).x(ctnX+cxW-140).y(yy+2).size(48,22)
                        .states(p.surface,p.surfaceHover,p.surfacePressed).radius(5).border(1,p.border)
                        .onClick([cleanName]{
                            std::thread([cleanName]{
                                std::lock_guard<std::mutex> lk(stateMutex);
                                state.processDetectStatus = "正在注入 " + cleanName + "...";
                            }).detach();
                            // Find PID and inject
                            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
                            if (snap != INVALID_HANDLE_VALUE) {
                                PROCESSENTRY32W pe = {sizeof(pe)};
                                if (Process32FirstW(snap, &pe)) {
                                    do {
                                        std::string n = WsToUtf8(pe.szExeFile);
                                        if (n == cleanName || n == cleanName + ".exe" ||
                                            (cleanName.find(n) != std::string::npos)) {
                                            StartInjectPipe();
                                            InjectDll(pe.th32ProcessID);
                                            std::lock_guard<std::mutex> lk(stateMutex);
                                            state.processDetectStatus = "已注入 " + cleanName + " (PID=" + std::to_string(pe.th32ProcessID) + ")。如果该进程注册了热键，将在日志中显示。";
                                            break;
                                        }
                                    } while (Process32NextW(snap, &pe));
                                }
                                CloseHandle(snap);
                            }
                        }).build();
                    ui.text("rs.inj.t."+cleanName).x(ctnX+cxW-140).y(yy+2).size(48,22)
                        .text("注入").fontSize(13).lineHeight(15).color(p.text)
                        .horizontalAlign(eui::HorizontalAlign::Center).verticalAlign(eui::VerticalAlign::Center).build();
                    yy += 28;
                }
            }
            if (!state.processDetectStatus.empty()) {
                label(ui,"rs.inject.result",ctnX+20,yy,cxW-40,22,state.processDetectStatus,15.0f,{0.95f,0.75f,0.18f,1.0f}); yy+=26;
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
                    .onClick([]{std::lock_guard<std::mutex> lk(stateMutex);state.darkMode=!state.darkMode;SaveTheme();}).build();
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
                    .onClick([]{std::lock_guard<std::mutex> lk(stateMutex);state.minimizeToTray=!state.minimizeToTray;SaveTheme();}).build();
                ui.text("st.tray.t").x(ctnX+cxW-120).y(sy+2).size(80,28)
                    .text(tr?"ON":"OFF").fontSize(16).lineHeight(18)
                    .color(tr?p.accentText:p.text)
                    .horizontalAlign(eui::HorizontalAlign::Center).verticalAlign(eui::VerticalAlign::Center).build();
                label(ui,"st.tray.lbl",ctnX+20,sy,cxW-200,30,"最小化到系统托盘",18.0f,p.text);
                sy+=46;
            }
            sy+=6;
            // Accent color picker — compact swatches, 8 per row
            label(ui,"st.accent.lbl",ctnX+20,sy,cxW-40,26,"强调色 (Windows 11 官方色板, 共48色)",18.0f,p.text);
            sy+=30;
            int curPreset = state.darkMode ? state.accentPresetDark : state.accentPresetLight;
            bool useCustom = state.darkMode ? state.useCustomAccentDark : state.useCustomAccentLight;
            float sw = 28.0f, sh = 28.0f, gap = 4.0f;
            int cols = 8;
            float startX = ctnX + 28;
            for (int pi = 0; pi < kAccentPresetCount; pi++) {
                int row = pi / cols, col = pi % cols;
                float sx = startX + col * (sw + gap);
                float sy2 = sy + row * (sh + gap);
                bool sel = !useCustom && curPreset == pi;
                eui::Color fill = state.darkMode ? kAccentPresets[pi].darkAccent : kAccentPresets[pi].lightAccent;
                ui.rect("st.sw."+std::to_string(pi)).x(sx).y(sy2).size(sw,sh)
                    .states(fill, fill, fill).radius(sel?14.0f:4.0f)
                    .border(sel?2.0f:1.0f, sel?eui::Color{1,1,1,1}:p.border)
                    .onClick([pi]{std::lock_guard<std::mutex> lk(stateMutex);
                        if(state.darkMode){state.accentPresetDark=pi;state.useCustomAccentDark=false;}
                        else{state.accentPresetLight=pi;state.useCustomAccentLight=false;}
                        SaveTheme();}).build();
            }
            int rows = (kAccentPresetCount + cols - 1) / cols;
            sy += rows * (sh + gap) + 10;

            // Custom color — opens EUI-NEO built-in colorPicker popover
            {
                bool isCustom = useCustom;
                float* crgb = state.darkMode ? state.customRgbDark : state.customRgbLight;
                eui::Color custCol = isCustom ? eui::Color{crgb[0],crgb[1],crgb[2],1.0f} : p.surface;
                ui.rect("st.custom.open").x(startX).y(sy).size(sw+60,sh)
                    .states(custCol, p.surfaceHover, p.surfacePressed)
                    .radius(6).border(1, isCustom?eui::Color{1,1,1,1}:p.border)
                    .onClick([]{std::lock_guard<std::mutex> lk(stateMutex);state.colorPickerOpen=true;}).build();
                ui.text("st.custom.open.t").x(startX).y(sy).size(sw+60,sh)
                    .text(isCustom ? "已自定义" : "自定义...").fontSize(14).lineHeight(16)
                    .color(isCustom ? eui::Color{0.94f,0.96f,0.99f,1.0f} : p.text)
                    .horizontalAlign(eui::HorizontalAlign::Center).verticalAlign(eui::VerticalAlign::Center).build();
            }
            sy += sh + 16;

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

            label(ui,"ab.name",ctnX+20,ay,cxW-40,36,"HotKeySight",28.0f,p.text);
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

        // EUI-NEO colorPicker — modal overlay, rendered at root for proper z-order
        std::vector<eui::Color> presetColors;
        for (int pi = 0; pi < kAccentPresetCount; pi++)
            presetColors.push_back(state.darkMode ? kAccentPresets[pi].darkAccent : kAccentPresets[pi].lightAccent);
        components::colorPicker(ui, "accentPicker")
            .open(state.colorPickerOpen)
            .colors(presetColors)
            .value(GetAccent())
            .size(std::min(420.0f, screen.width*0.55f), 380)
            .screen(screen.width, screen.height)
            .theme(themeTokens())
            .onChange([](eui::Color c){
                std::lock_guard<std::mutex> lk(stateMutex);
                float* out = state.darkMode ? state.customRgbDark : state.customRgbLight;
                out[0]=c.r; out[1]=c.g; out[2]=c.b;
                if(state.darkMode)state.useCustomAccentDark=true;else state.useCustomAccentLight=true;
                SaveTheme();
            })
            .onOpenChange([](bool open){std::lock_guard<std::mutex> lk(stateMutex);state.colorPickerOpen=open;})
            .build();

    }).build();
}

} // namespace app
