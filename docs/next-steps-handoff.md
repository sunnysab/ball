# ball 交接文档：安装脚本、开机自启与后续计划

**状态**：安装脚本已完成并验证；3.1（IP 计数展示）已完成，构建与自动化测试通过，真实环境部署已验证；3.2（外部采集器 drop-in）已完成并通过隔离测试；其余后续计划未开始
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
- **外部采集器模式使用实例级 drop-in**：在
  `/etc/systemd/system/ball@<user>.service.d/collector.conf` 中清空并覆盖 `ExecStart`，
  模板 unit 不变；重新 `cmake --install` 不会覆盖该配置。详见 §3.2。

### 1.2 已验证

| 项目 | 结果 |
| --- | --- |
| `sh -n` 语法 | 通过 |
| 暂存前缀安装（`--no-enable --no-gui-autostart`） | 文件树完整：`bin/ball`、`libexec/ball/socktrail`、unit、desktop、图标 |
| 采集器打包进 `libexec/ball/socktrail` | 是（`-DBALL_SOCKTRAIL_BINARY` 生效） |
| `--collector-socket` 覆盖 ExecStart | 实例级 drop-in；外部与内置模式均通过 `systemd-analyze verify` |
| GUI 自启条目被 ball 自己的开关认可 | `autostartEnabled()` = true |
| 缺采集器时 | 以退出码 1 报错并提示 `--collector-socket` |
| 缺同名私有组时 | 以退出码 1 报错并给出 `groupadd`/`usermod` 命令 |
| 无 systemd 时 | 以退出码 1 报错 |
| `--uninstall` | 移除已安装文件，残留为空 |
| 未知参数 | 退出码 2 |

### 1.3 部署验证与剩余限制

- 已在真实环境执行安装、启用与启动：`socktrail.service` 和当前用户的 `ball@<user>.service`
  均为 enabled / active，GUI 已启动，登录自启指向 `/usr/local/bin/ball`。
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

### 3.1 ball 消费 `ip_packets` / `ip_bytes`（已完成）

**状态**：代码、测试与文档已完成；开发构建与 9 组自动化测试通过。
真实采集器下的端到端已验证：普通用户可读 GUI 快照，IP 两字段正常传递。

**动机**：此前 ball 未读取 `stats` 帧里的这两个字段。它们是与进程 socket I/O
**并列的另一种口径**（采集点的 IP 报文），需要独立展示并解释差异。

**实现情况**：

- `src/daemon/socktrail.{h,cpp}`：`parseStats` 用 `mp::readCount` 读两个可选字段，缺失为 0，
  存在但非法则拒绝整帧；`Socktrail` 在 `applyStats` 里以新值**覆盖**累计（不累加），
  `start()` 清零。
- `src/snapshot.{h,cpp}`、`src/daemon/service.cpp`：顶层 `ip_packets` / `ip_bytes` 进快照，
  保持 64 位精度，与网卡、进程字段分开放置。
- `src/ui/details.cpp`：`details.ipTotals` 单独一行，tooltip 说明三种口径不等价；
  `fresh=false` 或 `process_status` 不可用/过期时显示 `—`，不受所选网卡影响。
- 测试：`tests/mp_frames.h`（`Writer::ipCounters`、`removeMember`）、`tests/socktrail.cpp`、
  `tests/service.cpp`、`tests/fake_collector.cpp`、`tests/ui.cpp`、`tests/ui_preview.cpp` 已同步。
- 文档：`docs/protocol.md` 补充字段、可选规则与「采集器运行累计 / 重启清零」语义；
  README「数据口径」把首次采样 0 与重启清零限定为网卡与进程累计。

**已验证**：

- `cmake --build --preset dev` 与 `ctest --preset dev` 通过（9/9 组测试）。
- 变更文件的 clang-format 与 clang-tidy（`clang-analyzer-*`）检查通过；离屏 UI 预览生成成功。
- 解析测试覆盖两字段分别缺失、同时缺失、64 位整数精度、非法值拒绝整帧。
- 本地 socket 测试覆盖最新帧覆盖、同毫秒延迟应用、坏帧不部分更新、采集器重启。
- UI 测试覆盖 IP 与网卡/进程口径不相加、网卡选择不影响 IP 值、不可用时显示占位符。

