# ball

一个常驻 KDE Plasma Wayland 桌面的网络流量悬浮窗（HUD）。

## 当前状态

本仓库目前只有 **可构建的骨架**：一个无边框半透明、位于 layer-shell `Top` 层的窗口，
以及最小 CLI（`--help` / `--version`）。统计后台、流量采集、进程/网卡视图和
`socktrail` 集成 **尚未实现**，`--daemon` 与 GUI 模式的分流也不存在。

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
ctest --preset dev          # 冒烟测试：ball --version

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
clang-format -i src/*.cpp
clang-tidy -p build/dev --removed-arg=-mno-direct-extern-access \
    --checks='-*,bugprone-*,performance-*,clang-analyzer-*' src/main.cpp
```

`--removed-arg` 用于跳过 Qt 发行版注入、Clang 无法识别的
`-mno-direct-extern-access` 编译参数。

## 代码风格

- 源码、标识符与注释使用英语；面向用户的界面文字（后续实现）统一使用简体中文；文档使用中文。
- 缩进 4 空格，列宽 100，见 `.clang-format`。
- 编译启用 `-Wall -Wextra -Wpedantic`，警告视为需要修复的问题。

## 目录结构

```
CMakeLists.txt      构建入口：可执行文件 ball 与 CTest 冒烟测试
CMakePresets.json   dev / release 的 configure、build、test presets
src/main.cpp        程序入口：CLI 解析与最小 layer-shell 窗口
```
