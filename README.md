# UQuick — Ubuntu 原生极速启动器

用 C + GTK4 写的 uTools 替代品：**一个 84 KB 的二进制、常驻进程预热窗口、按下热键 1 帧内上屏**。
没有 Electron、没有 Python、没有 Node，也不打包运行时。

```
名称        UQuick 0.1.0
语言        C（GTK4 / GLib / GIO）
二进制      84 KB（strip 前；snap 包 32 KB）
索引        全部 .desktop 应用 + 内置动作 + 自定义命令
搜索        4.2 µs / 次（约 24 万次/秒）
热键→首帧    ≈ 1 帧（60 Hz 下 15 ms；快路径客户端 10 ms）
```

## 功能

| 输入 | 结果 |
|---|---|
| `chr` / `web` / `设置` | 模糊搜索并启动应用（名称、GenericName、Keywords、desktop id，中文可用） |
| `1+2*3+sqrt(16)` / `15%3` / `=` 前缀 | 内联算式，回车把结果复制到剪贴板 |
| `https://…` / `www.…` | 用默认浏览器打开 |
| `/home/…` / `~/Downloads` | 在文件管理器中打开 |
| `>ls -la` | 交给 `/bin/sh` 执行 |
| `锁屏` `挂起` `注销` `重启` `关机` `Wi-Fi` `蓝牙` `显示` `声音` `电源` | 内置系统动作 |

排序 = 匹配分 + 使用热度（frecency，按次数与衰减时间计算），越用越顺手。

## 安装

### A. 本项目源码构建（推荐先用这个）

```bash
meson setup build --prefix=/usr
ninja -C build
sudo meson install -C build     # /usr/bin/uquick + .desktop + AppStream + 系统级自启
./contrib/install-hotkey.sh -f  # 绑定 <Super>space（可 -b '<Alt>space'），写用户级自启
```

### B. snap（classic 权限，见下文「为什么是 classic」）

```bash
sudo snap install --classic --edge uquick
./contrib/install-hotkey.sh -f
```

> 快照名 `uquick` 已注册；`0.1.0` 已上传到 edge，正在等待 store 的 classic 权限人工审核。
> 审核通过前可用 `sudo snap install --classic --dangerous uquick_0.1.0_amd64.snap` 本地安装。

## 热键链路（为什么这么快）

Wayland 下应用不能自己抓全局按键（GNOME 49+ 的 mutter 只在 X11 窗口持有焦点时转发抓取），
所以走「GNOME 自定义快捷键 + 常驻进程」这条路：

```
物理按键 → gnome-settings-daemon → exec 客户端（10 ms）
        → unix socket（/run/user/1000/uquick.sock）
        → 常驻进程在「已建好的窗口」上 present + 换文字
        → 合成器下一帧上屏
```

关键点：**窗口、列表行、图标缓存都在守护进程里预先建好**，热键路径只剩「改文字 + present」，
所以没有冷启动的建窗口开销。首次冷启动（守护进程不在）约 190 ms，之后都是 1 帧。

`contrib/install-hotkey.sh -f` 用的是快路径客户端：

```bash
sh -c 'printf toggle | nc -U /run/user/1000/uquick.sock 2>/dev/null || exec /snap/bin/uquick --toggle'
```

`nc` 直连 socket 约 10 ms；`snap run` 每次都带 55 ms 的 snap-confine 开销，所以不想用 nc 时
至少保证守护进程常驻（自启项已装），退化也只有几十毫秒。

## 命令行

```bash
uquick                 # 切换面板（守护进程不在则拉起）
uquick --toggle        # 同上
uquick --show / --hide
uquick --query "chr"   # 显示并预置查询（脚本/截图用）
uquick --run "chromium"# 按查询结果直接启动首个命中
uquick --reload        # 重载索引与自定义命令（也可 kill -HUP）
uquick --daemon        # 前台常驻
uquick --quit
uquick --bench         # 索引 + 搜索基准
uquick --bench-show    # 热键到首帧延迟基准（需要显示环境）
uquick --stats         # 索引项数、扫描耗时、socket、守护进程状态
uquick --explain TEXT  # 打印带分数的命中列表（调排序用）
```

## 自定义命令

`~/.config/uquick/commands.tsv`，每行 `名称<TAB>命令[<TAB>关键词]`，改完 `uquick --reload`：

