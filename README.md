# ball

KDE Plasma Wayland 桌面上的网络流量悬浮窗（HUD）。

ball 由一个后台采集守护进程和一个桌面悬浮窗组成：后台以 root 运行，采集网卡与
每进程的收发字节并经本机 Unix socket 发布只读快照；悬浮窗以普通用户运行，只读取
该 socket 并展示。所有统计都保存在内存里。

## 运行环境与限制

- **仅支持 KDE Plasma Wayland**：使用 layer-shell 协议，需要 Qt6 与 LayerShellQt。
- 本项目采用 Qt，只承诺支持 KDE Plasma Wayland；未实现 X11 / GNOME 适配。
- 后台需要 root：AF_PACKET 抓包、eBPF/BTF 程序挂载、读取其他用户的 `/proc/<pid>`
  都要求特权；悬浮窗本身不需要 root，也不做任何特权操作。
- 流量采集由外部 Go 程序 [socktrail](https://github.com/jimyag/socktrail) 完成，
  ball 不抓包，只管理该子进程并订阅它的 MessagePack 输出。

## 依赖

- CMake ≥ 3.25、Ninja
- 支持 C++23 的编译器
- Python 3（仅 `BUILD_TESTING=ON` 时需要，用于安装脚本的隔离回归测试）
- Qt 6（Core / Gui / Widgets / Network）与 LayerShellQt 的 CMake 包
- [msgpack-cxx](https://github.com/msgpack/msgpack-c)（Arch: `msgpack-cxx`；CMake 包名
  `msgpack-cxx`）。后台与界面之间的快照协议和采集器订阅协议都使用 MessagePack。
- 可执行的外部采集器 `socktrail`（见下节），需要以 `--output msgpack` 输出。

以上全部来自发行版包，构建过程不联网、不下载任何依赖。

## 安装

### 1. 准备采集器 socktrail

当前连接页要求采集器发布 `stats,flow`。本次验证使用本地 socktrail 仓库提交
`2fa3b7e`，包含大连接表刷新背压修复、四参数 `recvmsg` 探针、采集热点优化、socket 帧类型选择，以及
`hello.kinds` / `probes.ktls.status` 能力说明。旧采集器未声明 kinds 时，ball 仍能
根据实际收到的合法 flow 确认连接支持，但不会凭空推断 kTLS 探针状态。

以下命令假设 Go 1.27 与依赖缓存已经准备好，不联网下载依赖。默认使用本机检出目录，
也可以通过 `SOCKTRAIL_DIR` 指定其他包含这些功能的检出：

```sh
BALL_DIR="$PWD"
SOCKTRAIL_DIR="${SOCKTRAIL_DIR:-$HOME/Code/0-Cloned/0-Internet/socktrail}"
mkdir -p "$BALL_DIR/build"
cd "$SOCKTRAIL_DIR"
GOTOOLCHAIN=local GOPROXY=off GOSUMDB=off CGO_ENABLED=0 \
  go build -trimpath -o "$BALL_DIR/build/socktrail" .
"$BALL_DIR/build/socktrail" --version
cd "$BALL_DIR"
```

把生成的 `socktrail` 放在一个固定位置，构建 ball 时用
`-DBALL_SOCKTRAIL_BINARY=<绝对路径>` 指向它，安装时会被装到
`<prefix>/libexec/ball/socktrail`。不传该变量时安装不会包含采集器，CMake 会给出
明确警告，需要你自行把 collector 放到这个路径（或在运行时用 `--socktrail-bin` 指定）。

### 2. 配置与构建 ball

**安装前缀必须在 configure 阶段确定。** ball 会把默认采集器路径
`<prefix>/libexec/ball/socktrail` 编译进可执行文件，systemd unit 和 desktop 文件的
`ExecStart` / `Exec` 也在 configure 时写死为绝对路径。因此：

- 用 `-DCMAKE_INSTALL_PREFIX=/usr`（发行版风格）或 `-DCMAKE_INSTALL_PREFIX=/usr/local`
  （默认值）在 configure 时选好；
- **只执行 `cmake --install --prefix ...` 改路径是不够的**：文件会装到新前缀，但
  二进制里编译进去的采集器路径与 unit/desktop 里的绝对路径仍指向旧前缀。

```sh
# 以安装到 /usr 为例；开发调试可用 CMakePresets 的 dev / release
cmake -S . -B build/release -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/usr \
      -DBALL_SOCKTRAIL_BINARY=/path/to/socktrail
cmake --build build/release
```

不使用预设也能离线构建；`BUILD_TESTING=OFF` 可跳过测试目标。

### 3. 安装

推荐用 `install.sh`：它负责构建、安装文件、启用开机自启的后台服务，并写入当前用户的
GUI 登录自启条目。

```sh
./install.sh                     # 构建 + 安装 + 启用开机自启（当前用户）
./install.sh --prefix /usr       # 装到发行版风格前缀
./install.sh --no-enable         # 只安装文件，不启用服务
./install.sh --no-gui-autostart  # 只装后台，不写 GUI 登录自启
./install.sh --uninstall         # 停用并移除本脚本添加的文件
```

前提：存在与用户名同名的私有组（见「socket 权限与私有组」），脚本会在缺失时报错并给出
修复命令。脚本会自动使用 `build/socktrail-new` 或 `build/socktrail` 作为采集器；也可以用
`--collector-socket <绝对路径>` 让后台改为订阅一个已经常驻的采集器（见下文「使用外部采集器」）。
外部模式由 `/etc/systemd/system/ball@<user>.service.d/collector.conf` 配置，不改写安装的模板
unit，重新 `cmake --install` 也不会覆盖它。该配置是系统级文件，不受 `--prefix` 影响；
`--no-enable` 只跳过启用服务，仍会安装文件、更新 drop-in 并执行 `daemon-reload`，**不是沙箱安装**。
只想暂存文件时请用下面的 `DESTDIR` 方式。

脚本仅覆盖或删除带 `# X-Ball-Managed=true` 标记的 `collector.conf`。同名自定义文件或符号
链接会使安装报错；卸载则保留它们。再次安装时不传 `--collector-socket` 会移除本脚本的
覆盖配置，恢复模板的内置采集器模式，其他 drop-in 不变。

也可以手工安装：

```sh
# 先暂存验证安装内容，不写系统目录
DESTDIR=/tmp/ball-stage cmake --install build/release
find /tmp/ball-stage -type f | sort

# 确认无误后再真正安装
sudo cmake --install build/release
```

安装后的文件（以 `CMAKE_INSTALL_PREFIX=/usr` 为例）：

```
/usr/bin/ball
/usr/libexec/ball/socktrail
/usr/lib/systemd/system/ball@.service
/usr/share/applications/ball.desktop
/usr/share/icons/hicolor/scalable/apps/ball.svg
```

## 运行

ball 有两层「自启」，职责不同：

- **后台服务开机自启**：systemd 模板 `ball@<user>.service`，`WantedBy=multi-user.target`，
  开机就起，与是否登录桌面无关。`install.sh` 默认启用它。
- **GUI 登录自启**：当前用户的 `~/.config/autostart/ball.desktop`，只在登录 KDE 会话时
  拉起悬浮窗。`install.sh` 默认写入，也可以之后用托盘/右键菜单的「登录时启动界面」开关。

只装后台不装 GUI 自启，用 `./install.sh --no-gui-autostart`。想临时停掉 GUI 自启而不动
后台，用界面里的开关即可（它只写用户自己的 autostart 文件，不碰 systemd）。

### 后台服务（root，systemd）

unit 是模板 `ball@.service`，实例名就是运行 GUI 的用户名：

```sh
# 前提：该用户必须有同名的私有组（见下节）
sudo systemctl daemon-reload
sudo systemctl enable --now ball@$USER.service

systemctl status ball@$USER.service
sudo systemctl restart ball@$USER.service
sudo systemctl stop ball@$USER.service
sudo systemctl disable --now ball@$USER.service
```

unit 以 `User=root Group=%i` 启动，创建运行目录 `/run/ball-%i`（`0750`，root 所有、
组为 `%i`），并以
`ExecStart=<bindir>/ball --daemon --socket /run/ball-%i/gui.sock --socktrail-bin <libexecdir>/ball/socktrail`
运行。停止时先给守护进程发 SIGTERM，由它自己清理采集子进程；`TimeoutStopSec=10`
之后才会强杀整个进程组。

#### 使用外部采集器（可选）

默认由后台自己拉起并看管采集器。如果采集器已经作为独立服务常驻（例如
socktrail 的 `install-service.sh` 装的 `socktrail.service`），可以让 ball 只订阅它，
不再自己启动一个：

```sh
ball --daemon --socket /run/ball-$USER/gui.sock \
     --collector-socket /run/socktrail/collector.sock
```

也可通过安装脚本为指定实例配置：

```sh
./install.sh --user "$USER" --collector-socket /run/socktrail/collector.sock
systemctl cat "ball@$USER.service"
systemd-analyze verify "ball@$USER.service"
# 若该实例此前已在运行，重启后才会使用新的 ExecStart：
sudo systemctl restart "ball@$USER.service"
```

生成的 drop-in 先用空 `ExecStart=` 清除模板命令，再设置外部订阅命令；不借助 shell 执行。
默认的 `enable --now` 不会重启已运行的实例，因此切换模式或更新路径后应手动重启。
如果还有其他自定义 drop-in，应通过 `systemctl cat` 检查最终合并配置。

- `--collector-socket` 与 `--socktrail-bin` **互斥**：前者只订阅，后者才 spawn。
- 这条模式下 ball **不会**启动或停止采集器，采集器进程的生死由它的 unit 决定。
  因此「关闭后台不留下采集子进程」的保证不适用——采集器本来就不属于 ball。
- 采集器的 socket 必须对运行后台的用户可读写。systemd 常驻采集器默认是 `0660` +
  组 root，而 ball 后台以 root 运行，所以直接用即可；若消费方在别的账号，需要在
  采集器侧把 socket 组改成共享组。
- 采集器需要以 `--socket-kinds stats,flow` 运行，或把该参数写进 unit 的 drop-in：
  连接页依赖 `flow` 帧；只给 `stats` 时进程与网卡照常工作，连接页明确显示「未收到 flow
  数据」。订阅 kinds 是启动参数，切换 kind **必须重配并重启**正在运行的外部采集器，
  随后由 ball 后台自动重连采集器的 socket；ball 不会自动改动或重启任何正在运行的服务。
  ball 本身也不会自动部署或升级任何采集器：管理员需要显式更新采集器二进制、加上
  `flow` kind 并重配/重启现有服务，连接页才会出现数据。

### GUI（普通用户）

普通用户直接启动桌面程序即可，它默认连接 `/run/ball-<当前用户名>/gui.sock`：

```sh
ball              # 或从应用菜单启动「ball 流量悬浮窗」
```

也可以从 KDE 应用菜单启动（desktop 文件的 `Exec` 是绝对路径）。**不要用 sudo
运行 GUI**：程序明确拒绝以 root 启动 GUI。

## 界面操作

- 左键点击流量条：打开独立的「流量详情」窗口；再次点击唤回同一个窗口，不改变流量条大小和位置。
- 详情窗口是普通非模态窗口，可移动、缩放、最小化，并通过任务栏 / Alt-Tab 找回。
  点击其他应用不会自动关闭；Esc 或标题栏关闭只关闭详情，不影响流量条和后台。
  保留系统默认标题栏及窗口控制，不提供「置顶」或「固定展开」开关。
- 详情窗口分「概览」「进程」「网卡」「连接」四页，默认打开概览。概览突出所选网卡的实时速率、
  今日 / 本次累计与最近五分钟的上下行趋势，并列出按 socket 总速率排序的前五个活跃进程。
  趋势只保存在 GUI 内存中：隐藏详情窗口时仍记录，退出界面后清空；切换统计网卡、后台重启或
  时间倒退时重新记录。数据中断、缺失网卡及超过采样间隔的空档显示缺口，不补零、不平滑插值。
  网卡图表与进程 socket I/O 是不同口径，不互相相加。
- 概览或进程页双击某行，或在进程页选中一行后点「查看连接」，
  会切到连接页并按该进程的精确身份（PID + 启动时间）筛选；连接页顶部显示可清除的筛选标签，
  「清除进程筛选」恢复全部进程的连接。排序、搜索后下钻仍按真实身份匹配，不按原列表行号。
  实时刷新会保留选中进程的身份；统计卡片只在概览展示，其他页优先保留查询空间。
  采集器重连或后台重启会清除旧连接选择，避免连接 ID 复用串选。
- 拖动流量条：移动位置，靠近边缘时吸附；右键「锁定流量条位置」只禁止移动，不禁止点击。
- 右键「外观」提供三套皮肤：**瓷白**（默认）、**暖纸**、**石墨**；可选择**双行**（默认，上下行各一行）
  或**单行**布局。字号使用系统 UI 字体，数字使用等宽字体；切换皮肤不会更改统计口径。
- 「外观 → 背景不透明度」只改变流量条背景，文字保持不透明；详情保持实色，提高表格可读性。
- 概览按「当前速率 / 今日累计 / 本次累计」分组，实时速率使用更醒目的字号。
  进程与网卡表格默认显示当前速率和今日累计，「状态」「本次累计」通过「显示列」或右键表头展开；
  所有原始指标仍可排序、搜索，显示列选择在本次界面会话中保留。
  默认隐藏空闲或历史进程，「显示空闲」可查看其累计记录。所有列均可拖动表头分隔线调整宽度，
  实时刷新不会重置手动列宽；窗口较窄时可横向滚动。连接页也支持选择显示列，默认收起来源和状态，
  完整信息仍在连接详情中；拖动表格与详情之间的分隔线可调整两块区域的大小。
- 概览底部有一行单独的「IP 报文字节 / 报文数（采集器本次运行累计）」，与网卡、进程两栏
  并列而不相加。悬停可看到口径说明：这是采集点观测到的 IP 报文，既不等于进程 socket I/O，
  也不等于网卡计数，且不随所选网卡变化。进程采集不可用、数据过期或还没有快照时显示 `—`。
- 进程名称优先取经过 PID + 启动时间核验的 `/proc/<pid>/stat`，避免把 `tokio-rt-worker` 等线程名
  当作进程名。后台保留已核验的名称，进程退出后不退回线程名称；无法核验时保留采集器原始名称。
- 连接页自带搜索与过滤（搜索端点 / 域名 / 应用 / 进程名 / PID，协议、活动或已结束状态、
  排名指标、TopN、暂停 / 复制），进程页与网卡页的搜索框和「显示空闲」在这一页会隐藏，避免重复。
  查询顺序为：有界缓存全部记录 → 组合筛选 → 按所选指标排序 → TopN；相同值按连接 ID 排序。
  TopN 取值 0..8192，其中 0 表示全部；缓存固定的 8192 行上限与 8 MiB 序列化预算意味着
  达到上限时会淘汰最早结束 / 最久未更新的记录，摘要明确提示「缓存已截断，非全量历史」。
  暂停只冻结连接表显示，恢复后回到最新帧，不清空连接自己的查询。
- 连接详情区分三种进程关联：`client` / `server` 是建立或接受连接的进程，`io[]` 是实际
  搬运字节的进程；下钻筛选对三者都匹配 `(PID, start_ns)`，绝不退化为只看 PID。
- 连接 RX / TX 是采集点观测到的 IP 报文字节方向，不承诺等于应用下载 / 上传，也不是网卡计数；
  累计从采集器首次观测该连接开始，不命名为「今日」或「本次监测」。
- 鼠标悬停进程行时显示命令行及参数。命令行由普通用户 GUI 按自身权限读取，不经 root 后台转发，
  不缓存、不写日志或磁盘；读取前后都核验进程身份，避免 PID 复用导致串到另一个进程。
  进程已退出、无权限、空命令行或超过 64 KiB 时明确显示不可用，不截断后假装完整。
  参数中的空格、空参数、引号会保留边界。**命令行可能含口令或令牌，分享截图时请自行检查。**
- 右键选择统计网卡。首次默认只选启用的物理网卡；选中网卡部分缺失时会提示，不静默当作完整合计。
- 正常采集时不显示「正常」文字，也不为右侧状态标签预留空白；出现提示时才按文字宽度展开。
  进程统计有缺口时，流量条显示小型 `!` 标记，详情中解释原因。
  **进程归属缺口不代表网卡速率不可用**。采集连接中断 / 数据过期时，速率显示 `—` 而不是零。
  历史缺口可能在恢复采集后继续提示，因为缺失的累计数据无法补回；「查看原因」展开诊断信息。
  后台给出的 `process_status.info` 只作为中性纯文本说明单独显示，不会让流量条显示 `!` 或
  把进程统计标为不完整；外部文本始终按纯文本呈现，不会被当作 HTML 执行。
  连接采集不可用或未收到 flow 数据只影响连接页，不影响网卡与进程数值。
- 悬停流量条可查看所选网卡、今日累计和状态摘要。皮肤、布局、背景不透明度、网卡选择、
  流量条位置及详情窗口几何信息会保存；关闭、退出时会同步位置，重新打开恢复最后的请求位置。
  显示器布局改变时会将流量条限制在可见区域；Wayland 下普通详情窗口的最终位置由合成器决定。
- 右键或托盘菜单的「登录时启动界面」控制当前用户 GUI 登录自启，默认不自行开启。
  它不停止或禁用独立后台服务；操作失败会显示错误并恢复开关状态。
  自启文件位于 `~/.config/autostart/ball.desktop`，不会写系统自启目录。
  可执行路径含 `%` 时暂不支持自动登录启动，会明确报错而非写入失效文件。
- 托盘或右键可隐藏 / 恢复流量条，不联动关闭详情；「退出界面」关闭所有 GUI 窗口，但不会停止后台。

### 前台开发运行

开发时可以让 ball 同时充当后台：

```sh
# 手动启动时先准备目录及目标组；后台不会自动创建 /run 目录
sudo install -d -o root -g "$(id -gn)" -m 0750 "/run/ball-$USER"
sudo -u root -g "$(id -gn)" ./build/dev/ball --daemon \
    --socket "/run/ball-$USER/gui.sock" \
    --socktrail-bin "$PWD/build/socktrail"
# 前台后台用 Ctrl+C 停止；另开一个终端，以普通用户启动 GUI
./build/dev/ball
```

`ball --help` 列出全部参数，`ball --version` 打印版本。daemon 模式**必须**指定
绝对路径 `--socket`；`--socktrail-bin` 可省略，默认使用编译期安装路径。
GUI 的 `--socket` 可省略，默认连接当前用户名对应的运行目录。
手动运行后台时请显式指定 `sudo -u root`：只给 `sudo -g` 并不把用户提升为 root，
可能因目录权限不足而无法创建锁文件。安装后建议由 systemd 管理后台，不重复启动同一实例。

## socket 权限与私有组

- 运行目录 `/run/ball-<用户名>` 由 systemd 创建：`0750`、root 所有、组为同名私有组，
  其他用户无法进入。
- 快照 socket 本身权限是 `0660`：root 与同名私有组成员可读写，其他人不行。
- systemd 启动实例时解析 `Group=%i`，该同名私有组必须存在，GUI 用户也须属于该组。
  没有该组时可由管理员准备：

```sh
id -gn              # 当前用户的主组名，应与用户名一致
getent group "$USER"  # 有输出且组名等于用户名即可
# 缺少时（需要 root）：
sudo groupadd "$USER" && sudo usermod -aG "$USER" "$USER"
```

只需同名组存在且用户属于它，不要求它一定是主组。新增补充组后需重新登录，
使当前会话获得组权限；否则 GUI 仍可能无法连接。请不要改成所有用户可读写的 socket。

## 数据口径

ball 只把统计放在内存中，不落盘、不保存历史，因此：

- **关闭 GUI 不会丢失数据**：后台继续采样与累计，重新打开窗口后仍显示同一后台会话
  的累计值。
- **后台重启会清零「今日」和「本次监测」**：`started` 变为新时刻，之前累计不再保留。
- **仅重启 socktrail 子采集器不清空已有累计**：守护进程会重新建立进程计数基线。
  重连期间会出现进程流量采样空档；网卡累计不受此影响。
- **首次有效采样之前都是 0**：第一帧只用于建立基线，不计入任何字节。这只适用于
  **网卡与进程**两类观测累计，它们的首次采样都发生在后台启动之后。
- 「今日」为本地日期内的累计，跨本地午夜清空；「本次监测」从后台启动后的首次有效
  采样开始累计，跨午夜不变。
- 「IP 报文字节 / 报文数」不属于上面两类：它是**采集器进程自己的运行累计**，从采集器
  启动开始递增，**新值整体覆盖旧值而非续加**，也不按「今日」或「本次监测」分段，因此
  不能与网卡、进程累计相互换算。外部采集器模式下，采集器继续运行时 ball 后台重启
  不会清零；spawn 模式下后台重启必然连带重启采集器，该行也随之从零开始。

三类统计口径**互不相等**，请不要把它们对齐：

- 网卡视图：内核接口计数器的观测字节，包含无法归属到进程的流量。
- 进程视图：TCP/UDP 应用 socket I/O 字节，不与接口字节相加。协议头、重传、回环、
  代理、VPN 以及所选网卡范围都会使两者不同；进程合计可能小于，也可能大于网卡合计。
- 第三类口径在详情窗口单独一行：采集点的 IP 报文字节与报文数。它与上面两类同样
  **不相等、不相加**，也不受所选网卡影响；数值的起点是采集器启动时刻，而非后台监测起点。
- 首次默认选择启用的物理网卡，可在右键菜单修改；VPN、虚拟接口、回环不会默认混入。
  选定接口统计并不等于运营商计费的纯互联网流量。
- 采集器丢事件、未覆盖路径或网络命名空间限制可能导致进程统计不完整；界面会显示
  已知的质量问题。跨午夜的区间按新日期归属，误差最多一个采样区间。

## 诊断

`QLocalSocket: Remote closed` 需要区分连接层级：GUI 到 ball 后台、ball 后台到采集器。
若网卡快照持续刷新，而 `process_status.available=false`、`stale=true`，错误来自采集器订阅，
不能通过隐藏界面提示来修复。新版本分别报告 `backend connection closed` 和
`collector connection closed`；界面会解释哪一层断开，保持不可用速率并自动重连。
连接成功不等于数据恢复，只有有效新快照 / stats 才清除错误；主动拒绝坏帧时保留具体解析原因。

后台日志：

```sh
journalctl -u ball@$USER.service -b -n 100 --no-pager
journalctl -u ball@$USER.service -f      # Ctrl+C 停止跟随
```

**界面没有任何数据**时按顺序检查：

1. 后台是否在跑：`systemctl status ball@$USER.service`。
2. socket 是否存在、权限是否正确：
   `ls -l /run/ball-$USER/gui.sock`（应为 `srw-rw---- root <用户组>`）。
3. GUI 与后台实例名是否一致（GUI 默认按当前用户名找 socket，后台 `%i` 由
   `ball@<用户名>` 决定）。
4. 采集器是否可执行：日志里出现 `no collector configured` 或采集器启动失败时，
   检查 `<prefix>/libexec/ball/socktrail` 是否存在且可执行，或用 `--socktrail-bin`
   显式指定。
5. 内核是否支持采集：socktrail 依赖较新内核的 eBPF/BTF 与 AF_PACKET。若日志提示
   BTF / eBPF 不可用，先确认内核与 `socktrail` 的运行环境；此时网卡视图通常仍有
   数据，进程视图为空。

网卡视图有数据、进程视图为空是常见状态，代表采集器不可用或未获得足够内核支持，
而不是后台崩溃。

## 构建与测试

### 采集器热点验证

`reconcileWildcards()` 的索引优化保持进程归属与歧义处理语义不变。独立微基准包含
10,000 条流与 1,000 条通配记录，本机旧版约 88.5 ms/次，索引版约 0.83 ms/次，
每次临时分配约增加 144 KiB。该倍率只代表目标函数，不等于整个采集器的 CPU 降幅；
采集器重启后的连接数量也会影响实测 CPU，不能仅比较重启前后瞬时值。

在上述 socktrail 补丁工作树中运行：

```sh
GOTOOLCHAIN=local GOPROXY=off go test ./...
GOTOOLCHAIN=local GOPROXY=off go test ./internal/app -run '^$' \
  -bench '^BenchmarkReconcileWildcardsLarge$' -benchmem -count=3
```

### ball 验证

```sh
cmake --preset dev          # Debug + BUILD_TESTING=ON，产物在 build/dev
cmake --build --preset dev
ctest --preset dev          # 全部单元测试

cmake --preset release && cmake --build --preset release
```

`dev` 和 `release` 的 build preset 默认使用 **4 路并行编译**（`CMakePresets.json` 中的 `jobs`）。
可用 `cmake --build --preset release --parallel 8` 临时覆盖。直接指定构建目录时不会读取 preset，
例如 `cmake --build build/local-deploy --parallel 4`；安装脚本使用 Ninja 的默认并行行为。

测试不依赖桌面会话与 root；GUI 测试在 `QT_QPA_PLATFORM=offscreen` 下运行。
`ball_install` 用 Python 标准库和临时目录模拟提权命令、安装与 systemctl，不访问真实 `/etc`
或启停服务；覆盖外部配置、重装、模式切换、卸载保护、路径转义与失败清理。
UI 回归覆盖独立详情窗口的反复开关、失焦保留、Esc、系统标题栏、列宽拖动、布局切换、三套配色、位置恢复、
全部统计列、数值排序和搜索、命令行悬停与转义、历史进程记录，以及进程质量提示不影响网卡速率。
新增的概览回归覆盖：五分钟有界采样、重复时间戳、作用域 / 后台会话切换、无效速率与中断缺口、
曲线渲染、Top5 精确进程身份下钻和列菜单；连接页回归覆盖四页 tab、按速率排序后的进程身份下钻（含 2^53+1 的 start_ns 精度）、
双击与「查看连接」按钮两条入口、清除进程筛选、以及仅 `info[]` 时不显示 `!` 且按纯文本渲染。
采集器订阅与快照协议的回归还覆盖 IP 两个可选字段：缺失为 0、非法取值拒绝整帧、
超过 2^53 不失真、采集器重启后覆盖而非累加，以及详情中 IP 行不随所选网卡变化。

使用假数据和临时偏好生成预览，不会连接真实采集器或改写用户配置：

```sh
QT_QPA_PLATFORM=offscreen ./build/dev/ball_ui_preview /tmp/ball-ui-preview.png
# 概览截图：/tmp/ball-ui-preview.png
# 深色概览：/tmp/ball-ui-preview-dark.png
# 进程页截图：/tmp/ball-ui-preview-processes.png
# 连接页截图：/tmp/ball-ui-preview-connections.png
# 三套皮肤 × 两种布局：/tmp/ball-ui-preview-bars.png
```

预览使用合成的五分钟趋势（包含一次断流缺口）和三条不同协议 / 域名 / 进程的连接，
不读取真实域名或命令行。

真实 KDE Wayland 下还应检查：详情作为普通窗口接收键盘输入、任务栏 / Alt-Tab 唤回、最小化恢复，
详情开关不改变流量条尺寸与位置，以及不同缩放比例和多屏布局。
离屏测试不能代替真实合成器的焦点、激活、物理多屏热插拔或全屏应用组合验证。
验证安装内容而不写系统目录：

```sh
DESTDIR=/tmp/ball-stage cmake --install build/release
desktop-file-validate /tmp/ball-stage/usr/share/applications/ball.desktop
systemd-analyze verify /tmp/ball-stage/usr/lib/systemd/system/ball@.service
```

`systemd-analyze verify` 在暂存目录里会因为 `ExecStart` 的绝对路径在该前缀下不存在
而报 `is not executable`——这是预期的暂存副作用；把 `ExecStart` 临时改成本地已构建
的二进制后应验证通过，若仍有其他语法错误则是 unit 本身的问题。

## 目录结构

```
CMakeLists.txt          构建入口、安装规则、配置生成 unit / desktop
CMakePresets.json       dev / release 的 configure、build、test presets
packaging/              ball@.service.in、ball.desktop.in、ball.svg
docs/protocol.md        后台与 GUI 之间的本地快照协议
src/main.cpp            程序入口：CLI 解析与 layer-shell 窗口/Daemon 模式
src/messagepack.*       增量 MessagePack 读取器与严格字段读取
src/snapshot.*          快照结构、线格式字段名，以及编码器与解码器
src/process.*           PID / 启动时间核验与有界 Linux 进程元数据读取
src/daemon/             流量核心：网卡读取、内存累计、socktrail 订阅、快照服务
src/ui/                 流量条、独立详情窗口、内置配色、托盘与偏好
tests/                  QtTest 单元测试与假采集器 fixture
```
