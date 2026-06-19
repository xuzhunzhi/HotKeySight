# 构建与运行指南

## 环境准备

### 方案一：MinGW-w64（当前使用）

已安装在 `C:\msys64\mingw64\bin\g++.exe`

```bash
# 确认编译器可用
export PATH="/c/msys64/mingw64/bin:$PATH"
g++ --version  # 应输出: g++.exe (Rev5) 16.1.0
```

### 方案二：MSVC（如果安装了 Visual Studio）

```
# 在 Developer Command Prompt 中
cl /EHsc /O2 main.cpp user32.lib gdi32.lib advapi32.lib
```

## 编译

```bash
cd C:\Users\26391\hotkey-detector\cpp

# MinGW 编译（静态链接，无运行时依赖）
g++ -o hotkey-detector.exe main.cpp \
    -luser32 -lgdi32 -ladvapi32 \
    -O2 -static -std=c++17

# 生成 hotkey-detector.exe (~387KB)
```

### 编译选项说明

| 选项 | 含义 |
|------|------|
| `-o hotkey-detector.exe` | 输出文件名 |
| `-luser32` | 链接 user32.dll (RegisterHotKey, SetWindowsHookEx 等) |
| `-lgdi32` | 链接 gdi32.dll |
| `-ladvapi32` | 链接 advapi32.dll (注册表操作) |
| `-O2` | 优化等级 2 |
| `-static` | 静态链接 libstdc++，生成的 exe 可独立运行 |

## 运行

### 直接双击

在资源管理器中双击 `hotkey-detector.exe`

### 命令行

```bash
./hotkey-detector.exe
```

### 以管理员权限运行（推荐）

键盘钩子（WH_KEYBOARD_LL）在非管理员下可能有限制，建议以管理员身份运行以获得完整功能。

## 操作流程

```
程序启动
    │
    ├─ 检测管理员权限
    │
    ├─ [阶段1] 创建隐藏消息窗口
    │     └─ RegisterClassEx + CreateWindowEx(HWND_MESSAGE)
    │
    ├─ [阶段2] 暴力扫描
    │     └─ 16 修饰键组合 × 104 虚拟键 = 1664 次 RegisterHotKey 调用
    │     └─ 输出: 189 个被占用热键
    │
    ├─ [阶段3] 注册表查询
    │     └─ 读取 IME 热键、辅助功能热键
    │     └─ 输出: ~9 个注册表热键
    │
    ├─ [阶段4] 显示结果
    │     └─ 系统热键(60) / 应用热键(120) / 单键(9)
    │
    ├─ [阶段5] 键盘钩子监控
    │     └─ 进入消息循环
    │     └─ 按下已知热键 → 显示哪个进程响应
    │     └─ 按 ESC 退出
    │
    └─ [阶段6] 监控摘要
          └─ 按进程分组的热键使用统计
```

## 输出示例

```
======================================================================
  Windows Global Hotkey Detector v1.0 (C++)
======================================================================

[PHASE 2] Brute-force scanning registered hotkeys...
[SCAN] Done! 1664 tried, 0 skipped, 189 occupied (0.0s)

[PHASE 3] Reading registry hotkeys...
  Found 9 IME/accessibility hotkeys in registry.

======================================================================
                     SCAN RESULTS: 189 hotkeys occupied
======================================================================

  [System Hotkeys (Win+*, Ctrl+Alt+Del, etc.)] (60):
    Alt+F4  Alt+Tab  Ctrl+Esc  Win+0  Win+1  ... Win+E  Win+R  Win+D ...

  [Likely App Hotkeys (multi-modifier)] (120):
    Ctrl+Alt+A  Ctrl+Alt+Q  Ctrl+Shift+A  ...

  [Registry-identified Hotkeys]:
    Ctrl+Space         -> IME (输入法)
    ...

[PHASE 5] Starting keyboard hook monitor...

[00:00:05] HOTKEY: Win+E     <- explorer.exe
[00:00:12] HOTKEY: Win+D     <- explorer.exe
[00:00:23] HOTKEY: Ctrl+Alt+Q <- WeChat.exe

======================================================================
                     MONITOR SUMMARY
======================================================================

  [explorer.exe] — 2 hotkeys:
    - Win+E
    - Win+D

  [WeChat.exe] — 1 hotkeys:
    - Ctrl+Alt+Q
```

## 故障排除

### "SetWindowsHookEx failed: 5" (ACCESS_DENIED)

以管理员权限重新运行。

### "SetWindowsHookEx failed: 0"

可能是杀软拦截了全局钩子。尝试暂时关闭杀软或将程序加入白名单。

### 扫描结果为空

检查是否有其他程序占用了大量热键 ID。如果扫描结果极少，可能是消息窗口创建失败。
