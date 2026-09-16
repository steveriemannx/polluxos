# PolluxOS 桌面：向 macOS 26 看齐

## Context

PolluxOS 桌面壳（`userland/polluxdesk/`）已能运行：全屏 dui 窗口 + wlroots 合成器，顶栏与
Dock 正常渲染，中文与启动路径已修。用户要求继续向 macOS 26 靠拢，范围在对话中扩到七项：
**Dock 右侧（分隔线+废纸篓）**、**运行中程序指示点**、**最小化窗口收纳区**、**Settings 完善**、
**文件浏览器完善**、**液态玻璃 + 主题**、**动效**。全部纳入本次实施，无排除项。

## ⚠️ 先决：8 处陈旧路径正导致功能失效

目录由 `polluxos-main` 改名为 `polluxos` 后路径未更新，旧目录已不存在，`sh -c` 以 127 失败，
界面表现为「点了没反应」：`shell/PolluxOSForm.cpp` 第 **58/130/132/207/250/693** 行、
`shell/LaunchPadForm.cpp:337`、`apps/LaunchPadForm.cpp:335`。
**这是既有功能损坏**（Dock 的设置/文件图标、菜单-文件管理器、控制中心、启动台、退出登录全部失效），
必须第一步修掉。

## 能力边界（已核实，决定哪些能真做）

| 能力 | 结论 |
| :--- | :--- |
| **真背景模糊** | ❌ wlroots 0.20.2 **无** `wlr_scene_blur`；dui 也未向 Control/Box 暴露模糊（`SkImageFilters::Blur` 只在 `Render_Skia.cpp:1939` 画弹窗阴影）。真模糊须合成器自定义 GL 着色器 —— 独立大工程 |
| **旧线怎么做玻璃** | 旧 `galaxyos` **也是假的**（注释自陈：半透明白叠渐变壁纸"读起来像磨砂玻璃"）。当前 Dock 与旧线同水平 |
| **动画** | ✅ dui 有完整 API：`AnimationManager`（`SetFadeAlpha/Width/Height/Size`、`SetFadeInOutX/Y`、`Appear/Disappear`）+ `AnimationPlayer`（起止值/时长/缓动）；`Core/Control.h` 继承 `AnimationPlayer`，**所有控件可动画** |
| **主题/深色** | ⚠️ dui 无运行时主题切换。`themes/macos26` 存在但**切它有害**（其 `DefaultFontFamilyNames` 只有 PingFang/Noto，**丢 CJK 回退链会让所有中文变方框**）。故**不切主题**，只做桌面壳 + 合成器自己的配色 |
| **合成器标题栏** | 纯 C 硬编码配色，深色需同步改 |

## 关键机制与陷阱（决定实现方式）

- **shell→合成器只有窗口标题一条通道**（`strncmp(title,"PolluxOS Desktop",16)` + `strcmp(...,"(menu)")`）。
  它是**单向**的，合成器无法用它反问桌面壳。
  **阶段 3b 补上反方向**：合成器写状态文件、桌面壳轮询 —— 运行指示点与最小化收纳都依赖它。
- **配置与状态一律用「文件 + mtime 轮询」**：shell 已有 1 秒时钟定时器，在回调里 `stat()` 比较 mtime 即可，
  无需信号处理器、无需新协议，且手改配置也能生效。≤1 秒延迟对 Dock/外观完全无感。
  设置（用户意图）与窗口状态（合成器汇报）都走这个模式。
- ⚠️ **`BuildUi()` 绝不能在事件处理器里调用** —— `Window::AttachBox` 会**同步删除**旧控件树 → use-after-free。
  重建必须通过 `m_settingsDirty` 标志 + 定时器**延迟**执行。
- ⚠️ **`AttachMenuDismissHandlers()` 必须加一次性守卫** —— `EventSource::AddEventCallback` 是**追加**语义，
  重建时会叠加重复的窗口级处理器，导致菜单"点了立刻关"。
