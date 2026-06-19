# 共享堆逆向分析笔记

## 目标

通过解析 `user32.dll` 的 `gSharedInfo` 内部数据结构，找到 USER 句柄表，从中识别热键类型的句柄条目，并读取热键对象的修饰键/虚拟键信息，最终映射到所属进程。

## 结论

**Win11 25H2 (Build 26200) 上此方案不可行。** 主要原因：

1. `gSharedInfo` 的内部布局在该版本已发生显著变化
2. 句柄条目中的 `phead` 指向内核 Session Space（用户态不可读）
3. 无法可靠地确定哪种句柄类型（bType）对应热键对象
4. 内部结构随每个累积更新可能变化，不具备跨版本可移植性

## 诊断记录

### gSharedInfo 结构 (Win11 25H2, x64)

地址在 user32.dll 内部，每次进程启动时因 ASLR 变化。

```
gSharedInfo 导出地址示例: 0x7FF9B7472300

gSharedInfo 内容:
+0x00: 0x0000000000B31040  ← psi（可能是句柄表地址或 SHAREDINFO 地址）
+0x08: 0x0000000002900000  ← pDispInfo（显示信息指针）
+0x10: 0x0000000000000020  ← ulSharedDelta（地址转换 delta，固定为 0x20）
+0x18: 0x0000000000B36950  ← 未知指针
+0x20: 0x0000000000B30000  ← 共享堆基地址
+0x28: 0x0000000000000318  ← 计数值 = 792
+0x30: 0x0000000000B36B30  ← 未知指针
+0x48: 0x0000000000000318  ← 计数值 = 792（与 +0x28 相同）
+0x50: 0x0000000000B36BB0  ← 可能的句柄表指针
+0x58: 0x0000000000000014  ← 句柄条目大小 = 20 字节
+0x60: 0x0000000000B36C30  ← 未知指针
```

### 句柄表条目格式

在不同地址测试了多种 entrySize（16/20/24/32 字节），结果如下：

| entrySize | 有效条目数 (60个采样) | 最佳匹配地址 |
|-----------|:---:|---|
| 16 字节 | 29 | gsi+0x00 指向的地址 |
| 20 字节 | 17 | gsi+0x00 指向的地址 |
| 24 字节 | 22 | gsi+0x00 指向的地址 |
| 32 字节 | 16 | gsi+0x00 指向的地址 |

**最有可能是 entrySize=16**，但 pOwner 只有 4 字节空间，无法容纳完整 64 位指针。这意味着 Win11 可能使用压缩格式或索引而非直接指针。

### 句柄条目结构（推测 20 字节版本）

```c
struct HANDLEENTRY {
    PVOID  phead;      // +0x00: 指向 USER 对象（内核 Session Space 地址）
    PVOID  pOwner;     // +0x08: 指向 THREADINFO（用户态可读）
    BYTE   bType;      // +0x10: 对象类型
    BYTE   bFlags;     // +0x11: 标志位
    WORD   wUniq;      // +0x12: 唯一性计数器
    // 共 20 字节（0x14）
    // 或 16 字节版本：pOwner 可能是 4 字节索引
};
```

### 类型分布（gsi+0x00, entrySize=20, 60个采样）

```
类型 0:   1 个  ← 可能是 WINDOW 类型（但太少，预期应有上百个）
类型 8:   1 个
类型 160: 2 个
类型 64:  2 个
类型 224: 1 个
... (共 14 种类型)
```

类型分布与预期不符（窗口类型应该有数百个条目），证实读取位置可能不完全正确。

### pOwner → 进程映射

虽然无法读取 phead 指向的对象，但 pOwner 指向的 THREADINFO 是用户态可读的。成功从中提取了一些线程 ID：

```
通过 OpenThread → GetProcessIdOfThread 调用链：
  THREADINFO 中的 idThread 字段疑似在 +0x48~+0x60 偏移范围内
```

但无法确定这些线程对应的句柄条目是否属于热键类型。

## 为什么内核态无法读取

64 位 Windows 的地址空间布局：

```
0x0000000000000000 - 0x00007FFFFFFFFFFF : 用户态
0xFFFF800000000000 - 0xFFFFFFFFFFFFFFFF : 内核态（包括 Session Space）
```

`phead` 指针的值范围在 `0xFFFF800000000000` 以上，属于内核 Session Space。用户态进程即使将共享堆映射到自己的地址空间，也无法通过 `phead` 直接访问对象数据。

## 替代方案

1. **暴力扫描**（✅ 已采用）：遍历 RegisterHotKey 检测占用
2. **键盘钩子**（✅ 已采用）：WH_KEYBOARD_LL 识别进程
3. **内核驱动**（❌ 未采用）：需要签名证书，且 Windows 11 强制要求 WHQL 签名
4. **等待社区逆向成果**：关注 ReactOS、WinDbg 社区对 win32k.sys 的逆向进展

## 参考

- ReactOS `win32k` 源码 (ReactOS-0.4.15)
- Vergilius Project (Windows 内核结构数据库)
- "Windows 10 x64 Handle Table" — UnknownCheats 论坛