```
重载索引	uquick --reload
开代理	~/.local/bin/proxy-on	proxy vpn
```

## 数据与路径

| 内容 | 路径 |
|---|---|
| 配置 | `~/.config/uquick/commands.tsv` |
| 使用热度 | `~/.local/state/uquick/frecency` |
| 延迟日志（`UQUICK_TRACE=1` 时） | `~/.local/state/uquick/latency.log` |
| IPC socket | `$XDG_RUNTIME_DIR/uquick.sock` |

## 实测数据（Ubuntu 26.04 / GNOME 50 / Wayland / Ryzen 本机）

```
$ uquick --bench
索引：98 项，扫描耗时 2.88 ms
搜索：20000 次 / 83.5 ms，平均 4.18 µs/次（24.0 万次/秒），命中均值 8.4

$ uquick --bench-show
请求 -> surface 提交 : avg=7.79 ms  min=0.05  max=154.72
请求 -> 首帧上屏     : avg=26.69 ms  min=14.71  max=182.71
```

- **4 µs/次** 来自「预折叠 haystack + 无分配打分 + 定长结果数组插入」；
- 上屏时间基本等于 1 帧（min 14.7 ms ≈ 60 Hz 的 16.7 ms），说明瓶颈在合成器 vblank，不在我们；
- 冷启动（`pkill uquick; time uquick --show`）：客户端 190 ms 拿到 socket，首帧 210 ms。

复测：`./build/uquick --bench`、`./build/uquick --bench-show`、`UQUICK_TRACE=1 ./build/uquick --daemon`
后按热键，`~/.local/state/uquick/latency.log` 里是每帧延迟（微秒）。

## 实现要点

- `src/main.c` —— 模式分派、unix socket 服务端（`g_unix_fd_add`）、客户端（纯 POSIX，不初始化 GTK）
- `src/core.c` —— XDG applications 扫描（GKeyFile 解析、OnlyShowIn/NoDisplay/TryExec 处理）、
  模糊打分（连续子串 > 词边界 > 子序列，子序列只在名称段）、frecency
- `src/calc.c` —— 递归下降求值器（+ - * / % ^ 括号、一元负号、sqrt/abs/log/ln/sin 等、pi/e/tau）
- `src/launch.c` —— GDesktopAppInfo 启动（含 Terminal=true、启动通知上下文）+ 失败回落、剪贴板、URI/路径
- `src/ui.c` —— 单窗口、10 个可复用行控件、图标缓存、CAPTURE 阶段键盘处理、帧时钟延迟测量

## 为什么是 classic 权限（重要）

启动器的职责是**执行宿主机上任意 .desktop 程序与用户自定义命令**。strict 沙箱做不到：

```
$ snap run --shell shot-screenshot.shot -c 'ls /usr/bin | wc -l; ls /usr/share/applications'
468                      # 只有 base 快照的 /usr，宿主机 2850 个
mimeapps.list  xdg-open.desktop   # 看不到宿主机的应用清单
```

所以只能是 `confinement: classic`（store 的 classic 支持类别里明确包含
「terminal emulators, multiplexers and shells」与「running arbitrary/user-configurable commands」）。
注意 classic 快照**不允许声明 plugs/slots**，否则上传直接被拒：

```
- confinement 'classic' not allowed with plugs/slots
```

classic 需要 store 团队人工审核（forum 的 `classic-confinement` 分类）。审核通过前只能装 edge/dev。

## 打包与商店素材

```bash
sudo snapcraft pack --destructive-mode        # -> uquick_0.1.0_amd64.snap（32 KB）
snapcraft upload --release=edge uquick_0.1.0_amd64.snap
./contrib/mkstore-shots.sh                    # 离屏生成 1920x1080 商店截图到 store/
```

`mkstore-shots.sh` 用 Xvfb 空屏 + 只截自己的窗口 + 圆角遮罩 + 自绘背景，**绝不拍真实桌面**
（避免把用户窗口带进商店素材）。商店截图只能通过 dashboard 网页上传。

## 已知限制

- 只测过 GNOME 50 / Wayland / Ubuntu 26.04；classic 快照用的是宿主机的 GTK4，要求宿主机 GTK ≥ 4.12。
- 没有插件市场，插件能力由 `commands.tsv`（任意 shell 命令）+ 前缀语法承担。
- 剪贴板历史未实现（Wayland 下需要常驻读取或 portal，留待后续）。
