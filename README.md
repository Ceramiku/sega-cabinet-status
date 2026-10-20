# Sega Cabinet Status

Windows 机台状态采集 DLL，通过 HTTP 向 Bot 等客户端提供空闲、占用和未知状态。

当前版本 **0.2.2**。部署者已在舞萌、音击上测试并反馈目前未发现明显问题；这不代表所有游戏版本、游客和双人组合都已验证。中二尚无单独的实机验证记录。

## 下载与部署

从 [Releases](https://github.com/Ceramiku/sega-cabinet-status/releases) 下载发布压缩包。`dist/x64`、`dist/x86` 分别对应 64 位和 32 位宿主进程，按 `amdaemon.exe` 的位数选择。

将 `cabinet-status.dll`、`cabinet-status.ini` 放到游戏目录，在原 AMDaemon 注入命令中追加 `-k cabinet-status.dll`，保留原有游戏 Hook、配置和启动参数。舞萌使用 `mai2hook.dll`，音击使用 `mu3hook.dll`。

```text
机台上的 AMDaemon + cabinet-status.dll
                  ↓ HTTP /status
              Bot / 查询客户端
```

详细操作、音击与舞萌启动差异见 [部署说明](docs/deployment.md)。仓库根目录的 `start.bat`、`run-amdaemon.bat` 是舞萌同目录布局模板，使用前核对原有参数。

## HTTP 接口

默认监听 `0.0.0.0:18080`，支持 GET 和 HEAD。客户端使用机台实际 LAN IP 访问。

| 路径 | HTTP 状态 | 内容 |
| --- | --- | --- |
| `/status` | 200 | `{"status":"free"}` 或 `{"status":"occupied"}` |
| `/status` | 503 | `{"error":"state_unavailable","state":"unknown"}` |
| `/debug` | 200 | 版本、状态原因、计数和当前超时配置等诊断信息 |

HTTP 503 表示当前无法判断状态；连接失败表示进程、地址或网络不可达，两者应分别处理。Bot 接入示例见 [Bot 集成](docs/bot-integration.md)。

## 状态判断

DLL 观察同一宿主进程内的 NFC 串口写入和可识别的网络请求线索，不读取游戏内存。状态属于启发式判断。

- 启动时为 `unknown`；连续至少 3 次有效 POLL 后进入 `free`，相邻间隔不超过 1 秒。
- 卡片操作或可识别的网络登录线索确认 `occupied`。
- 占用后至少保持 45 秒；恢复连续至少 8 次 POLL、跨度至少 1.5 秒，才解除占用。
- 射频开关可以穿插在 POLL 之间，但开关本身不计作 POLL。
- 曾确认空闲后，射频持续关闭 6 秒可作为游客占用线索；不适用时将 `GuestRadioOffSeconds` 设为 0。
- 默认 `IdleTimeoutSeconds=0`，连续续关期间没有新刷卡信号也保持占用，等待真正恢复待机轮询。
- 空闲状态超过 60 秒没有有效信号则回到 `unknown`。

取消占用超时后，采集链路故障但 HTTP 仍在线也可能保留旧占用。需要超时策略时可将 `IdleTimeoutSeconds` 设为正数秒数。

## 配置

配置文件与 DLL 放在同一目录；修改后重启宿主进程。

| 段 / 键 | 默认值 | 含义 |
| --- | --- | --- |
| Server / Enabled | 1 | 启动 HTTP 服务；0 时仍采集并记录状态 |
| Server / Port | 18080 | HTTP 端口 |
| Server / BindAddress | 0.0.0.0 | IPv4 监听地址 |
| Cabinet / MinOccupiedHoldSeconds | 45 | 占用后最少保持秒数 |
| Cabinet / GuestRadioOffSeconds | 6 | 游客射频关闭线索延时；0 禁用 |
| Cabinet / IdleTimeoutSeconds | 0 | 占用信号过期秒数；0 禁用，正数范围 1～86400 |
| Cabinet / SignalTimeoutSeconds | 60 | 空闲信号过期秒数 |
| Cabinet / ReaderAddress | 0 | NFC 请求帧中的读卡器地址，与 COM 端口号不同 |

本服务没有认证。部署时通过路由和 Windows 防火墙将访问范围限定到需要查询的本地网络。

## 构建与测试

源码位于 `src/`，测试位于 `tests/`。仓库不包含游戏文件、segatools、inject.exe 或编译器。

### CMake / Visual Studio

需要 Windows、CMake 3.20+、Visual Studio C++ 桌面构建工具：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1
```

默认构建并测试 x64、Win32，成功后生成 `dist/`。此构建路径已提供，但当前发布环境尚未实际执行。

### TinyCC

发布二进制使用 TinyCC 0.9.27 原生 Windows 编译器及对应完整 WinAPI 头文件包。工具链由使用者从 [TinyCC 官方目录](https://download.savannah.gnu.org/releases/tinycc/) 获取。

```text
工具链根目录/
  win64/tcc/tcc.exe
  win32/tcc/tcc.exe
```

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Compiler Tcc -ToolchainRoot "工具链根目录" -WinApiInclude "完整头文件目录\include\winapi"
```

脚本先检查核心状态机，再分别运行默认、显式禁用和正数占用超时的 DLL 宿主测试；全部通过后复制到 `dist/`。测试只向临时普通文件写入模拟协议数据，不需要真实游戏或读卡器。构建测试默认使用回环地址及 28201/28202、28211/28212、28221/28222 端口。

验证范围和限制见 [验证说明](docs/verification.md)，版本变化见 [CHANGELOG](CHANGELOG.md)。

## 参考项目

- [segatools](https://github.com/djhackersdev/segatools)：NFC 协议格式和游戏 Hook 使用方式。
- [capnhook](https://github.com/decafcode/capnhook)：注入器参数及重复 `-k` 的用法。
- [TinyCC](https://download.savannah.gnu.org/releases/tinycc/)：发布构建所用工具链。
