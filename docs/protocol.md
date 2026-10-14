# 本地流量快照协议

此文档固定后台和 Qt 桌面界面的实现契约。接口仅用于本机 Unix socket，不使用 HTTP。

## 传输与权限

- 后台监听 Unix socket；客户端只读取，不发送控制命令。
- 每次连接立即获得当前完整快照，之后约每秒获得一行完整快照。
- 编码为 UTF-8 NDJSON，每行一个 JSON 对象。连接不是帧边界。
- GUI 默认连接 `/run/ball-<当前用户名>/gui.sock`，允许通过启动参数指定其他路径。
- 正式部署由 systemd 创建 root 所有、目标用户组可进入的运行目录；socket 为 `0660`。
- 慢客户端必须被断开，不能阻塞采集或无限积压数据。GUI 重连后以最新快照覆盖状态，不累加快照。
- 此接口不输出连接、域名、报文内容；只输出流量、进程标识、接口和数据质量。

## 快照

```json
{
  "version": 1,
  "kind": "snapshot",
  "started": "2026-10-14T14:30:00+08:00",
  "at": "2026-10-14T14:30:02+08:00",
  "interfaces_available": true,
  "interface_error": "",
  "process_status": {
    "available": true,
    "stale": false,
    "incomplete": false,
    "error": "",
    "notes": []
  },
  "interfaces": [
    {
      "index": 2,
      "name": "eno1",
      "kind": "physical",
      "up": true,
      "today": {"rx_bytes": "2048", "tx_bytes": "512"},
      "session": {"rx_bytes": "2048", "tx_bytes": "512"},
      "rate": {"rx_bps": 1024.0, "tx_bps": 256.0}
    }
  ],
  "processes": [
    {
      "pid": 1234,
      "start_ns": "9007199254740993",
      "name": "firefox",
      "present": true,
      "today": {"rx_bytes": "1024", "tx_bytes": "128"},
      "session": {"rx_bytes": "1024", "tx_bytes": "128"},
      "rate": {"rx_bps": 512.0, "tx_bps": 64.0}
    }
  ]
}
```

- `started` 是本次后台监测起点，后台重启后改变，GUI 重启不改变。
- `at` 是生成快照的时刻。两者使用带时区的 ISO 8601 / RFC 3339 字符串。
- `rate` 的 `bps` 指 **bytes per second**，不是 bits per second；为有限、非负 JSON 数字。
- 所有字节计数及 `start_ns` 使用非负十进制字符串，避免 JSON/界面转换损失 64 位精度。PID、网卡索引为正整数。
- `kind` 只取 `physical`、`virtual`、`loopback`。网卡和进程的数组顺序不是身份。
- `today` 是本地日期内已观测累计；`session` 是本次监测已观测累计。首次监测前视为零，不从系统已有计数补算历史。
- 缺席进程及已移除接口可以保留累计行，`present` / `up` 为 false、速率为零。

## 界面解释

- 流量条只合计用户选定网卡。首次使用默认选择当前启用的物理网卡，不自动把 VPN、回环和虚拟接口加入。
- 进程统计是应用 socket I/O，网卡统计是内核接口计数，不要求两者相等，不互相相加。
- `interfaces_available=false` 或 GUI 没有新鲜快照时，不能把过期速率显示成真实的零；应显示不可用状态。
- 进程页是否实时可用须同时检查 `available` 和 `!stale`；不完整标记及原因可见。
- 快照只在 GUI 内保留最新一份，流量历史不落盘；显示位置、透明度及网卡选择可保存为偏好设置。

## 验证要求

后台与界面分别用真实本地 socket 测试半帧、多帧、初始快照、断线重连、无新鲜数据、坏帧与缓冲上限。
部署验收须确认普通用户 GUI 能读取允许访问的 socket，关闭 GUI 不终止后台，关闭后台不留下采集子进程。
