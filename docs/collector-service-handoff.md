# 采集器常驻服务化：交接文档

**状态**：已完成并验证（2026-10-17）
**涉及仓库**：`ball`（本仓库）、`socktrail`（`~/Code/0-Cloned/0-Internet/socktrail`）

## 1. 目标

让 socktrail 可以作为由 systemd 管理的常驻采集器运行，并让 ball 订阅它，而不是
自己 spawn 并看管一个子进程。

动机：

- 采集器与 ball 后台解耦。升级、重启、排查采集器时不需要动 ball。
- 采集是特权操作（AF_PACKET / eBPF），而 ball 后台的职责是聚合与发布。
  分开之后，采集器的权限、内存上限和重启策略可以在它自己的 unit 里单独管理。
- 同一台机器上可以有多个只读消费方（ball、监控 agent、诊断工具）订阅同一条流，
  而不必各自拉起一份采集器（socktrail 默认单实例，`--allow-multiple` 才会并发）。

非目标：

- 不做 ball 的连接级视图（连接列表、域名、SNI）。见 §7。
- 不改 ball 默认行为：不传 `--collector-socket` 时仍由后台 spawn 采集器。

## 2. 方案

### 2.1 总体结构

```
systemd: socktrail.service (root, 常驻)
    socktrail --output msgpack --socket /run/socktrail/collector.sock
                   --socket-mode 0660 --refresh 1s --socket-kinds stats
                              │
                              │ MessagePack frames (hello / stats)
                              ▼
systemd: ball@<user>.service (root)
    ball --daemon --socket /run/ball-<user>/gui.sock
         --collector-socket /run/socktrail/collector.sock
                              │
                              │ MessagePack frames (snapshot)
                              ▼
                        GUI (普通用户)
```

### 2.2 socktrail 侧改动

| 改动 | 位置 |
| --- | --- |
| `--socket-kinds` 参数：选择 socket 上发布的帧类型（`stats` / `flow`） | `internal/app/app_linux.go`、`internal/app/stream_socket_linux.go` |
| 过滤点放在 `streamSink.encodeFrame`，握手与增量共用一条规则；`hello` 永不过滤 | `internal/app/stream_socket_linux.go` |
| systemd unit | `packaging/socktrail.service` |
| 安装脚本 | `install-service.sh` |
| 文档 | `README.md`、`docs/user/usage.md`、`docs/internals/stream-output.md` |
| 测试 | `internal/app/stream_summary_linux_test.go` |

unit 要点：`Type=simple`、`RuntimeDirectory=socktrail`、`Restart=on-failure`、
`CapabilityBoundingSet`/`AmbientCapabilities` 覆盖抓包与 eBPF、`LimitMEMLOCK=infinity`。
刻意不加 `RestrictAddressFamilies`（会挡 AF_PACKET）和 `PrivateTmp`（单实例锁在
`/tmp/socktrail.lock`，私有 /tmp 会允许第二个实例并列启动）。

安装到 `/usr/local/bin`（与 socktrail 官方 `install.sh` 和 `task install` 一致）。

### 2.3 ball 侧改动

| 改动 | 位置 |
| --- | --- |
| `ServiceOptions::collectorSocketPath`：外部采集器模式 | `src/daemon/service.h` |
| `--collector-socket` 参数与校验（绝对路径、仅 daemon、与 `--socktrail-bin` 互斥） | `src/main.cpp` |
| `startCollector()` 分支：外部模式只订阅，不 spawn | `src/daemon/service.cpp` |
| `stopCollector()` / `collectorRunning()` 在外部模式下不碰外部进程 | `src/daemon/service.cpp` |
| spawn 模式追加 `--socket-kinds stats` | `src/daemon/service.cpp` |
| 消费 `io_unmatched_bytes`，报为质量说明 | `src/daemon/socktrail.cpp` |
| 文档 | `README.md`、`docs/protocol.md` |

外部模式下的生命周期语义（已在文档中写明）：