- ⚠️ **`SetStateColorRound(r, r)` 与 `border_round="r,r"` 是两个独立 API**，必须给同一个数，
  否则静默画出两个不同圆角。抽成一个 `SetRadius()` 助手统一。
- ⚠️ **现存 bug**：合成器只在初始提交与改标题时给桌面壳设尺寸，**分辨率切换后 Dock 会浮在半空**。
  面板 3（显示器）要真正可用，必须先修这个。
- ⚠️ **`dui` 子模块不可改**（其构建已提交且工作正常）。资源根 `EMBED_RES_DIR` 硬绑 `${DUI_ROOT}/resources`，
  所以**不能加新图标文件** —— 废纸篓等要用 dui 图元手绘（`FilesForm` 已有 `MakeFolderIcon` 的先例）。

## 现状要点

- **Dock** `BuildDock()`（723）：嵌套 `HBox`，毛玻璃条 `#E6FFFFFF` + `border_round="18,18"`；
  每图标 `ui::Button` 56×56（嵌入 SVG + 渐变底 + 圆角 14，中文名仅 tooltip）；`kDockApps[]` 是**编译期静态 5 项**
- **Settings**（364 行）：4 个按钮侧边栏 + 整块重建面板；**仅壁纸真正生效**（写配置 + `pkill -USR1` 通知合成器）；
  网络是硬编码、Wi-Fi 是假开关（自标"界面演示"）；配置用 `fopen(...,"a+")` 追加 → **累积重复行**
- **文件浏览器**：初始目录固定 `$HOME`，**不接受 argv**
- **合成器已有**：`SIGUSR1` → `reload_user_settings()` 解析 `wallpaper=`/`resolution=`；最小化/最大化/Alt+Tab
- **登录界面**：`polluxdesk/` 内**无任何源码**，现用旧线外部二进制 `launcher_code`（GDM 风格 PAM）。
  自建 = 从零写新组件 → **列为后续，本次不做**
- **可用**：`mixer`（音量，`mixer -o` 可脚本解析）、`ifconfig`（含 `wlan0 list scan` 免 root 可扫）、
  `sysctl`（型号/核数/内存/版本）；控件 `ListCtrl`/`Option`/`Slider`/`CheckBox`/`TabBox`/`TreeView`；
  样式类 `option_1`、`slider_horizontal_green`、`btn_recycle`

## 方案（每阶段独立可验证、可回滚）

依赖顺序：`0 → 1 → 2 → 3 → 3b → 3c/3d → 4 → 5 → 6 → 7`
（3b 是 3c/3d 的前提；4 依赖 1 与 3 打好的尺寸/半径基础；6 的显示器面板依赖 5b）


### 阶段 0 — 修路径（前置，~5 分钟）
新增 `common/PolluxPaths.h`（`#define POLLUX_BIN "$HOME/projects-main/polluxos/build/polluxdesk/bin"`，
靠相邻字符串字面量拼接用于静态命令表），替换上列 8 处；同时把菜单里仍是"cat README"的
「系统设置」项改为真正启动 `polluxdesk_settings`。各组件 CMakeLists 加
`include_directories(.../common)`（**不要**把 `common` 加进顶层 `foreach` 循环，它没有 CMakeLists）。

### 阶段 1 — 设置模块（~30 分钟）
新增 `common/PolluxSettings.h`（header-only、无 dui 依赖）：键名、`struct Settings`、
`Load()`/`SaveKey()`（**读全量→改一键→写临时文件→fsync→rename**，修掉追加累积且并发安全）、
`MtimeNs()`、`DockIconPx()`、`ResolveDark()`（auto→按本地小时判）、`AccentHex()`、`Run()`（popen 助手）。
合成器侧**不动** —— 它用前缀匹配，未知键本就忽略。

### 阶段 2 — 文件浏览器接受目录（~20 分钟）
`files/main.cpp` 改用 `DUI_APP_ENTRY_ARGS(App)`，`argv[1]` 作起始目录（失败回退 `$HOME` → `/`，
`Navigate()` 已有错误提示）。废纸篓要用它。

