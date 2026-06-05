# LAN-ClipSync（开源版）

局域网轻量级剪切板文本同步工具（Windows 10/11）。

添加双方IP后，可以实现剪切板同步，行为与 Windows 系统剪切板一致（仅同步纯文本，长度无上限），同时本工具支持快速生成二维码，方便离线用户快速输出文本或者链接至手机场景。

## 文档

| 文件 | 说明 |
|------|------|
| [docs/FEATURES.md](docs/FEATURES.md) | **功能介绍**（面向用户与贡献者） |
| [docs/REQUIREMENTS.md](docs/REQUIREMENTS.md) | 架构与实现说明 |
| [BUILD.md](BUILD.md) | Windows 编译与运行指南 |

## 快速编译（Windows）

```bat
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

可执行文件：`build\Release\LAN-ClipSync.exe`

## 说明

- 仅同步 **纯文本**，**无字符长度限制**（与系统剪切板一致）
- **TCP 点对点**，无 UDP 广播；跨 VLAN 需手动配置对端 `IP:端口`
- 仅 **局域网**，不访问互联网
- 托盘 **双击 / 右键 → 对端设置** 管理节点；**右键 → 生成二维码** 分享本机地址