- ball **不会**启动或停止采集器；采集器的生死由它的 unit 决定。
- 「关闭后台不留下采集子进程」的保证只对 spawn 模式成立。
- 外部 socket 必须对运行 ball 后台的用户可读写。

## 3. 实施顺序

1. socktrail：加 `--socket-kinds`，测试覆盖「stats-only 不出现 flow 帧」「flow-only
   不出现 stats 帧」「非法取值被拒绝」「hello 永不过滤」。
2. socktrail：unit + 安装脚本 + 文档。
3. ball：`--collector-socket` 模式 + CLI 校验 + CLI 测试。
4. ball：spawn 模式追加 `--socket-kinds stats`。
5. ball：消费 `io_unmatched_bytes`（此前完全忽略的缺口字段）。
6. 文档与端到端验证。

## 4. 验证

已验证（本机，root，真实 eBPF）：

- 外部模式端到端：`socktrail.service` 风格的常驻采集器 + `ball --collector-socket`，
  GUI 侧观测到 `available=true stale=false incomplete=false err=''`，26+ 进程、4 网卡。
- 停 ball 不影响外部采集器；重启 ball 能重新订阅同一条流。
- `--socket-kinds stats` 实测：订阅流量 **8,812,211 → 42,383 字节 / 6 秒（约 208×）**，
  `flow` 帧归零，`stats` 仍约 1 Hz。
- 单元测试：ball `ctest --preset dev` 9/9；socktrail `go test ./...` 全绿。
- 参数校验：`--collector-socket` 的相对路径、非 daemon 用法、与 `--socktrail-bin`
  同时出现，三种情况都以退出码 2 拒绝。

2026-10-17 补充真实部署验证：

- `socktrail.service` 与 `ball@<当前用户>.service` 已安装、enabled / active，实际 unit 与
  drop-in 通过 `systemd-analyze verify`，GUI 已启动。
- 独立采集器使用本机 drop-in 选择 stats-only、关闭两类连接探针；通用模板保留默认能力。
- 普通用户成功读取真实快照（4 个网卡、30 多个进程、非零 IP 计数），进程速率可用、错误为空。
- 停止、启动 ball 后采集器 PID 不变，ball 能重新订阅；真实进程树没有额外采集器实例。
- 仍有 `unmatched socket I/O` 质量缺口与 kTLS 探针不可用的非致命警告，不能声称观测完全无缺口。

未验证：

- 机器真实重启后的自启，以及注销、登录后的 GUI 自启。
- socket 组为共享组、消费方在别的账号的组合。
- 采集器侧 `--socket-kinds` 在极高连接数（>10k）下的降幅。

## 5. 已知问题与踩坑（重要）

### 5.1 单实例锁会静默拖垮 spawn 模式

socktrail 默认对 `/tmp/socktrail.lock` 加 `flock`，第二个实例直接退出。当机器上已有
一个手工启动的 socktrail（或用 `--allow-multiple` 起过又没清理）时，ball spawn 的采集器
启动即失败，GUI 侧表现为：

```
process_status.error = "collector exited; ... socktrail: another socktrail is already
running; use --allow-multiple to run more than one"
```

这是**正确**的报错，不是 bug。排查采集器不工作时先看这条。

### 5.2 一次误判的记录

本次改动过程中，曾把「ball 收不到 stats」误判为新版 socktrail 的回归。**该结论是错的，
已撤回**。真实原因是测试环境里残留的 socktrail 进程持有单实例锁（§5.1），导致 ball
spawn 的采集器从未启动。清理锁之后，同一份新版 socktrail + ball 完全正常。

教训：`pkill -f socktrail` 会匹配到调用它的 shell 自身（命令行里含 "socktrail"），
造成大量假的「静默失败 / exit 143」。清理测试进程请按 PID，或用 `pkill -x <精确名>`。

### 5.3 flow 帧不是「暴涨」