### 阶段 3 — Dock 右侧：分隔线 + 废纸篓（~1 小时）
**放进 `pDock` 内、应用图标之后**（macOS 就是这样：Dock 是一条居中的条，废纸篓在条内），
外层 `child_align="hcenter,vcenter"` 的居中机制**不受影响**。
- 分隔线：1px `ui::Control`，高约条高的 62%，左右 7px 边距；**浅色模式用深色发丝线**（`#33000000`）
- 废纸篓：`ui::ButtonVBox`（Box 派生，可容纳子控件）+ dui 图元**手绘**垃圾桶（盖/沿/桶身/三条纹），
  灰渐变底复用「设置」图标的配色族；`SetToolTipText("废纸篓")`
- 点击：`LaunchApp("mkdir -p \"$HOME/.Trash\"; exec " POLLUX_BIN "/polluxdesk_files \"$HOME/.Trash\"")`
- 顺手把 `BuildDock` 的硬编码尺寸改为从设置推导（`iconPx` 44/56/64 → 条高/圆角按比例），
  为阶段 4 铺路；**半径统一走一个 `SetRadius()` 助手**

### 阶段 3b — 反向通道：合成器 → 桌面壳（运行指示点与最小化收纳的前提）

**这是新增的基础设施** —— 现有只有单向的标题字符串（客户端→合成器），而"哪些程序在运行、
哪些窗口最小化了"必须由合成器告诉桌面壳。

**不用信号，不用新协议**：合成器把窗口状态写进 `~/.config/polluxdesk/state.conf`，
桌面壳用**已有的 1 秒定时器**轮询解析（与阶段 4 的设置轮询同一机制，零新增通道）。

合成器侧（`polluxdesk_compositor.c`）：在窗口 map/unmap/destroy、标题变更、最小化/还原、
聚焦切换处调用一个新的 `write_window_state(server)`，写：

```
gen=N                      # 每写一次自增，供上层判断新鲜度
desktop_pid=1234           # 桌面壳自身，用于识别哪个 app_id 是桌面壳
win=<id>|<app_id>|<title>|<minimized>|<focused>|<w>|<h>
win=...                    # 每窗口一行
```

`app_id` 用 `wlr_xdg_toplevel->app_id`（Wayland 的标准应用标识）。**排除桌面壳自身**
（`toplevel_is_desktop_shell()` 已能识别）与启动台，避免给它们画指示点。

**桌面壳侧**：定时器里 `stat()` 比较 mtime（≤1 秒延迟，与 macOS 的观感一致），解析出窗口列表。

**陷阱**：
- 合成器**重启**后旧 state 文件里的 pid 会失效 → 桌面壳在每次桌面壳启动时**清空** `state.conf`；
  并在 `gen` 长期不更新时按"全灭"处理
- **绝不按窗口标题去匹配 dock 应用** —— 应匹配 `app_id` 或 `Exec` 的可执行名；标题会随文档名变
- 写文件要**原子**（临时文件 + `rename`），否则桌面壳可能读到半行

### 阶段 3c — 运行中程序指示点

在每个 Dock 图标**下方**加一个小圆点（`ui::Control` 3×3 圆角，半透明深色/浅色随主题）。
数据来自 3b 的窗口列表：某应用的 `app_id`/可执行名匹配到任意窗口 → 亮点，否则不画。

**注意**：桌面壳**不 fork 记录 pid**（`SIGCHLD` 被忽略），所以必须走 3b 的合成器通道，
不能靠"我自己启动过什么"来判断 —— 否则用终端命令起应用就漏了。

### 阶段 3d — 最小化窗口收纳区

Dock **分隔线右侧、废纸篓左侧**放最小化窗口的缩略图格子。

- **从哪拿缩略图**：合成器在 `minimize_toplevel()`（`:837`）最小化的瞬间，把该窗口的
  场景节点渲染成一个小的 `wlr_scene_buffer` 并**永久持有一个引用**（此后窗口不再重绘，
  引用等价于快照），按 `<id>` 命名放进 `server->minimized` 列表
