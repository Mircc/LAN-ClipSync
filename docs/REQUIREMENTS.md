# 局域网轻量级剪切板同步工具 (LAN-ClipSync) - 开发架构文档

> 本文档为开源版架构说明。用户向功能介绍见 **[FEATURES.md](FEATURES.md)**。实现状态见文末「实现对照」。

## 1. 项目概述与核心约束

本项目是一个基于 Windows 平台的 C++ 局域网剪切板实时同步应用程序（**开源版**）。

* **开发语言**：C++ (推荐 C++17 或以上标准)
* **目标平台**：Windows 10/11 (调用原生 Win32 API)
* **核心功能**：实现局域网内多台设备间的剪切板文本实时同步。
* **严格约束**：
  1. **仅支持文本**：当前版本仅处理文本格式（`CF_UNICODETEXT` 或 `CF_TEXT`），严格拒绝文件（`CF_HDROP`）、图片（`CF_DIB`）等其他格式。
  2. **无长度限制**：文本长度与 Windows 系统剪切板一致，不做 artificial 截断（开源版与内部 200 字符限制版不同）。
  3. **网络环境**：完全在局域网（LAN）内运行，不可包含任何外部互联网请求。

## 2. 系统架构与模块划分

系统采用**事件驱动**与**多线程**架构，主要划分为以下三个核心模块：

### 2.1 剪切板管理模块 (Clipboard Manager)

* **职责**：负责监听本地剪切板变化，以及将网络接收到的数据写入本地剪切板。
* **核心 API**：
  * 使用较新的 `AddClipboardFormatListener` 进行事件注册（避免使用老旧的 `SetClipboardViewer` 导致链条断裂）。
  * 响应 `WM_CLIPBOARDUPDATE` 消息。
  * 读写操作：`OpenClipboard`, `GetClipboardData`, `EmptyClipboard`, `SetClipboardData`, `CloseClipboard`。
* **防无限回环设计 (CRITICAL)**：
  * 当程序主动调用 `SetClipboardData` 写入数据时，也会触发 `WM_CLIPBOARDUPDATE`。
  * **必须设计内部状态锁或文本哈希校验**：在读取到剪切板变化时，对比当前文本与“最后一次通过网络接收的文本”，如果相同则忽略，避免 A -> B -> A -> B 的无限广播风暴。

**代码位置**：`src/ClipboardManager.h`, `src/ClipboardManager.cpp`

### 2.2 网络通信模块 (Network Manager)

采用 **TCP 点对点 (Data Transfer)**，**不使用 UDP 广播**（避免企业多 VLAN 环境下的广播风险）：

* **对端配置（手动）**：
  * 用户在「对端设置」窗口中填写对方 `IP:端口`；本机监听地址显示在窗口顶部供复制告知对方。
  * 双方必须**互相添加**对端地址；配置持久化至 `LAN-ClipSync-peers.json`。
* **数据传输 (TCP Socket)**：
  * 本机监听端口在 `45000–65000` 内随机选取（首次），写入配置文件后重启沿用。
  * 剪切板变更时，向已配置的对端建立 TCP 连接并发送数据（出站连接，便于跨网段）。

**代码位置**：`src/NetworkManager.h`, `src/NetworkManager.cpp`

### 2.3 协调与界面模块 (Coordinator & UI)

* **职责**：初始化各模块，维护后台托盘图标（System Tray），提供退出功能和简单的网络状态提示。
* **注意**：网络 IO 必须在独立线程运行，绝不能阻塞主线程（UI/消息循环线程），否则会导致系统剪切板卡死。

**代码位置**：`src/Application.h`, `src/Application.cpp`, `src/main.cpp`

### 2.4 二维码工具模块 (QR Tool)