顺带澄清：订阅流里 `flow` 帧数量大，是因为握手会为**当时存在的每条连接**发一帧
`changes:["snapshot"]`，之后每 `--refresh` 重发未变化的进行中连接。本机约 2000 条连接
≈ 2000 帧握手、约 4.8 MB/s。这是协议设计，不是 bug。只订阅 `stats` 即可完全避开。

## 6. 复现步骤

```sh
# 1. 构建采集器
cd ~/Code/0-Cloned/0-Internet/socktrail
GOTOOLCHAIN=local GOPROXY=off GOSUMDB=off CGO_ENABLED=0 go build -trimpath -o socktrail .

# 2. 装成常驻服务（或手工起进程验证）
./install-service.sh

# 3. 让 ball 订阅它（spawn 模式与外部模式二者只能选一）
cd ~/Code/ball
cmake --preset dev && cmake --build --preset dev
sudo ./build/dev/ball --daemon --socket /run/ball-$USER/gui.sock \
     --collector-socket /run/socktrail/collector.sock

# 4. 观察
#    spawn 模式：每次启动自检，应看到 --socket-kinds stats
#    外部模式：采集器由 systemd 管理，ball 不产生子进程
```

## 7. 后续规划

按优先级排列；IP 计数展示已完成，其余状态见各项说明。

### 7.1 ball 消费已有的缺口统计（低风险，建议先做）

`stats` 帧里的缺口统计及展示状态（后续进展见 `docs/next-steps-handoff.md` §3.1）：

| 字段 | 含义 | 建议用途 |
| --- | --- | --- |
| `ip_packets` / `ip_bytes` | 采集点的 IP 报文数 / 字节 | 已做：详情窗口独立展示采集器运行累计，明确不与进程或网卡口径相加 |
| `io_unmatched_bytes` | 没能关联到所选流的 socket I/O | 已做（报为质量说明）；可进一步在详情里显示数值 |

### 7.2 连接级视图（大改动，独立立项）

ball 目前**只有**网卡和进程两个视图。`flow` 帧、`--socket-sniff`、`--openssl-probe`
产出的都是连接级数据（连接列表、域名、SNI、JA4、内核 TCP 指标），ball 现在完全用不上，
因此：

- spawn 模式显式传 `--openssl-probe=false --socket-sniff=false` 关掉它们（省采集器 CPU）。
- 外部模式若想用，需要 ball 新建连接表视图，并取消 `--socket-kinds stats`。

这相当于把 socktrail TUI 的主要能力搬进 HUD，应作为独立功能设计，不要塞进本改动。

### 7.3 共享组部署

外部采集器给非 root 消费方使用时，需要 unit 的 `--socket-mode 0660` 加一个共享组，
并确保 socket 目录组可进入。当前文档已说明，未做实际部署验证。

### 7.4 让 ball 的 unit 也支持外部采集器（已完成）

`install.sh --collector-socket` 使用实例级 drop-in 覆盖 `ExecStart`，保留模板的
`--socktrail-bin` 默认值，重新安装模板不覆盖外部配置。隔离安装测试与 systemd
配置语法与真实服务部署验证通过；详见 `docs/next-steps-handoff.md` §3.2。
外部采集器不属于 ball 子进程，停止 ball 时不应终止它。

## 8. 相关文件

```
socktrail/
  install-service.sh                     安装脚本
  packaging/socktrail.service            systemd unit
  internal/app/stream_socket_linux.go    帧过滤（publishes / encodeFrame）
  internal/app/app_linux.go              --socket-kinds 参数
  internal/app/stream_summary_linux_test.go  过滤测试
  docs/user/usage.md, README.md          文档
ball/
  src/main.cpp                           --collector-socket
  src/daemon/service.{h,cpp}             外部采集器模式
  src/daemon/socktrail.cpp               --socket-kinds stats、io_unmatched_bytes
  tests/cli.cpp                          rejectsCollectorSocketMisuse
  README.md, docs/protocol.md            文档
```
