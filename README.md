# ball

一个常驻 KDE Plasma Wayland 桌面的网络流量悬浮窗（HUD）。

## 当前状态

本仓库目前是 **可构建的骨架加统计核心**：一个无边框半透明、位于 layer-shell `Top` 层的窗口，
最小 CLI（`--help` / `--version`），以及不依赖 QWidget 的流量统计核心。

已实现：

- `src/daemon/interfaces`：通过 Linux rtnetlink `RTM_GETLINK` 读取每块网卡的累计 RX/TX、
  名称、index 与 flags，标记物理 / 虚拟 / loopback。非阻塞 socket 加有限 `poll` 超时：
  `Interfaces::poll(timeoutMs)` 阻塞调用线程最多 `timeoutMs`，后台线程请使用较小超时或异步调度。
- `src/daemon/traffic`：在内存中按网卡分别累计「今日」与「本次监测」的收发字节；
  速率用调用方提供的单调时间戳（如 `QElapsedTimer::elapsed`）计算，墙钟只用于监测起点与日期。
- `src/daemon/processes`：在内存中按进程（`pid` + socktrail `start_ns`）累计「今日」与「本次监测」
  的收发字节，语义与网卡累计一致，另外处理后端采集会话重启与进程缺席后重现。
- `src/daemon/socktrail`：异步订阅外部 `socktrail --socket` 的 Unix socket NDJSON 流，把
  周期性 `stats` 帧喂给 `Processes`。只使用 `stats` 的全量 `process_io` 表，不从 `flow` 帧求和。
- `src/daemon/service`：统一采样网卡、管理采集子进程、订阅进程数据，并通过受限权限的
  本地 socket 发布完整快照。GUI 断开不影响采集；慢客户端会被断开；停止服务清理自己的子进程。
  运行目录由部署层创建，快照字段见 [本地协议](docs/protocol.md)。

尚未接入程序入口：daemon 模式、GUI 数据展示与进程 / 网卡选择、安装配置。
统计、订阅和服务组件已有独立测试，`main` 仍是最小窗口。

统计契约：

- 首次样本只建立基线，不产生流量；`elapsed` 为 0 时不消耗基线，避免漏记或尖峰；
- 速率只看单调时间，墙钟回拨或跳进不会影响速率；无效的墙钟 / 单调样本被拒绝，不污染累计；
- 计数器方向重置时重新建立基线，不下溢、不产生反直觉尖峰；
- 本地日期变化时清空「今日」，「本次监测」保持不变；
- 监测起点在首次有效采样时记录，供后续界面展示；
- 网卡消失时保留已观察流量，同一 index 重现时重建基线，重命名保持累计值；
- 所有数据仅在内存中，从监测开始计数，不落盘保存历史。

进程累计契约（`Processes`）：

- 输入 `ProcessCounter{pid, startNs, name, rxBytes, txBytes}`，`startNs` 是采集器
  `start_ns`（纳秒身份），不是 `/proc/<pid>/stat` 的 starttime tick；`pid` + `startNs` 共同
  构成身份，PID 复用不会混入旧进程的累计；
- `rxBytes` / `txBytes` 是采集器当前会话的累计值，不保证覆盖进程启动以来的全部流量；
- 首个有效快照只建立基线（监测开始前的流量计为 0）；此后首次出现的进程按新段处理，
  其当前计数值全部计入（进程在后端启动后才产生流量的常见情形）；
- 差分使用调用方提供的单调毫秒；某方向计数回退表示该方向的新段，按新段字节计入并保留
  自身累计，不下溢、不产生尖峰；
- 进程缺席时保留行与累计、速率归零，`present` 为 false；同一身份重现时按新段计入；
- `backendSessionChanged` 为 true 时（后端 socktrail 重启）重建全部基线，保留已累计的
  today / session，该样本速率为 0；
