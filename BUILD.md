# LAN-ClipSync 编译与运行指南

需求与架构全文见：**[docs/REQUIREMENTS.md](docs/REQUIREMENTS.md)**  
功能介绍见：**[docs/FEATURES.md](docs/FEATURES.md)**

## 环境要求

| 项目 | 要求 |
|------|------|
| 操作系统 | **Windows 10 / 11**（64 位，必须） |
| 架构 | 本程序为 **x64**，32 位 Windows 无法运行 |
| 编译器 | Visual Studio 2019/2022（含 C++ 桌面开发）或 Build Tools |
| CMake | 3.16 及以上 |
| 依赖库 | **无第三方库**（仅系统 Win32 + Winsock2） |

> **无法在 Ubuntu 上运行或完整测试**：本项目使用 `AddClipboardFormatListener`、系统托盘等 Win32 API，必须在 Windows 主机上编译运行。

## 方式一：Visual Studio（推荐）

1. 安装 [Visual Studio 2022 Community](https://visualstudio.microsoft.com/zh-hans/downloads/)
   - 工作负载勾选：**使用 C++ 的桌面开发**
2. 安装 [CMake](https://cmake.org/download/)（若 VS 安装器未包含）
3. 打开 **x64 Native Tools Command Prompt for VS**，进入项目目录：

```bat
cd path\to\LAN-ClipSync
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

可执行文件路径：

```
build\Release\LAN-ClipSync.exe
```

程序图标与版本信息见 `res/`（产品名：**局域网剪切板同步助手**，版本 **1.0**）。在资源管理器中右键 exe → **属性 → 详细信息** 可查看。

### 重新生成多尺寸图标

将各尺寸 PNG 放入 `res/icon_sizes/`（命名 `icon_16x16.png` … `icon_256x256.png`），然后执行：

```bat
tools\BuildIcon.exe res\icon_sizes res\LAN-ClipSync.ico
cmake --build build --config Release
```

`LAN-ClipSync.ico` 内嵌 **16 / 24 / 32 / 48 / 64 / 128 / 256** 像素，供 Windows 11 按场景（资源管理器、任务栏、托盘、高 DPI）自动选用。

## 方式二：CMake + Ninja

```bat
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## 使用说明

1. 在需要同步的 Windows 电脑上分别运行 `LAN-ClipSync.exe`
2. 首次启动会提示防火墙（仅 TCP，**无 UDP 广播**）
3. 打开 **对端设置**（托盘右键或双击图标）：
   - 将窗口中的 **本机监听地址**（`IP:端口`）告知对方
   - 添加对方的 **`IP:端口`**（跨 VLAN / 跨网段均可，需网络路由可达）
   - **双方必须互相添加**对端地址
4. 复制 **纯文本** 即可同步（**无字符长度限制**，与系统剪切板一致）
5. 托盘：**右键 → 对端设置 / 生成二维码 / 退出**
6. 配置文件与 exe 同目录：`LAN-ClipSync-peers.json`（对端与监听端口自动保存；支持 **计算机名** 持久化，域环境 IP 变化仍可同步）
7. **约定端口**：本机监听与对端探测优先使用 `12123`、`12345`、`12306`，均不可用后再随机端口
8. 对端设置：输入 `计算机名` 或 `IP` 可省略端口；**双击已连接行**删除，**双击未连接行**自动探测端口；回车或双击空白处添加
9. **IPv4 + IPv6 双栈**监听/连接；本机地址复制格式：`IPv4:端口;[IPv6]:端口;计算机名:端口`（英文分号），粘贴时可整行解析并优先保存计算机名

## 内存占用说明

任务管理器中的「内存」多为**工作集**，含 Windows 加载的 DLL（`ws2_32`、`comctl32` 等），托盘 + Winsock + C++ 运行时很难压到 **5MB 以内**。

已做优化：去掉周期连接探测（启动时探测一次 + 手动刷新）、工作线程小栈、延迟初始化 UI 字体、日志去掉 `iostream`、Release `/Os`。

若可接受目标机安装 [VC++ 运行库](https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist)，可用动态运行时进一步降低常驻内存：

```bat
cmake -B build -DLAN_CLIPSYNC_STATIC_RUNTIME=OFF
cmake --build build --config Release
```

典型预期（视系统与对端数量而定）：优化后约 **12–18MB** 工作集；静态 `/MT` 通常比动态 `/MD` 高约 **2–5MB**。
7. 日志：`LAN-ClipSync.log`

## 网络说明

- **协议**：TCP + 4 字节长度头 + JSON 正文（点对点，不广播）
- **监听端口**：首次启动在 `45000–65000` 随机选取并写入配置；重启沿用已保存端口
- 企业多 VLAN 环境请由管理员保证两端 IP 互通，并在防火墙放行对应 TCP 端口

## 虚拟机 / 另一台电脑无法启动？

常见原因与处理：

| 现象 | 原因 | 处理 |
|------|------|------|
| 双击无反应或提示缺少 `VCRUNTIME140.dll` | 未安装 VC++ 运行库 | 使用本项目 **Release + 静态运行库** 重新编译（见 `CMakeLists.txt`），或安装 [VC++ 2015–2022 x64 运行库](https://learn.microsoft.com/zh-cn/cpp/windows/latest-supported-vc-redist) |
| 提示“不是有效的 Win32 应用程序” | 系统是 32 位，或误用 x86 构建 | 在 x64 Windows 上使用 `cmake -A x64` 编译 |
| 闪退无提示 | 系统低于 Windows 10 | 需 Windows 10 及以上（使用了 `AddClipboardFormatListener` 等 API） |
| 能启动但无法同步 | 网络/防火墙 | 放行 TCP 入站，双方互相配置对端 `IP:端口` |

将 `LAN-ClipSync.exe` 复制到虚拟机时，建议同时复制同目录下的 `LAN-ClipSync-peers.json`（若已配置）。日志在虚拟机 exe 同目录的 `LAN-ClipSync.log`。

## Ubuntu 开发者说明

当前 Cursor 环境为 Ubuntu，只能编写与提交源码，**不能本地链接运行**。请将 `LAN-ClipSync` 文件夹复制到 Windows 机器后按上文编译。

可选：在 Ubuntu 安装 MinGW 仅做交叉编译（仍不能在 Linux 上运行 exe）：

```bash
sudo apt install mingw-w64 cmake
# 需要单独配置 Windows 版 CMake toolchain，一般不如直接在 Windows 编译省事
```