- **怎么给桌面壳**：缩略图是 GPU 图像，**不能写进文本文件**。两条路：
  - **(A) 复用 `wlr-screencopy`**（`protocol/` 里已 vendored 该协议头）：桌面壳请求捕获
    某个缩略图 → 拿到 shm 缓冲 → 转成 dui 位图。改动最小，是推荐路径
  - **(B) 自定义协议**：更干净但要在 dui 的 Wayland 后端加客户端支持，
    而 `NativeWindow_Wayland.cpp` 是**受跟踪文件，不可改** → 排除
- **交互**：点击缩略图 → 桌面壳通过既有的标题通道发命令（这是它唯一能用的方向），
  合成器识别 `"PolluxOS Desktop (restore:<id>)"` 后还原并聚焦该窗口。命令**先清号**
  （`"PolluxOS Desktop"`）再执行，避免重复触发 —— 这是对既有 hack 的审慎扩展，
  注释里要写明它的性质

**风险与回退**：如果缩略图导出在 `wlr_scene_buffer` 上遇到阻滞（例如捕获到的是空缓冲），
**先退到"只显示窗口标题文本的迷你块"**（无需 GPU 路径），保证功能可见 —— 缩略图作为增强再迭代。
`polluxdesk-compositor-desktop` 脚本里已有合成器重启逻辑，state 文件过期可自愈。

### 阶段 4 — Dock 尺寸/位置 + 外观主题（~2 小时）
- **调色板改为可变**：`static const` 颜色块 → `struct Palette g_pal` + `ApplyAppearance(dark, accent)`，
  读点全改读 `g_pal`。浅/深两套值（深色 `#CC1C1C1E` 系）；强调色 8 档（macOS 26 蓝紫粉红橙黄绿石墨）
- **重建机制**：定时器里 `PolluxSettings` 轮询 mtime → 变化则置 `m_settingsDirty` →
  **下一拍**执行 `RebuildUi()`（`HideMenuPanel` → `ApplyAppearance` → `BuildUi` → `InvalidateAll`）。
  守卫 `m_handlersAttached`；`StartClock()` **只**在 `OnInitWindow` 调，避免叠加定时器
- **位置 bottom/left/right**：`BuildUi` 改为 `root(VBox) = 菜单栏 + body(HBox){ [左Dock列] 桌面  [右Dock列] }`；
  `pDock` 类型按方向选 `VBox`/`HBox`（都派生自 `Box`，`AddItem`/`SetAttribute` 通用）
- **诚实的限制**：桌面壳是**最底层**场景节点，任何应用窗覆盖 Dock 区域就会盖住它并吞掉点击。
  正解是 layer-shell 的 reserved strut（客户端半在 dui 的 Wayland 后端里，**不可改**）→ 本次不做，写进文档
- **自动隐藏：不做**。触发点在应用窗之下，实现必然出"藏起来再也回不来"的死状态。若日后要，
  最诚实的近似是：合成器在最大化时写状态文件，shell 读到就隐藏 —— 但那是"全屏时让路"，不是靠近感应

### 阶段 5 — 合成器两处小改（~1 小时）
- **5a 改分辨率后重设桌面壳尺寸**（修上面那个现存 bug）：在 `reload_user_settings()` 末尾与
  `output_request_state` 里重新 `wlr_xdg_toplevel_set_size` + 定位
- **5b 导出可用模式**：合成器已有 `wlr_output->modes`，写 `~/.config/polluxdesk/outputs.conf`
  （`current=` + 逐行 `mode=`），供设置面板枚举（系统上无 `wlr-randr`/`wayland-info`，客户端无从枚举）

