# ball 交接文档：安装脚本、开机自启与后续计划

**状态**：安装脚本已完成并验证；后续计划未开始
**涉及仓库**：`ball`（本仓库）、`socktrail`（`~/Code/0-Cloned/0-Internet/socktrail`）
**相关文档**：`docs/collector-service-handoff.md`（采集器常驻服务化，已完成）

## 1. 本次完成的事

### 1.1 一键安装脚本 `install.sh`

覆盖「构建 → 安装 → 开机自启 → GUI 登录自启」全流程。用法：

```sh
./install.sh                     # 构建 + 安装 + 启用开机自启（当前用户）
./install.sh --user alice        # 为另一个用户装后台
./install.sh --prefix /usr       # 安装前缀（默认 /usr/local）
./install.sh --collector-socket PATH   # 改为订阅外部采集器
./install.sh --no-build          # 用已有构建目录
./install.sh --no-enable         # 只装文件，不启用服务
./install.sh --no-gui-autostart  # 不写 GUI 登录自启
./install.sh --uninstall         # 停用并移除本脚本添加的文件
```

关键设计决定：

- **没有写第二个 systemd unit**。后台本来就是 `ball@<user>.service`，`install.sh`
  只是 `systemctl enable --now` 它。多写一个 unit 会与模板实例冲突。
- **GUI 登录自启复用 ball 自己的 autostart 模块格式**。脚本写出的
  `~/.config/autostart/ball.desktop` 与 `src/ui/autostart.cpp` 的 `enabledEntry()`
  逐字段一致（含 `X-Ball-Managed=true`），已实测 `autostartEnabled()` 返回 true。
  这样脚本和界面开关不会互相打架。
- **前缀通过 `cmake --install --prefix` 显式传入**。构建配置里的 prefix 可能与安装目标
  不同，而 unit 与 desktop 文件里的绝对路径在 **configure** 阶段就写死了。脚本在构建时
  传 `-DCMAKE_INSTALL_PREFIX`，安装时再传一次 `--prefix`，两者保持一致。
- **`--uninstall` 只删本项目装的文件**，并且只删除带 `X-Ball-Managed=true` 的
  autostart 条目——用户手写的同路径文件不会被删。
- **外部采集器模式改写 unit 的 `ExecStart`**：把 `--socktrail-bin "<path>"`
  替换为 `--collector-socket "<path>"`。已用 `systemd-analyze verify` 验证生成的 unit。

### 1.2 已验证

| 项目 | 结果 |
| --- | --- |
| `sh -n` 语法 | 通过 |
| 暂存前缀安装（`--no-enable --no-gui-autostart`） | 文件树完整：`bin/ball`、`libexec/ball/socktrail`、unit、desktop、图标 |
| 采集器打包进 `libexec/ball/socktrail` | 是（`-DBALL_SOCKTRAIL_BINARY` 生效） |
| `--collector-socket` 改写 ExecStart | 正确，`systemd-analyze verify` 无输出 |
| GUI 自启条目被 ball 自己的开关认可 | `autostartEnabled()` = true |
| 缺采集器时 | 以退出码 1 报错并提示 `--collector-socket` |
| 缺同名私有组时 | 以退出码 1 报错并给出 `groupadd`/`usermod` 命令 |
| 无 systemd 时 | 以退出码 1 报错 |
| `--uninstall` | 移除已安装文件，残留为空 |
| 未知参数 | 退出码 2 |

### 1.3 未验证

- 真实的 `systemctl enable --now ball@<user>.service`（我全程避免动用户正在跑的服务，
  只验证到 `--no-enable` 与 unit 语法）。
- 为**另一个用户**（`--user alice`）安装的完整流程。
- 脚本在 `/usr` 前缀下的真实安装（只用临时前缀验证过；`/usr` 需要额外注意
  desktop 文件的 `Icon=ball` 依赖图标装到 `/usr/share/icons`）。

## 2. 本次澄清的概念：两层自启

之前的文档没有把这两件事讲清，`install.sh` 让它显式了：

