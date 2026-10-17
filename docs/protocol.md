# 本地流量快照协议

此文档固定后台和 Qt 桌面界面的实现契约。接口仅用于本机 Unix socket，不使用 HTTP。

## 传输与权限

- 后台监听 Unix socket；客户端只读取，不发送控制命令。
- 每次连接立即获得当前完整快照，之后约每秒获得一个完整快照。
- 编码为 **MessagePack**：连接是连续的 MessagePack 对象，不带长度前缀或分隔符，
  每个对象自定界。连接不是帧边界，读写都可能落在对象中间。
- GUI 默认连接 `/run/ball-<当前用户名>/gui.sock`，允许通过启动参数指定其他路径。
- 正式部署由 systemd 创建 root 所有、目标用户组可进入的运行目录；socket 为 `0660`。
- 慢客户端必须被断开，不能阻塞采集或无限积压数据。GUI 重连后以最新快照覆盖状态，不累加快照。
- 单帧上限 16 MiB（`mp::kMaxFrameBytes`）；超过上限或无法解析的字节流必须断开连接，
  不能继续解析，因为 MessagePack 没有可用来重新同步的分隔符。
- 此接口不输出连接、域名、报文内容；只输出流量、进程标识、接口和数据质量。

## 快照

顶层是一个 map。下面的 JSON 只是同一份结构的可读写法，用于说明字段与取值；
线格式是 MessagePack，字段名以 `src/snapshot.h` 的 `field` 常量组为准。

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
      "today": {"rx_bytes": 2048, "tx_bytes": 512},
      "session": {"rx_bytes": 2048, "tx_bytes": 512},
      "rate": {"rx_bps": 1024.0, "tx_bps": 256.0}
    }
  ],
  "processes": [
    {
      "pid": 1234,
      "start_ns": 9007199254740993,
      "name": "firefox",
      "present": true,
      "today": {"rx_bytes": 1024, "tx_bytes": 128},
      "session": {"rx_bytes": 1024, "tx_bytes": 128},
      "rate": {"rx_bps": 512.0, "tx_bps": 64.0}
    }
  ]
}
```

- `started` 是本次后台监测起点，后台重启后改变，GUI 重启不改变。
- `at` 是生成快照的时刻。两者都是 MessagePack timestamp 扩展（ext type `-1`），
  按规范表示一个精确时刻；解码后转成本地时间显示。字符串时间不是合法取值。
- `rate` 的 `bps` 指 **bytes per second**，不是 bits per second；为有限、非负数字。
- 所有字节计数及 `start_ns` 使用整数。MessagePack 的整数本身就保证 64 位精度，
  所以不需要字符串编码；`start_ns` 为 0 时省略该字段，解码端按 0 处理。
- PID 与网卡索引为正整数且不超过 `INT_MAX`。
- `kind` 只取 `physical`、`virtual`、`loopback`。网卡和进程的数组顺序不是身份；
  同一快照内网卡索引、`(pid, start_ns)` 都不得重复。
- `interface_error` 与 `process_status` 的各字段、`notes` 数组必须存在，缺失即坏帧；
  所有字符串不得包含控制字符（口碑的解析器会拒绝，避免把终端转义带进界面）。
- `today` 是本地日期内已观测累计；`session` 是本次监测已观测累计。首次监测前视为零，
  不从系统已有计数补算历史。
- 缺席进程及已移除接口可以保留累计行，`present` / `up` 为 false、速率为零。
- 未知字段必须被忽略：新增可选字段不改变现有消费方。

## 界面解释

- 流量条只合计用户选定网卡。首次使用默认选择当前启用的物理网卡，不自动把 VPN、回环和虚拟接口加入。
- 进程统计是应用 socket I/O，网卡统计是内核接口计数，不要求两者相等，不互相相加。
- `interfaces_available=false` 或 GUI 没有新鲜快照时，不能把过期速率显示成真实的零；应显示不可用状态。
- 进程页是否实时可用须同时检查 `available` 和 `!stale`；不完整标记及原因可见。
- 快照只在 GUI 内保留最新一份，流量历史不落盘；显示位置、透明度及网卡选择可保存为偏好设置。

## 采集器订阅

后台以 `--output msgpack --socket <路径>` 启动 socktrail，订阅同一个编码，但帧结构不同：
每帧是一个带 `kind` 的信封，`kind` 取 `hello`、`flow` 或 `stats`，负载在同名 key 下。
后台只使用 `hello`（`version=1`、`started` 时间戳）与 `stats`（`at` 时间戳、
`process_io` 全量进程表、`probes.pid` 与 `io_unindexed` / `expired_pid_io` /
`expired_pid_count` 质量计数）；`flow` 帧按协议接受但完全忽略，因为进程累计只能来自
`stats` 表。后台只关心进程流量，所以采集器以 `--socket-kinds stats` 启动（spawn 模式
由后台传参；外部采集器模式需在采集器侧自行配置）：`hello` 与 `stats` 之外的 `flow` 帧
一律不发布。本机约 2000 条连接时，这会把订阅流量从约 4.8 MB/s 降到每秒约一帧。
采集器自身的字段说明见其仓库的 `docs/internals/stream-output.md`。

## 验证要求

后台与界面分别用真实本地 socket 测试半帧、多帧、初始快照、断线重连、无新鲜数据、
坏帧与缓冲上限。部署验收须确认普通用户 GUI 能读取允许访问的 socket，关闭 GUI 不终止后台，
关闭后台不留下采集子进程。
