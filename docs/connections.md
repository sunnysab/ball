# 连接查询设计

本文描述当前已实现的连接页：查询在 GUI 本地对有界连接缓存完成，不向后台新增查询命令。
尚未部署到任何正在运行的服务；外部采集器必须显式启用 `flow` 并重配、更新二进制后才会提供数据。

## 范围与交互

悬浮条保持轻量。详情窗口保留「进程」「网卡」，已实现第三页「连接」。

- 进程页双击某行，或选中一行后点「查看连接」，切到连接页，并按精确的 `(PID, start_ns)` 筛选；
  连接页顶部显示可清除的进程筛选标签，提供「清除进程筛选」入口。下钻按当前显示模型中该行的真实身份
  取值，排序 / 搜索后不会错读成原列表行号。
- 连接页支持文本搜索（端点、域名、应用、进程名和 PID）、协议、活动或已结束状态、排名指标和 TopN。
- 查询顺序为：全部缓存记录 → 组合筛选 → 按指标排序 → TopN；TopN 实际可选 0..8192（0 表示全部），
  默认 20。相同值按 ID 排序，避免刷新抖动。
- 排名指标包括总速率、累计 IP 字节、RX 速率、TX 速率、重传。
- 表格为 `QTableWidget`，列为名称或应用、协议、来源、目标、参与进程、状态、采集 RX/TX 速率、
  累计 IP 字节、RTT、重传。
- 选中一条连接显示可复制的详情：ID、端点、域名证据说明、接口、客户端与服务端身份、实际 I/O 进程
  及其 socket 字节、开始/最近时间、结束原因。
- 支持暂停连接表刷新（明确标记为暂停）、恢复及复制所选连接；刷新保持选中 ID 和手动列宽。
- 进程页 / 网卡页的搜索框与「显示空闲」在连接页隐藏，避免与连接页自己的控件重复；切 tab 不清空
  连接页自身的查询状态。
- 无连接、筛选无结果、未收到 flow、过期和截断必须可区分；筛选只作用于连接，不改变原有网卡/进程合计。

## 数据口径

连接 RX/TX 是采集点 IP 报文字节方向，不承诺等于应用下载/上传；总速率为 RX+TX。
连接字节累计从采集器对该连接的首次观测开始，不命名为「今日」或「本次监测」。
进程总量继续只取 stats.process_io。`client`/`server` 表示建立/接受连接的进程，`io[]` 表示实际执行
I/O 的进程；进程查询三种来源都匹配 `(pid, start_ns)`，绝不退化为只看 PID。未关联进程的连接仍在全局
列表中显示为未知。
不做自动反向 DNS、网络请求或 TLS 解密；明文 Host/SNI 等证据仅使用采集器已经提供的字段。

## 已实现的容量与状态

- 连接缓存是有界的 `Connections` 类，容量固定 8192 行（`kMaxConnections`），另有 8 MiB 序列化预算。
- 达到上限时优先回收最早结束的连接，否则最久未更新者；不增长到无界，不改变进程总量。
- 缓存发生 reclaim / 拒绝时摘要显示「缓存已截断，非全量历史」；不承诺无限历史。
- 各阶段状态：收到 hello 且声明 `flow`，或实际收到合法 flow 帧之前，连接状态为不可用/未确认；
  连接与统计独立新鲜度，断线立即让连接状态过期，连接缓存速率只在成功应用 stats 时统一 sample。
- `process_status.info` 只作中性纯文本说明单独显示，不驱动 `incomplete` 或 HUD 感叹号；
  外部文本始终按纯文本呈现。

## 实现边界与共享契约

第一版沿用现有只读全量快照协议，GUI 在有界连接缓存上本地查询，不新增服务端查询命令或依赖。
后台只保留展示所需的精简字段，不透传原始 flow 或负载。传输仍由 16 MiB 帧限制和慢客户端断开保护。

### 数据类型（src/snapshot.h）

新增以下结构，成员名为后续组件的固定契约：

- `ConnectionProcess`：`int pid=0; quint64 startNs=0; QString name; quint64 rxBytes=0, txBytes=0;`
- `ConnectionSnapshot`：`quint64 id=0; QString protocol, app, state, direction, source, target, name, detail, end; QStringList interfaces; QDateTime firstSeen, lastSeen; quint64 rxBytes=0, txBytes=0, packets=0, retransmits=0; double rxBps=0, txBps=0; qint64 rttUs=0; QString rttSource; ConnectionProcess client, server; QList<ConnectionProcess> io;`
- `ConnectionStatus`：`bool available=false, stale=true, truncated=false; QStringList notes;`
- `Snapshot` 增加 `QList<ConnectionSnapshot> connections; ConnectionStatus connectionStatus;`
- `ProcessStatus` 增加 `QStringList info;`，仅作说明，不驱动 incomplete 或 HUD 感叹号。

