# Bot 集成

机台 DLL 提供 HTTP 服务，Bot 主动查询。DLL 留在机台，Python / Docker 只需 HTTP 客户端。

## 接口约定

| 场景 | HTTP | JSON |
| --- | --- | --- |
| 空闲 | 200 | `{"status":"free"}` |
| 占用 | 200 | `{"status":"occupied"}` |
| 无法判断 | 503 | `{"error":"state_unavailable","state":"unknown"}` |

使用 `/status`；`/debug` 的 `state` 字段用于诊断。把网络不可达、HTTP 503 和正常空闲分别处理，避免将故障显示为空闲。

## 已有 lounge.py 插件

如果插件已经有 `CABINETS_CONFIG`、通用查询函数和 `有无` 指令注册器，在配置字典中增加：

```python
    "舞萌": {
        "aliases": ["舞萌", "舞萌DX", "maimai", "mai"],
        "urls": ["http://舞萌机台IP:18080/status"],
    },
    "音击": {
        "aliases": ["音击", "ongeki", "mu3"],
        "urls": ["http://音击机台IP:18080/status"],
    },
```

中文地址占位替换成实际 LAN IP。同机种多台时，在对应的 `urls` 列表继续添加地址。保存并更新运行中的插件，重启 Bot 后查询“舞萌有无”“音击有无”。此示例依赖已有插件的配置读取及注册逻辑，并非独立的 NoneBot 插件。

以后增加其他机种也采用同样格式；其状态采集工具需先提供兼容接口。

## 从 Bot 环境检查连通性

在 Bot 所在系统执行，替换地址：

```sh
python -c "import urllib.request; print(urllib.request.urlopen('http://机台IP:18080/status', timeout=2).read().decode())"
```

Docker 部署时，在宿主机执行，替换容器名和地址：

```sh
docker exec Bot容器名 python -c "import urllib.request; print(urllib.request.urlopen('http://机台IP:18080/status', timeout=2).read().decode())"
```

HTTP 503 会使该诊断命令抛出 HTTPError，表示服务可达但状态未知。连接超时或拒绝连接则检查地址、服务进程和网络。

Bot 在 PVE 虚拟机内的 Docker 中时，请求方向是容器 → 虚拟机 → 机台。先确认虚拟机能访问机台，再检查容器。普通 Docker bridge 通常支持这种出站请求，不需要为 Bot 映射入站端口。

`127.0.0.1` 指向发请求的电脑或容器；访问另一台机台时使用它的实际 IP。多网段环境需要路由互通。
