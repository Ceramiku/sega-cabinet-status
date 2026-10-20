# 机台部署

## 通用步骤

1. 正常退出游戏并确认 `amdaemon.exe` 已结束，备份原启动脚本及插件配置。
2. 根据宿主进程位数选择 Release 中的 `dist/x64` 或 `dist/x86`，将 DLL 和 INI 放到游戏目录。
3. 在原本注入 `amdaemon.exe` 的命令中追加 `-k cabinet-status.dll`，放在进程名之前，并保留原有 Hook 在前。
4. 按原方式启动游戏，从本机访问 `http://127.0.0.1:18080/debug` 和 `/status`。
5. 从 Bot 所在环境用机台实际 IP 验证访问，再验证 Bot 指令。

同一台电脑只需让目标 AMDaemon 加载一份本插件。将同一个监听端口的插件再注入游戏进程可能发生端口冲突。

根目录的虚拟磁盘挂载、硬件等待流程按原设置运行。注入器、游戏 Hook 和游戏文件由原有机台环境提供。

## 舞萌

原 AMDaemon 命令中的 Hook 顺序应类似：

```text
inject -d -k mai2hook.dll -k cabinet-status.dll amdaemon.exe
```

以上只展示注入部分；实际命令继续保留原有 `-f -c` 及 JSON 参数。

根目录 `start.bat` 和 `run-amdaemon.bat` 是以下布局的模板：`amdaemon.exe`、`Sinmai.exe`、`inject.exe`、`mai2hook.dll`、配置 JSON 和本插件全部在同一目录。

模板会检查文件、拒绝重复启动 AMDaemon、保存注入器输出，并在 Sinmai 退出后结束 AMDaemon。复制前核对模板中的 JSON 文件名、游戏启动参数及退出行为是否适合当前机台。使用模板时两个 BAT 必须一起复制。

## 音击

在现有启动脚本中，只给 AMDaemon 那一条命令追加本插件，注入部分类似：

```text
inject -d -k mu3hook.dll -k cabinet-status.dll amdaemon.exe
```

继续保留原有 JSON 参数，以及后续 `mu3` 游戏命令的 Hook、窗口、分辨率和其他参数。音击使用自己的启动脚本；仓库中的两个 BAT 是舞萌模板。

建议首次验证时设置 `GuestRadioOffSeconds=0`，先检查待机、刷卡、续关和退场，再判断游客启发式是否适用。

脚本进入自身所在目录时使用：

```bat
pushd "%~dp0"
```

引号使包含空格的路径仍作为一个目录参数处理。

## 中二及其他机种

需先确认 AMDaemon 注入方式、进程位数、原有 Hook 链，以及读卡请求是否经过被观察的导入调用。当前没有中二的单独实机验证记录，不应仅凭成功加载就确认状态规则适用。

## 升级到 0.2.2

保留原启动脚本，用同位数 DLL 替换旧文件，并将现有 INI 的 `[Cabinet]` 段设置为：

```ini
IdleTimeoutSeconds=0
```

保留现有端口、地址和游客配置。重启后 `/debug` 应显示 `version=0.2.2`、`occupied_timeout_seconds=0`。旧 DLL 不支持用 0 禁用占用超时，DLL 和配置需要一起更新。

## 日志和排查

- `cabinet-status.log` 位于 DLL 所在目录；不能写入时尝试运行用户的 `%TEMP%/cabinet-status-fallback.log`。
- 使用舞萌模板时，注入器输出保存在 `inject-cabinet.log`，退出后写入 `inject-cabinet.exit.txt`。
- 没有日志时先检查注入命令、文件路径、进程位数和依赖。
- HTTP 监听错误 10048 通常是端口占用，10013 通常是绑定被拒绝。
- 初始 HTTP 503 正常；待机后应看到 `polls`、`valid_frames` 增长并进入 `free`。
- `hooks` 大于零只能证明装上了部分拦截，不能单独证明读卡器信号已采集。

## 网络

`0.0.0.0` 是监听地址，客户端填写机台实际 LAN IP。通过 `ipconfig` 查看实际网卡地址，不根据 segatools 的模拟网络配置推测。

本机可访问而 Bot 访问超时时，检查 Windows 入站 TCP 18080、设备间路由和容器网络。防火墙来源范围按 Bot 的实际地址设置；本服务没有认证，限定在需要查询的本地网络内使用。