- 本地日期变化只清 today，session 不变；无效墙钟 / 单调样本被拒且不污染状态；
- 输出 `ProcessTraffic{pid, startNs, name, present, today, session, rate}`，一般 QMap 行；
- 同样只在内存中，不落盘。

socktrail 订阅契约（`Socktrail`）：

- 协议为每行一个 JSON 对象的 NDJSON；`kind` 为 `hello` / `flow` / `stats`。客户端只读
  `stats` 的**全量** `process_io` 累计值，**不**从 `flow` 增量求和，否则会因丢帧而少计；
- 每帧先完整解析并校验再改动状态：`hello` 校验 `version == 1` 与 `started`（带时区的
  RFC 3339），`stats` 校验 `at`、每个进程的 `pid` / `start_ns` / `rx_bytes` / `tx_bytes`
  以及 `probes.pid` 与未索引 / 过期字节字段。数字用 `QJsonValue::toInteger` 读取，不经
  `double` 损失 > 2^53 的整数；负数、小数、缺字段、重复身份、非法 pid 一律拒绝整帧；
- 一个字节计数若超出可精确表示的 `qint64` 范围也拒绝整帧，不跳过单行：部分接受会删掉
  进程的旧基线，反而制造一次假重启；
- 断线或拒绝帧会关闭连接并重连，错误是短 ASCII 文本；累计值在断线时保留，不清零；
- 每次连接必须先收到合法 `hello` 才接受 `stats`。`available()` 在一次完整 `stats` 之后
  为 true，连接建立本身不算；实时展示必须同时检查 `available()` 与 `!stale()`，
  质量丢失由独立的 `incomplete()` 标记说明；
- `stale()` 由单调定时器判定：约 5 秒没有 `stats` 即 stale，`flow` 帧不刷新新鲜度，
  stale 时给出明确原因；
- `incomplete()` 表示观测历史存在无法弥补的缺口：尚未收到 `stats`、断线、后端重启、
  `stats` 帧携带 PID 丢失 / 未索引等质量问题时保持为 true。瞬时速率不伪造；
- 同一毫秒到达的多个 `stats` 帧不能靠单调时钟区分，保留最新一帧，等出现正间隔后再计入，
  不假造时间戳，也不会让最后一帧的累计永远丢失；
- 墙钟只用于本地日期与监测起点（`frame.at.toLocalTime()`，不把 UTC 当本地）；单调时间
  使用只初始化一次的 `QElapsedTimer`，因此 `Processes` 的历史在重连后仍连续；
- `Socktrail::kMaxLineBytes`（16 MiB）是单帧长度上限，decode 前检查，包括已经带换行的
  超长行；分块读取处理半行与一读多帧，超长即断开，避免缓冲无界增长；
- PID 丢失 / 未索引字节只作为质量提示（`qualityNotes()`），不加入任何已知进程或网卡总和；
- 重连约每秒一次，只有一个 timer；`stop()` 与析构都不再重连，对象归属为父子关系，
  信号 lambda 都带 context，不使用任何阻塞事件循环的 `waitFor...` 调用。

采样失败语义：

- `Interfaces::poll()` 返回 `std::optional<QList<InterfaceInfo>>`；`nullopt` 表示整次 dump 未成功
  （超时、`NLM_F_DUMP_INTR`、`NLMSG_ERROR`、`MSG_TRUNC`、坏帧 / 坏属性、`POLLERR` 等），
  调用方 **不能** 当作空网卡列表，否则会错误删除接口基线；只有完整成功的 dump 才返回列表。
- 解码器 `decodeLinkMessage` 用 `memcpy` 读 header / `ifinfomsg` / `rtattr`，拒绝负 index、
  非 NUL 接口名、长度不合法或尾部残留的属性；`IFLA_STATS64` 只要求字段到 `tx_bytes` 末端。
- 物理 / 虚拟由真实轮询时按 `/sys/class/net/<name>/device` 是否存在判定，
  `IFLA_LINKINFO` 仅作为 decoder 层提示。

