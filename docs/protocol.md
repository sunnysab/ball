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
- 此接口输出连接标识、域名证据说明与端点的流量计数，但不输出报文内容、载荷或原始 flow。

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
  "ip_packets": 2450000,
  "ip_bytes": 3892314112,
  "process_status": {
    "available": true,
    "stale": false,
    "incomplete": false,
    "error": "",
    "notes": [],
    "info": []
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
  ],
  "connections": [
    {
      "id": 42,
      "protocol": "TCP",
      "app": "tls",
      "state": "established",
      "direction": "outbound",
      "source": "10.0.0.2:40000",
      "target": "10.0.0.5:443",
      "name": "example.com",
      "detail": "sni: example.com",
      "end": "",
      "interfaces": ["eno1"],
      "first_seen": "2026-10-14T14:29:58+08:00",
      "last_seen": "2026-10-14T14:30:02+08:00",
      "rx_bytes": 2048,
      "tx_bytes": 512,
      "packets": 40,
      "retransmits": 0,
      "rate": {"rx_bps": 1024.0, "tx_bps": 256.0},
      "rtt_us": 800,
      "rtt_source": "kernel",
      "client": {"pid": 1234, "start_ns": 9007199254740993, "name": "firefox", "rx_bytes": 2048, "tx_bytes": 512},
      "server": {"pid": 0},
      "io": [{"pid": 1234, "start_ns": 9007199254740993, "name": "firefox", "rx_bytes": 2048, "tx_bytes": 512}]
    }
  ],
  "connection_status": {
    "available": true,
    "stale": false,
    "truncated": false,
    "notes": []
  }
}
```

- `started` 是本次后台监测起点，后台重启后改变，GUI 重启不改变。
- `at` 是生成快照的时刻。两者都是 MessagePack timestamp 扩展（ext type `-1`），
  按规范表示一个精确时刻；解码后转成本地时间显示。字符串时间不是合法取值。
- `ip_packets` / `ip_bytes` 是**采集器本次运行**观测到的 IP 报文个数与字节总数，
  与进程 socket I/O、网卡计数是三种互不相等的口径，任何两个都不得相加。两者是可选
  字段：缺失时解码端按 0 处理（后台尚未收到 `stats`、或采集器不提供该计数），但字段
  存在而取值非法时整帧被拒绝，不会退化成 0。合法取值与非负整数计数的规则相同
  （见下面关于整数与浮点的条目）。
- `rate` 的 `bps` 指 **bytes per second**，不是 bits per second；为有限、非负数字。
- 所有字节计数及 `start_ns` 使用整数。MessagePack 的整数本身就保证 64 位精度，
  所以不需要字符串编码；`start_ns` 为 0 时省略该字段，解码端按 0 处理。
  浮点形式只在能被精确表示时接受（非负、无小数部分、不超过 2^53），否则拒绝整帧，
  以免计数被静默取整。
- PID 与网卡索引为正整数且不超过 `INT_MAX`。
- `kind` 只取 `physical`、`virtual`、`loopback`。网卡和进程的数组顺序不是身份；
  同一快照内网卡索引、`(pid, start_ns)` 都不得重复。
- `interface_error` 与 `process_status` 的各字段、`notes` 数组必须存在，缺失即坏帧；
  `process_status.info` 与整个 `connections` / `connection_status` 段是后加的可选字段，
  缺失时分别视为空列表和“不支持连接”。字段一旦出现必须严格校验。
  所有字符串不得包含控制字符（协议解析器会拒绝，避免把终端转义带进界面）。
- `today` 是本地日期内已观测累计；`session` 是本次监测已观测累计。首次监测前视为零，
  不从系统已有计数补算历史。后端重启时两者都清零。
- `ip_packets` / `ip_bytes` 与 `today` / `session` 的分段规则不同：它们是采集器进程
  自己的 64 位累计，**采集器重启后从零重新开始**。因此它们与界面上的网卡、进程累计
  没有共同的起点，不能相互校验或相减。
- 缺席进程及已移除接口可以保留累计行，`present` / `up` 为 false、速率为零。
- 未知字段必须被忽略：新增可选字段不改变现有消费方。

## 界面解释

- 流量条只合计用户选定网卡。首次使用默认选择当前启用的物理网卡，不自动把 VPN、回环和虚拟接口加入。
- 进程统计是应用 socket I/O，网卡统计是内核接口计数，不要求两者相等，不互相相加。
  详情窗口另有一行单独的「IP 报文字节 / 报文数」：它是采集点的 IP 口径，工具提示里
  说明三者互不相等、不互相相加，且该行不随所选网卡变化。
- 只有 `process_status.available=true`、`!stale` 且快照新鲜时该行才显示数值；否则显示
  占位符 `—`（也包括还没有任何快照时）。已经收到的旧数值不得当作当前值展示。
- `interfaces_available=false` 或 GUI 没有新鲜快照时，不能把过期速率显示成真实的零；应显示不可用状态。
- 进程页是否实时可用须同时检查 `available` 和 `!stale`；不完整标记及原因可见。
  `process_status.notes` 是真实数据缺口（PID 事件丢失、unindexed 字节），会进入
  `incomplete`；`process_status.info` 只是解释（unmatched socket I/O、已过期 PID、
  非 `kernel TLS probes active` 的 kTLS 探针状态），不驱动 `incomplete`。
- 连接页由独立的 `connections` 与 `connection_status` 描述：`available` 表示采集器
  声明或实际发送了 flow，`stale` 与 stats 新鲜度一致，`truncated` 表示有界缓存发生
  了淘汰。字段缺失视为旧后台不支持连接，不伪装成可用的空表。连接速率只在成功应用
  `stats` 时采样，flow 帧本身不会让 stats 变新鲜。
- 快照只在 GUI 内保留最新一份，流量历史不落盘；显示位置、透明度及网卡选择可保存为偏好设置。

## 采集器订阅

后台以 `--output msgpack --socket <路径>` 启动 socktrail，订阅同一个编码，但帧结构不同：
每帧是一个带 `kind` 的信封，`kind` 取 `hello`、`flow` 或 `stats`，负载在同名 key 下。
后台使用 `hello`（`version=1`、`started` 时间戳、可选 `kinds`）、`stats`（`at` 时间戳、
`process_io` 全量进程表、`ip_packets` / `ip_bytes` 采集器运行累计、`probes.pid` 与
`io_unindexed` / `expired_pid_io` / `expired_pid_count` 质量计数）与 `flow`
（一条连接：`id` 非零，`protocol` / `source` / `target` / `first_seen` / `last_seen` /
`rx_bytes` / `tx_bytes` / `packets` 必需，其余字段按上游省略规则读取）。
`hello.kinds` 包含 `flow` 即声明连接可用；没有 `kinds` 时直到收到首个合法 flow 才确认。
每次 `hello`（包括同一采集器重连）都会重建连接缓存，断线立即清空并标记过期。
`ip_packets` / `ip_bytes` 的可选规则与拒绝策略同 `process_io`：缺失为 0，
存在但非法则拒绝整帧并断开订阅。
后台还会读取 `probes.ktls.status`（可选）作为 `info` 说明，不自动提权。
stats 的字符串诊断允许包含多行，读取时把控制字符归一为空格并在过长时明确省略，
不会因为一条合法诊断就以坏帧为由拒绝 stats。
采集器以 `--socket-kinds stats,flow` 启动，继续关闭 openssl 与 socket-sniff 探针
（spawn 模式由后台传参；外部采集器模式需在采集器侧自行配置为 `stats,flow`）。
采集器自身的字段说明见其仓库的 `docs/internals/stream-output.md`。

## 验证要求

后台与界面分别用真实本地 socket 测试半帧、多帧、初始快照、断线重连、无新鲜数据、
坏帧与缓冲上限。IP 两字段另需覆盖：读入与缺失为 0、超过 2^53 与 `UINT64_MAX` 不失真、
非法取值拒绝整帧、坏帧不部分更新、采集器重启后覆盖而非累加。部署验收须确认普通用户
GUI 能读取允许访问的 socket，关闭 GUI 不终止后台，关闭后台不留下采集子进程。

连接关联计数 `io_unmatched_bytes` 中的字节已经计入 `process_io`，不能作为进程漏采量；
上游目前还存在依赖首个采集接口的汇总口径限制。kTLS 探针状态不是解密能力：缺少探针
会限制内核 TLS socket 字节覆盖，与 unmatched 计数无直接因果。状态中的原始加载错误
用于诊断权限、模块 BTF 或内核兼容性，不自动推断为缺少某一项 capability。

`connection_status.generation` 是无符号订阅代次，每次合法 hello（包括连接同一采集器）
递增；缺失默认为 0。连接身份必须按后台 `started`、订阅代次、连接 ID 三者区分。
后台重启或代次改变时，GUI 清除连接选择，避免 ID 复用串到无关连接；暂停状态下仍可
查看冻结的旧代次，恢复时再清除旧选择。进程筛选继续使用独立的 `(pid, start_ns)`。