进程身份 0 表示缺失，-1 只用于上游明确的歧义进程；io[] 只接受正 PID。startNs 和所有计数保持 uint64 精度。
新增快照字段使用 snake_case，完整验证后再更新。缺少整个 connections/connection_status 视为不支持连接，不伪装成可用空表。
为现有测试 fixture 允许缺少新增字段，但字段一旦出现必须严格校验；不是新增第二套旧协议实现。

### 连接缓存（src/daemon/connections.h/.cpp）

类 `ball::Connections`，公开接口固定为：

- `void reset()`：清空所有连接、基线和截断状态。
- `void update(const ConnectionSnapshot& row)`：按 ID 替换累计值；更新不会累加原始计数。
- `void sample(qint64 monotonicMs)`：按真实单调时钟采样速率；首次出现为基线，重复采样时间不重置基线；计数回退重建基线，不产生巨幅速率；已结束连接速率为 0。
- `QList<ConnectionSnapshot> rows() const`：返回最新记录。
- `bool truncated() const`：本次缓存是否发生过容量淘汰。
- `static constexpr int kMaxConnections = 8192;`

容量满时优先回收最早结束的连接，否则最久未更新者；不增长到无界，不改变进程总量。结束记录保留在同一有界缓存，不承诺无限历史。
每次 hello（含同一采集器重连）重建连接缓存，避免遗漏 end 和 ID 复用。断线立即让连接状态过期。

### 采集器接线（src/daemon/socktrail.*、service.*）

- `SocktrailFrameStatus` 增加 `Flow`，`SocktrailFrame` 增加 `ConnectionSnapshot connection; QStringList infoNotes; bool connectionsAdvertised=false;`。
- hello 可选 `kinds` 字符串数组，包含 flow 则声明连接可用；缺失时直到实际收到合法 flow 才确认支持。
- flow 帧 envelope 下 `flow` 完整验证。ID 非零；protocol/source/target/first_seen/last_seen/rx_bytes/tx_bytes/packets 必需；其余字段按上游省略规则读取。
- `Socktrail` 增加 `const Connections& connections() const; bool connectionsAvailable() const; QStringList infoNotes() const;`。
- flow 更新不让 stats 变新鲜；连接缓存速率在成功应用 stats 时统一 sample，避免握手刷新产生虚假峰值。
- malformed flow 严格拒绝整帧并进入已有重连流程；测试的原始空 flow fixture 改为合法连接，不继续忽略坏帧。
- service 将缓存与独立 connectionStatus 放进快照；缓存不可用/过期与接口可用性分开。
- spawn 改用 `--socket-kinds stats,flow`，继续关闭 openssl/socket-sniff；现成抓包证据足够支持第一版连接页。
- 外部采集器文档明确需 stats,flow；不自动修改正在运行的服务或重启它。

### 质量说明

PID ring 丢失、无效事件、unindexed 和断线保留真实 incomplete 语义。
unmatched 与 expired PID 进入 info，不触发历史丢失标记；说明 unmatched 不是 process_io 漏采量，当前上游多接口口径还有顺序依赖。
本轮**没有**修复上游 unmatched 的多接口重新记账算法，也没有把它拆成 netns 级独立修复；各接口的 unmatched 仍然不能简单求和。
kTLS 能力从可选 probes.ktls.status 读取，加入说明；不承诺无 kTLS 采集缺口，也不自动增加 CAP_SYS_ADMIN。

## 验证与限制

- 连接页在 GUI 内对快照携带的有界缓存做本地查询；无持久历史、无录包、无主动探测、无反查。
- 性能验证至少覆盖 2000 条连接；筛选和排名只针对当前缓存，达到容量上限时明确提示不是全量历史。
- 后台不改写或重启正在运行的采集器服务：切换到 `stats,flow`、更新采集器二进制都必须由管理员显式完成。
- 测试中的「匹配计数」与「可见计数」是本地当前帧的计数，不代表全网历史。
- 离线验证使用 `cmake --preset dev`、`cmake --build --preset dev` 和 `ctest --preset dev`；
  连接页用合成数据做离屏测试 / 预览，不把真实域名、命令行写入截图或日志。

## 所有权与文档维护

源码注释使用英文，README 与本文件使用中文。
不提交临时 agent 文件，不 push，不安装或重启系统服务。
上游 unmatched 的 netns 级重新记账属另行修复，不能把各接口的 unmatched 简单求和。
本轮不加入日历史存储、主动探测、录包、反向解析或自动服务提权。

## 集成验收补充

- 连接选择按后台 started、订阅 generation、连接 ID 区分，重连与重启不会沿用旧选择。
- 进程表刷新按 PID 与启动时间恢复选择，不随速率排序串行；连接页收起无关网卡卡片。
- 查询结果是当前有界缓存中的结果；结束记录可能被淘汰，不是持久化连接历史。