> 跨午夜的采样区间会被计入新的一天，因此「今日」最多存在一个采样区间的误差。

规划的最终形态：

- 仅支持 KDE Plasma Wayland（依赖 layer-shell 协议），不支持 X11；
- 流量由外部 Go 程序 `socktrail` 采集，ball 只负责展示；
- 在内存中统计「今日」与「本次监测」累计值；
- 从监测起点开始计数，不落盘保存流量历史；
- 提供进程视图与网卡视图两个视图。

## 依赖

- CMake ≥ 3.25、Ninja
- 支持 C++23 的编译器（开发环境为 GCC 16 / Clang 23）
- Qt 6（Core / Gui / Widgets / Network）
- LayerShellQt（`LayerShellQt::Interface`）

## 构建与运行

```sh
cmake --preset dev          # Debug 配置，生成 build/dev 与 compile_commands.json
cmake --build --preset dev
ctest --preset dev          # ball_version、ball_traffic、ball_processes 与 ball_socktrail 单元测试

# 核心测试不依赖桌面会话，可在无显示环境直接跑：
env -u DISPLAY -u WAYLAND_DISPLAY QT_QPA_PLATFORM=offscreen ./build/dev/ball_traffic_test

cmake --preset release && cmake --build --preset release
```

运行（必须在 KDE Wayland 会话中，否则会以错误退出）：

```sh
./build/dev/ball
```

安装到系统：

```sh
cmake --install build/release --prefix /usr/local
```

代码格式与静态检查：

```sh
clang-format -i src/daemon/*.cpp src/daemon/*.h tests/*.cpp
# .clang-tidy 的 HeaderFilterRegex 只检查 src/ 与 tests/，AUTOMOC 产物不参与
clang-tidy -p build/dev --removed-arg=-mno-direct-extern-access \
    --checks='-*,bugprone-*,performance-*,clang-analyzer-*' --warnings-as-errors='*' \
    src/daemon/interfaces.cpp src/daemon/traffic.cpp src/daemon/processes.cpp \
    src/daemon/socktrail.cpp tests/traffic.cpp tests/processes.cpp tests/socktrail.cpp
echo $?   # 非 0 表示有未处理的告警
```

`--removed-arg` 用于跳过 Qt 发行版注入、Clang 无法识别的
`-mno-direct-extern-access` 编译参数。

## 代码风格

- 源码、标识符与注释使用英语；面向用户的界面文字（后续实现）统一使用简体中文；文档使用中文。
- 缩进 4 空格，列宽 100，见 `.clang-format`。
- 编译启用 `-Wall -Wextra -Wpedantic`，警告视为需要修复的问题。

## 目录结构

```
CMakeLists.txt        构建入口：ball_daemon 静态库、ball 可执行文件与 CTest 单元测试
CMakePresets.json     dev / release 的 configure、build、test presets
.clang-tidy           只对 src/ 与 tests/ 源码报错，忽略 AUTOMOC 生成代码
src/main.cpp          程序入口：CLI 解析与最小 layer-shell 窗口
src/daemon/           流量统计核心：rtnetlink 网卡读取、按网卡与按进程的内存累计、socktrail 订阅
  socktrail.h/.cpp   Unix socket NDJSON 客户端：帧解析校验、重连、可用性与新鲜度状态
  interfaces.h/.cpp  rtnetlink 网卡读取
  traffic.h/.cpp     按网卡累计
  processes.h/.cpp   按进程累计
tests/traffic.cpp     QtTest（QTEST_GUILESS_MAIN）：解析器 / 累计逻辑测试与只读网卡烟测
tests/processes.cpp   QtTest（QTEST_GUILESS_MAIN）：进程累计的基线 / 差分 / 重启 / 跨日测试
tests/socktrail.cpp   QtTest（QTEST_GUILESS_MAIN）：帧校验与本地 QLocalServer mock 端到端测试
```