**真实环境验证**（2026-10-17）：独立 socktrail 服务 → ball 后台 → 普通用户快照读取成功，
实际包含 4 个网卡、30 多个进程和非零 IP 计数；GUI 已打开。采集器上报了
`unmatched socket I/O`，因此数据仍有不完整标记，不将其隐藏或误报为完整。

**风险**：低。属于纯展示扩展，不影响累积逻辑。

### 3.2 ball 的 unit 支持外部采集器（已完成）

**动机**：此前 `install.sh` 用 `sed` 改写已安装的 unit，重新 `cmake --install`
就会覆盖外部采集器配置。

**实现**：

- 模板仍使用 `--socktrail-bin`；脚本用实例级 drop-in
  `/etc/systemd/system/ball@<user>.service.d/collector.conf` 覆盖命令，先写空 `ExecStart=`
  清除模板值，再设置 `--collector-socket`，不使用 `sh -c`。
- `# X-Ball-Managed=true` 标记归属；切回内置模式与卸载只删本脚本的配置，不删其他
  drop-in。安装遇到同名自定义文件或符号链接时拒绝覆盖，卸载则保留。
- 检查用户名与路径，转义命令参数中的引号、反斜线、`%`、`$`；安装失败清理临时文件。
- drop-in 固定写系统配置目录，不随安装前缀改变。`--no-enable` 只是不启用服务，
  仍会写系统配置并 reload，不能用于无副作用的暂存测试。
- 已运行的实例不会被 `enable --now` 重启；切换模式后需要显式
  `sudo systemctl restart "ball@$USER.service"`，并用 `systemctl cat` 检查其他覆盖配置。

**已验证**：

- `sh -n`、`bash --posix -n` 语法检查与 Ruff 检查、格式检查通过。
- 开发构建与全量 `ctest --preset dev` 通过（10/10 组，其中 `ball_install` 含 15 个用例）。
- 隔离安装回归覆盖配置更新、模板重装、模式切换、卸载保护、自定义文件与符号链接、
  路径校验与转义、临时文件失败清理；提权和服务操作全部被测试替身拦截。
- 真实 `systemd-analyze verify` 在临时 unit 搜索路径中合并实际模板与 drop-in，
  外部与内置模式均通过，包含特殊字符参数。

**真实环境验证**（2026-10-17）：

- 通用 socktrail unit 安装到 `/etc/systemd/system/socktrail.service`；本机单独使用
  `socktrail.service.d/ball.conf` 选择 stats-only 并关闭 `socket-sniff` / `openssl-probe`。
- ball 的 `collector.conf` 生效，`systemd-analyze verify` 通过；两个服务均 enabled / active，
  ball 进程树不含采集器子进程，socktrail 只有一个实例。
- 停止并重新启动 ball 不影响独立采集器，采集器 PID 未改变；重连后普通用户重新读到健康快照。
- socktrail 提示 kTLS 内核探针不可用，但可继续采集；socket I/O 归属缺口仍作为质量提示展示。
- GUI 启动和登录自启配置完成；未进行真实机器重启或重新登录验证。

**剩余限制**：其他用户与共享组组合未验证；ShellCheck 不可用，未运行该静态检查器。

**风险**：其他管理员 drop-in 仍可能覆盖本脚本生成的命令，应检查最终合并配置；
更新已安装的模板不影响外部模式，但重新运行 `install.sh` 时须明确选择采集器模式。

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

无 root、无系统配置变更的自动化验证：

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
# 仅安装脚本隔离测试（fake sudo / cmake / systemctl，全部操作留在临时目录）：
ctest --preset dev -R ball_install

# 只暂存 CMake 安装内容，不调用 install.sh，不写 /etc 或重载服务：
DESTDIR=/tmp/ball-stage cmake --install build/dev
find /tmp/ball-stage -type f | sort
```

实际安装（会写系统级配置，默认启用开机自启；已在当前用户环境验证）：

```sh
./install.sh --collector-socket /run/socktrail/collector.sock
systemctl cat "ball@$USER.service"
systemd-analyze verify "ball@$USER.service"
# 已运行实例需要重启才能应用新的 ExecStart：
sudo systemctl restart "ball@$USER.service"
systemctl status "ball@$USER.service"
```

不传 `--collector-socket` 再次安装可切回内置采集器模式；`--uninstall` 会停用服务，
并移除本脚本的 drop-in，但保留自定义文件和其他 drop-in。

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