### 阶段 6 — Settings 重做（~1 天）
侧边栏改 macOS 26 布局：`ui::ButtonHBox` 行（高 30、圆角 7）+ 20×20 圆角 5 的**彩色图标块**
（有内置 SVG 的用 SVG，其余用**单个中文字**当图形 —— 本机无 SF Symbols 字体，不追字形parity），
分两组（外观/桌面与程序坞/显示器 ｜ 声音/网络/关于）；窗口 860×600 → **780×560**；
面板外套 `VScrollBox`；选中态用强调色填充 + 白字。

六个面板（**全部读当前值回填**，不再每次从默认重画）：
1. **外观** — `ui::Option`×3（浅/深/自动）+ 8 个强调色块 + 壁纸预设
2. **桌面与程序坞** — 图标大小 `Option`×3、位置 `Option`×3
3. **显示器** — 读 `outputs.conf`，模式列表用 `Option` 行（macOS 也是列表）；应用 = 写配置 + `pkill -USR1`
4. **声音** — `ui::Slider` + `mixer`（读 `vol.volume`、写 `mixer vol=NN%`）+ 静音 `CheckBox`；
   设备名取 `mixer` 首行；面板可见时 2 秒轮询同步
5. **网络** — `ifconfig -l` 枚举 → 每接口解析 flags/inet/ether/status/ssid，卡片式展示 + 刷新；
   另加 `ifconfig wlan0 list scan` 真实 SSID 列表（免 root 可扫）；**删掉假的 Wi-Fi 开关**（`up/down` 需 root，做了就是撒谎）
6. **关于本机** — `sysctl` 真型号/核数/内存/版本 + `df -h /` 存储

⚠️ **`ui::Combo` 有风险**：它会弹独立子窗口，而本后端"每进程单窗口"的输入路由才可靠
（代码注释里有前车之鉴）。**一律用 `Option` 组代替 `Combo`**。
⚠️ 面板内控件**不得**直接调 `ShowSection()`（控件会在派发中被删）→ 用待处理索引 + 定时器。

### 阶段 7 — 动效与玻璃质感（贯穿，~1 小时）
- **玻璃（多层模拟，非真模糊）**：现有单层 `#E6FFFFFF` 之上加 —— 顶部 1px 高光带、
  上下微渐变、1px 内描边、外投影。比现在明显更像玻璃，成本低
- **动效**：下拉菜单与面板用 `SetFadeAlpha`/`SetFadeSize` 淡入淡出；菜单项悬停 `SetFadeInOutY`；
  Dock 图标悬停微放大（需自写逐帧步进，`AnimationPlayer` 只支持尺寸/透明度）→ **可选**
- **不做**：真折射玻璃、真 squircle 连续圆角（须改 dui 内部）

## 附带发现（记录，本次不处理）

`shell/LaunchPadForm.cpp` 与 `apps/LaunchPadForm.cpp` 是**已分叉的两份拷贝**
（高度 320 vs 620、应用列表不同、`m_launchHandler` 只有一份有），且**两份都参与编译**。
本次只在阶段 0 同时补两处路径；下次任一侧改动都会静默漏掉另一侧 —— 建议另立清理项。

## 验证方式

- 构建：`cd build/polluxdesk && PATH=../tools:$PATH cmake --build . -j16`（约 37 秒）
  （阶段 0 改了 `include_directories`，需先跑一次 `cmake` 重新生成）
- 重启会话：`polluxdesk-compositor-greeter` → 启动 `polluxdesk`
- **截屏逐项核对**：`ssh polluxos 'XDG_RUNTIME_DIR=/var/run/xdg/shxu WAYLAND_DISPLAY=wayland-0 grim /tmp/s.png'`
  → `scp` 到本地 `tmp/` → Read 看图确认 Dock 右侧、分隔线、废纸篓、主题、设置面板
- **远程触发点击**（无鼠标时验证交互）：
  `ssh polluxos 'XDG_RUNTIME_DIR=/var/run/xdg/shxu WAYLAND_DISPLAY=wayland-0 setsid <bin> <args> &'`
- 功能核对：阶段 0 后点 Dock 各图标应全部可开；改设置后 1 秒内桌面应有反应；
  切分辨率后 Dock 应贴住新底边（验阶段 5a）