| | 后台服务 | GUI 悬浮窗 |
| --- | --- | --- |
| 载体 | systemd `ball@<user>.service` | `~/.config/autostart/ball.desktop` |
| 触发 | 开机（`multi-user.target`） | 登录 KDE 会话 |
| 权限 | root | 普通用户 |
| 安装脚本默认 | 启用 | 写入 |
| 关闭方式 | `systemctl disable ball@$USER` | 界面里的「登录时启动界面」开关 |

「关闭 GUI 不终止后台」这条既有保证不变：两者生命周期本就独立。

## 3. 后续计划

按优先级排列。每项都写明**动机、方案、验证方式、风险**。

### 3.1 ball 消费 `ip_packets` / `ip_bytes`（低风险，建议先做）

**动机**：`stats` 帧里有这两个字段，ball 完全没读。它们是与进程 socket I/O **并列的另一种
口径**（采集点的 IP 报文字节），`docs/protocol.md` 已声明两者不相等、不互相相加，但界面
没有呈现，用户看不出这个差异。

**方案**：

- `src/daemon/socktrail.cpp` 的 `parseStats` 增加两个可选字段的读取（沿用现有
  `mp::readCount` 与拒绝策略；注意这两个字段在采集器侧是无 `omitempty` 的必填项）。
- 通过 `Snapshot` 传到 GUI，在详情窗口的网卡区域或状态行显示「IP 报文字节」，
  与「进程 socket I/O」分开呈现，并在 tooltip 里说明口径差异。
- 需要给 `Snapshot`/`InterfaceSnapshot`/`ProcessSnapshot` 之外的顶层加字段，注意
  同步更新 `tests/ui_preview.cpp` 的样例数据与 `tests/mp_frames.h` 的写帧器。

**验证**：单元测试覆盖「读到该字段」「字段缺失时按 0」「与进程口径并列显示不被相加」；
真实采集器下 `ctest --preset dev` 全绿。

**风险**：低。属于纯展示扩展，不影响累积逻辑。

### 3.2 ball 的 unit 支持外部采集器（中风险）

**动机**：`packaging/ball@.service.in` 目前写死 `--socktrail-bin`。`install.sh` 是靠
`sed` 改写已安装的 unit 来实现外部模式的，这属于安装期打补丁，重新 `cmake --install`
就会被覆盖。更好的做法是让 unit 本身可选。

**方案**（择一）：

- 方案 A：unit 用 `EnvironmentFile=-/etc/ball/%i.conf`，把采集器来源做成配置项，
  `ExecStart` 用 shell 展开。注意 unit 里 `ExecStart` 不用 shell，需要
  `Environment=` 变量拼接或 `/bin/sh -c`（后者会引入引号与注入面的问题，不推荐）。
- 方案 B（推荐）：保留单一 `ExecStart`，由 `install.sh` 在安装后写一个 drop-in
  `/etc/systemd/system/ball@%i.service.d/collector.conf` 覆盖 `ExecStart`。
  drop-in 不会被 `cmake --install` 覆盖，语义也符合 systemd 的惯例。

**验证**：`systemd-analyze verify`；`systemctl cat ball@<user>` 确认 drop-in 生效；
spawn 与外部两种模式各起一次，确认进程树符合预期。

**风险**：中。`ExecStart` 的覆盖是 systemd 里比较容易出错的部分（多个 `ExecStart` 会
串联而非覆盖，必须用 `ExecStart=` 空赋值清空）。需要实测。

### 3.3 共享组部署验证（低风险）

**动机**：外部采集器给**非 root** 消费方使用时，需要把 socket 目录和 socket 的组改成
共享组。目前只有文档，没实测过。

**方案**：起一个 `--socket-mode 0660` 的采集器，把 group 改成目标共享组，用另一个账号
的消费方订阅，确认能读、且其他账号不能读。

**验证**：正反两个方向各测一次（可读 / 不可读）。

**风险**：低，但涉及多账号，需要在测试机上新建用户。

### 3.4 连接级视图（大改动，独立立项）

**动机**：ball 目前只有网卡和进程两个视图。`flow` 帧、`--socket-sniff`、
`--openssl-probe` 产出的都是连接级数据（连接列表、域名、SNI、JA4、内核 TCP 指标），
ball 完全用不上——所以 spawn 模式显式关掉了这两个探针，订阅也只收 `stats`。