* **职责**：离线将文本（如本机 `IP:端口`）编码为 QR 码并显示，辅助对端配置；不依赖外网。
* **入口**：托盘右键菜单 **生成二维码** → `QrDialog`（模态窗口）。
* **生成流程**：文本框输入 → 防抖定时器（约 350ms）→ `QrRender::renderQrToBitmap` → 位图显示；打开时可选读取剪切板预填。
* **编码库**：使用 [Project Nayuki QR Code generator library](https://www.nayuki.io/page/qr-code-generator-library)（MIT），文件 `src/qrcodegen.cpp`、`src/qrcodegen.hpp`；纠错级别 `Ecc::MEDIUM`。
* **解码（源码）**：`src/QrDecoder.cpp` 提供离线图片解码能力，供后续扩展；当前 UI 未单独暴露。

**代码位置**：`src/QrDialog.*`, `src/QrRender.*`, `src/qrcodegen.*`, `src/QrDecoder.*`

## 3. 数据协议规范

局域网内通信使用轻量级的 JSON 格式（或简单的二进制 Header+Payload）。建议使用 JSON 以便后续扩展。

**TCP 数据包格式示例（UTF-8 编码）：**

```json
{
  "version": "1.0",
  "type": "text",
  "length": 15,
  "payload": "Hello LAN Sync!",
  "timestamp": 1698765432
}
```

* **开发者要求**：解析 `payload` 时使用动态字符串，不对文本长度做 artificial 上限；超大 payload 仍受 TCP 帧与系统内存约束。

**实际传输**：TCP 使用 **4 字节大端长度头 + JSON 正文** 帧格式（见 `NetworkManager::sendFramed` / `recvFramed`）。

**对端配置文件示例（`LAN-ClipSync-peers.json`）：**

```json
{
  "listen_port": 52341,
  "peers": [
    {"ip": "10.20.30.40", "port": 51888}
  ]
}
```

## 4. 异常处理与安全策略

1. **剪切板独占异常**：Windows 剪切板是全局共享资源，可能会被其他程序短暂占用导致 `OpenClipboard` 失败。需要实现**重试机制**（如间隔 50ms 重试 3 次）。
2. **内存泄露防护**：操作剪切板内存时，必须严格遵守 `GlobalAlloc`、`GlobalLock` 和 `GlobalUnlock` 的使用规范。传入 `SetClipboardData` 的内存句柄由系统接管，无需手动释放；但读取时的内存不可随意释放。
3. **防火墙配置**：程序启动时由于涉及 UDP 监听和 TCP 服务，需要提示用户在 Windows Defender 防火墙中允许其通过局域网通信。

## 5. 开发步骤指导 (Action Items)

1. 先搭建 Win32 隐藏窗口及消息循环，实现系统托盘图标（NotifyIcon）。
2. 实现 `ClipboardManager` 类，封装监听、读取、写入逻辑，并实现防回环校验。
3. 实现 `NetworkManager` 类，利用 Winsock2 (WS2_32.lib) 完成 UDP 节点发现和 TCP 文本发送。
4. 在主程序中桥接网络与剪切板事件：`OnLocalClipboardChanged` 触发网络发送，`OnNetworkDataReceived` 触发剪切板写入。
5. 提供完整的 CMakeLists.txt 或 Visual Studio 编译指南。

## 6. 编译与运行

详见项目根目录 **`BUILD.md`**。

## 7. 待讨论 / 后续版本

* **端到端加密**：当前 v1.0 为明文传输（局域网内可被嗅探）。若需 AES-GCM 或预共享密钥，建议在 v1.1 扩展协议字段，见下方备注。
* **可配置端口**、**开机自启**、**节点列表 UI** 等均未在 v1.0 实现。

### 加密扩展备注（未实现）

若在信任的局域网环境下先以实现基础互通为最优先，可保持现状。若加入加密，建议：

- UDP 发现包增加 `key_id` 或能力协商字段；
- TCP `payload` 改为 Base64(AES-GCM(nonce + ciphertext))；
- 各节点配置相同预共享密钥（需 UI 或配置文件）。

---

## 实现对照（截至当前代码库）

| 需求项 | 状态 | 说明 |
|--------|------|------|
| Win32 隐藏窗口 + 消息循环 | ✅ | `HWND_MESSAGE` |
| 系统托盘 | ✅ | 双击状态 / 右键退出 |
| ClipboardManager | ✅ | `AddClipboardFormatListener` |
| 防回环 | ✅ | `lastNetworkText_` + `suppressEcho_` |
| 仅文本 / 无长度限制 | ✅ | 拒绝 HDROP/DIB 等；开源版不截断 |
| UDP 广播发现 | ❌ | v2 改为手动对端 IP（企业 VLAN 友好） |
| TCP 点对点传输 | ✅ | 随机监听端口 + 长度前缀 JSON |
| 对端设置 UI | ✅ | 托盘菜单 / 双击打开 |
| 二维码生成 | ✅ | 托盘「生成二维码」；Nayuki qrcodegen |
| 网络独立线程 | ✅ | UDP / 发现 / TCP accept |
| OpenClipboard 重试 | ✅ | 50ms × 3 |
| 防火墙提示 | ✅ | 首次 MessageBox（注册表 `HKCU\Software\LAN-ClipSync`） |
| 无外网 | ✅ | 仅 LAN |
| CMake 构建 | ✅ | `CMakeLists.txt` |
| 端到端加密 | ❌ | v1.1 候选 |

## 8. 项目目录结构

```
LAN-ClipSync/
├── CMakeLists.txt
├── BUILD.md                 # Windows 编译指南
├── docs/
│   ├── FEATURES.md          # 功能介绍
│   └── REQUIREMENTS.md      # 本文件
└── src/
    ├── main.cpp
    ├── Application.*
    ├── ClipboardManager.*
    ├── NetworkManager.*
    ├── QrDialog.* / QrRender.* / QrDecoder.*
    ├── qrcodegen.*          # Nayuki QR 编码库 (MIT)
    ├── JsonUtil.*
    └── Logger.*
```

## 9. 默认网络参数

| 参数 | 值 |
|------|-----|
| TCP 监听端口 | 45000–65000 随机（首次），写入配置后固定 |
| 对端配置 | 与 exe 同目录 `LAN-ClipSync-peers.json` |
| 最大文本长度 | 无 artificial 限制（与系统剪切板一致） |
| 日志文件 | 与 exe 同目录 `LAN-ClipSync.log` |
