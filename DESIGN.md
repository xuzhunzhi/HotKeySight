# HotKeySight - 热键检测工具

## 项目概述

HotKeySight 是一款 Windows 原生应用，用于检测和显示系统中已注册的热键，支持查看热键冲突。

## 技术栈

- **框架**: WinUI3 (Windows App SDK 1.8)
- **语言**: C# / .NET 8
- **平台**: Windows 10 (Build 17763+)
- **IDE**: Visual Studio 2022/2024

## 功能设计

### 核心功能

1. **按应用查看** - 列出所有已注册热键的应用，选择后显示该应用的所有热键
2. **热键检测** - 按下任意组合键，实时显示哪个应用注册了该热键
3. **热键冲突** - 显示被多个应用同时注册的热键

### UI 布局

- **左侧导航栏** (200px)
  - 按应用查看
  - 热键检测
  - 热键冲突

- **内容区域**
  - 双栏布局 (应用列表 + 热键详情)
  - 卡片式设计，圆角 8px
  - Mica 模糊背景

## 项目结构

```
HotKeySight/
├── HotKeySight.slnx
├── HotKeySight/
│   ├── App.xaml / App.xaml.cs
│   ├── MainWindow.xaml / MainWindow.xaml.cs
│   ├── HotKeySight.csproj
│   ├── Helpers/
│   │   └── BoolToVisibilityConverter.cs
│   ├── Pages/
│   │   ├── ByAppPage.xaml / ByAppPage.xaml.cs
│   │   ├── ByHotKeyPage.xaml / ByHotKeyPage.xaml.cs
│   │   └── ConflictsPage.xaml / ConflictsPage.xaml.cs
│   └── Assets/
└── .git/
```

## 待实现功能

- [ ] 热键扫描服务 - 枚举系统中所有已注册的热键
- [ ] 全局热键监听 - 使用 RegisterHotKey API
- [ ] 应用图标加载 - 从进程获取图标
- [ ] 冲突检测逻辑 - 检测重复注册的热键

## API 参考

- `RegisterHotKey` / `UnregisterHotKey` - 注册/注销全局热键
- `GetForegroundWindow` - 获取前台窗口
- `EnumWindows` - 枚举所有窗口

## 进度

- [x] 项目创建 (VS + Git)
- [x] UI 框架 (3个页面)
- [ ] 热键扫描核心功能
- [ ] 热键检测功能
- [ ] 冲突检测功能