**方案要点**（需要先做设计，不宜直接开写）：

1. 采集器改为 `--socket-kinds stats,flow`，并评估重新打开 `--socket-sniff` /
   `--openssl-probe` 的成本（前者读每个本地 TCP socket 的 16 KiB，后者挂用户态探针）。
2. ball 侧新建连接表：需要在 `Socktrail` 里维护 `id → flow` 的映射（`flow` 帧是
   **累计值覆盖**，不是增量，`id` 只在本次运行内稳定），并新增详情窗口的第三个视图。
3. 界面要处理 `flow` 帧的字段：`source`/`target`、`client`/`server`/`io[]` 三种进程
   来源、`end` 后的保留策略、以及 `io[]` 与 `process_io` 的口径差异。
4. 性能：本机约 2000 条连接时订阅流量约 4.8 MB/s，handshake 约 2000 帧。
   这是 GUI 进程（普通用户）要消化的量，需要实测确认不卡界面。

**验证**：解析层单测、真实采集器下的端到端、以及界面在高连接数下的响应。

**风险**：高。这是把 socktrail TUI 的主要能力搬进 HUD，工作量和设计复杂度都显著。
建议单独出设计文档后再实施。

### 3.5 让 `install.sh` 覆盖更多部署形态（低风险）

**可选扩展**：

- 目前只处理 `systemd`。若要支持无 systemd 的环境，需要另写一份前台运行脚本。
- `--user` 路径未实测（为另一个用户安装后台）。
- `/usr` 前缀下 `Icon=ball` 的图标查找路径未验证。

## 4. 复现与验证步骤

```sh
cd ~/Code/ball

# 构建 + 装到临时前缀，验证文件树与 unit 语法（不改动系统）
./install.sh --prefix /tmp/ball-check --no-enable --no-gui-autostart
find /tmp/ball-check -type f | sort
systemd-analyze verify /tmp/ball-check/lib/systemd/system/ball@.service

# 外部采集器模式的 ExecStart
./install.sh --prefix /tmp/ball-check2 --collector-socket /run/socktrail/collector.sock \
             --no-enable --no-gui-autostart
grep ExecStart /tmp/ball-check2/lib/systemd/system/ball@.service

# 清理
sudo ./install.sh --prefix /tmp/ball-check --uninstall
sudo ./install.sh --prefix /tmp/ball-check2 --uninstall
```

真实安装（会启用开机自启）：

```sh
./install.sh
systemctl status ball@$USER.service
```

## 5. 已知问题与踩坑

- **单实例锁**：spawn 模式下，机器上若已有手工启动的 socktrail（或残留进程），
  ball 的采集器会启动即失败并报 `another socktrail is already running`。这是正确行为，
  详见 `docs/collector-service-handoff.md` §5。
- **`pkill -f socktrail` 会误杀自己**：调用它的 shell 命令行里含 "socktrail"，
  会被一并匹配，表现为大量假的 exit 143。清理测试进程请按 PID 或用 `pkill -x <精确名>`。
- **`install.sh --prefix` 必须与构建时的 `-DCMAKE_INSTALL_PREFIX` 一致**：unit 与
  desktop 里的绝对路径在 configure 时写死，脚本已在两处都传，但手工安装时要注意
  （README 的「安装前缀必须在 configure 阶段确定」一节）。
- **GUI 自启条目与界面开关共享同一文件**：脚本用 `X-Ball-Managed=true` 标记自己的条目，
  `--uninstall` 只删这个标记的文件，不会动用户手写的同名文件。

## 6. 相关文件

```
ball/
  install.sh                         本次新增：构建+安装+自启+卸载
  README.md                          「3. 安装」「运行」两节更新
  packaging/ball@.service.in         后台 unit 模板（本次未改，见 3.2）
  packaging/ball.desktop.in          应用菜单项
  src/ui/autostart.{h,cpp}           GUI 登录自启模块（本次未改，被脚本复用格式）
  CMakeLists.txt                     安装规则（本次未改）
  docs/collector-service-handoff.md  采集器常驻服务化（已完成）
  docs/next-steps-handoff.md         本文档
```
