# HotKeySight

Windows 全局热键检测器 — 检测系统中被占用的全局快捷键，识别进程归属。

## 功能

- **暴力扫描** — 遍历 16 种修饰键 × 104 个虚拟键 = 1664 个组合，通过 `RegisterHotKey` API 检测占用
- **实时监控** — `WH_KEYBOARD_LL` 键盘钩子 + `SetWinEventHook` 前台/新窗口检测，识别热键归属进程
- **Windows 内置快捷键数据库** — 收录 100+ 系统快捷键（Win+* / Alt+* / Ctrl+* 等）
- **进程名友好映射** — 80+ 常用软件文件名 → 中文名称
- **持久化** — 扫描结果和监控数据自动保存，下次启动加载
- **深色/浅色模式**
- **系统托盘**

## 构建

### 依赖

- [EUI-NEO](https://github.com/sudoevolve/EUI-NEO) UI 框架 (v0.5.0+)
- MinGW-w64 (g++ 16.1.0+) 或 MSVC 2022
- CMake 3.14+, Ninja

### 编译

```bash
# 1. 将 src/gui_app.cpp 复制到 EUI-NEO/examples/hotkey_detector.cpp
# 2. 在 EUI-NEO 目录中构建
cd EUI-NEO
mkdir -p build && cd build
cmake .. -G Ninja -DEUI_BUILD_APPS=ON
ninja hotkey_detector

# 3. 将 build/hotkey_detector.exe 和 assets/ 复制到同一目录即可运行
```

### CLI 版（无 GUI 依赖）

```bash
g++ -o hotkey-detector.exe src/main.cpp -static -luser32 -lgdi32 -ladvapi32 -O2
```

## 技术方案

| 功能 | 方案 |
|------|------|
| 热键检测 | `RegisterHotKey` 暴力枚举 |
| 进程识别 | `SetWinEventHook` 监听前台切换 + 新窗口创建 |
| 键盘监控 | `WH_KEYBOARD_LL` 低层键盘钩子 |
| 注册表查询 | `HKCU\Control Panel\Input Method\Hot Keys` 等 |
| UI | EUI-NEO (OpenGL + GLFW) |

### 为什么不用共享堆(gSharedInfo)？

Win11 25H2 (Build 26200) 上 `gSharedInfo` 内部结构已大幅变化，句柄表条目指向内核 Session Space，用户态无法直接读取热键对象。详见 `docs/shared-heap-analysis.md`。

## 项目结构

```
HotKeySight/
├── README.md
├── src/
│   ├── gui_app.cpp      # GUI 版主程序 (EUI-NEO)
│   ├── main.cpp          # CLI 版 (独立)
│   └── hook_dll.cpp      # DLL 注入实验 (不稳定)
├── assets/               # 图标、字体等
└── docs/
    ├── shared-heap-analysis.md
    └── build-guide.md
```

## License

MIT
