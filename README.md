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
  ball 不抓包，只管理该子进程并订阅它的 NDJSON 输出。

## 依赖

- CMake ≥ 3.25、Ninja
- 支持 C++23 的编译器
- Qt 6（Core / Gui / Widgets / Network）与 LayerShellQt 的 CMake 包
- 可执行的外部采集器 `socktrail`（见下节）

以上全部来自发行版包，构建过程不联网、不下载任何依赖。

## 安装

### 1. 准备采集器 socktrail

ball 使用本地分支 `perf/wildcard-index`，其中包含两个独立补丁：

- `db7df63`：为本机 Linux 7.2 新增四参数 `recvmsg` 探针。
- `c3a6892`：每轮只索引一次通配连接候选，避免逐条扫描全部流记录。

原检出目录的 `main` 不变。已在本地缓存准备好的 Go 1.27 环境中验证真实 TCP/UDP、
订阅数据及索引优化的单元测试；新版本应用后仍需观察实际 CPU。可在 ball 根目录执行：

```sh
BALL_DIR="$PWD"
mkdir -p "$BALL_DIR/build"
cd ../ball-worktrees/socktrail-perf
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
  窗口内的「置顶」只作用于详情；原来的「固定展开」已移除。
- 拖动流量条：移动位置，靠近边缘时吸附；右键「锁定流量条位置」只禁止移动，不禁止点击。
- 右键「外观」提供三套皮肤：**瓷白**（默认）、**暖纸**、**石墨**；可选择**双行**（默认，上下行各一行）
  或**单行**布局。字号使用系统 UI 字体，数字使用等宽字体；切换皮肤不会更改统计口径。
- 「外观 → 背景不透明度」只改变流量条背景，文字保持不透明；详情保持实色，提高表格可读性。
- 详情顶部按「当前速率 / 今日累计 / 本次累计」分组。进程与网卡表格同时显示当前上下行速率、
  今日上下行累计、本次上下行累计，不需要切换下拉框。可按各数值列排序，按进程名 / PID 或网卡名搜索；
  默认隐藏空闲或历史进程，「显示空闲」可查看其累计记录。窗口较窄时可横向滚动。
- 进程名称优先取经过 PID + 启动时间核验的 `/proc/<pid>/stat`，避免把 `tokio-rt-worker` 等线程名
  当作进程名。后台保留已核验的名称，进程退出后不退回线程名称；无法核验时保留采集器原始名称。
- 鼠标悬停进程行时显示命令行及参数。命令行由普通用户 GUI 按自身权限读取，不经 root 后台转发，
  不缓存、不写日志或磁盘；读取前后都核验进程身份，避免 PID 复用导致串到另一个进程。
  进程已退出、无权限、空命令行或超过 64 KiB 时明确显示不可用，不截断后假装完整。
  参数中的空格、空参数、引号会保留边界。**命令行可能含口令或令牌，分享截图时请自行检查。**
- 右键选择统计网卡。首次默认只选启用的物理网卡；选中网卡部分缺失时会提示，不静默当作完整合计。
- 正常采集时不显示「正常」文字；进程统计有缺口时，流量条显示小型 `!` 标记，详情中解释原因。
  **进程归属缺口不代表网卡速率不可用**。采集连接中断 / 数据过期时，速率显示 `—` 而不是零。
  历史缺口可能在恢复采集后继续提示，因为缺失的累计数据无法补回；「查看原因」展开诊断信息。
- 悬停流量条可查看所选网卡、今日累计和状态摘要。皮肤、布局、背景不透明度、网卡选择、
  流量条位置及详情窗口几何信息会保存；Wayland 下普通详情窗口的最终位置由合成器决定。
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
- **首次有效采样之前都是 0**：第一帧只用于建立基线，不计入任何字节。
- 「今日」为本地日期内的累计，跨本地午夜清空；「本次监测」从后台启动后的首次有效
  采样开始累计，跨午夜不变。

两种统计口径**不相等**，请不要把两个数字对齐：

- 网卡视图：内核接口计数器的观测字节，包含无法归属到进程的流量。
- 进程视图：TCP/UDP 应用 socket I/O 字节，不与接口字节相加。协议头、重传、回环、
  代理、VPN 以及所选网卡范围都会使两者不同；进程合计可能小于，也可能大于网卡合计。
- 首次默认选择启用的物理网卡，可在右键菜单修改；VPN、虚拟接口、回环不会默认混入。
  选定接口统计并不等于运营商计费的纯互联网流量。
- 采集器丢事件、未覆盖路径或网络命名空间限制可能导致进程统计不完整；界面会显示
  已知的质量问题。跨午夜的区间按新日期归属，误差最多一个采样区间。

## 诊断

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

测试不依赖桌面会话与 root；GUI 测试在 `QT_QPA_PLATFORM=offscreen` 下运行。
UI 回归覆盖独立详情窗口的反复开关、失焦保留、Esc、置顶、布局切换、三套配色、偏好恢复、
全部统计列、数值排序和搜索、命令行悬停与转义、历史进程记录，以及进程质量提示不影响网卡速率。

使用假数据和临时偏好生成预览，不会连接真实采集器或改写用户配置：

```sh
QT_QPA_PLATFORM=offscreen ./build/dev/ball_ui_preview /tmp/ball-ui-preview.png
# 详情截图：/tmp/ball-ui-preview.png
# 三套皮肤 × 两种布局：/tmp/ball-ui-preview-bars.png
```

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
src/process.*           PID / 启动时间核验与有界 Linux 进程元数据读取
src/daemon/             流量核心：网卡读取、内存累计、socktrail 订阅、快照服务
src/ui/                 流量条、独立详情窗口、内置配色、托盘与偏好
tests/                  QtTest 单元测试与假采集器 fixture
```
