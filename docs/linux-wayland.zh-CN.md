# Linux 与 Wayland 适配说明

本文记录两件事的取舍依据：为什么在 Wayland 会话上让 Electron 跑 XWayland，以及悬浮选词在各桌面能做什么、不能做什么。文中数据均为 KDE Plasma 6 / KWin 6.8 / Electron 44 实测。

## 一、为什么在 Wayland 会话上强制 XWayland

桌面伴侣的每个表面——选词工具条、答案卡片、舞台窗口、权限引导——都用 `BrowserWindow.setBounds()` 贴着 `display.workArea` 定位。在原生 Wayland 会话下这两样都不可靠：

| 指标 | Wayland 原生 | 顶层 `appendSwitch` | `--ozone-platform=x11` |
|---|---|---|---|
| `setBounds` | 失效，窗口停在 (0,0) | 生效 | 生效 |
| `primary` 显示器 | 报 1680×1050 @ (0,0)，错 | 仍报错 | 1920×1080 @ (1680,195)，对 |
| `workArea` | 等于 `bounds`，未扣面板 | 仍未扣 | 正确扣掉 35px 面板 |

第二列的坑值得单独说：在 JS 顶层调 `app.commandLine.appendSwitch('ozone-platform', 'x11')` **看起来能用**——窗口确实进了 XWayland，`setBounds` 也不报错了——但 `screen` 模块仍然返回 Wayland 的几何。用错的坐标系去摆放，比彻底失效更难查。所以不能用这条路。

Wayland 协议不允许客户端设置自己的全局坐标，这是设计如此，不是 Electron 的 bug。唯一可靠的开关是进程命令行，而进程一旦起来就改不了，只能带上参数重启一次。

实现见 `src/main/linuxOzone.ts`：检测到 Wayland 会话且命令行没有指定平台时，用 `app.relaunch()` 带 `--ozone-platform=x11` 重启，并在子进程里设 `LOBSTERAI_OZONE_RELAUNCHED=1` 防止无限重启。这一段必须在 `app.whenReady()` 之前、任何锁和文件写入之前执行，被丢弃的那个进程才不留痕迹。

想保留原生 Wayland（并接受伴侣窗口无法定位）：

```
LOBSTERAI_OZONE_PLATFORM=native lobsterai
```

## 二、悬浮选词的能力矩阵

选词文本来自 PRIMARY selection：X11 下经 XFixes，Wayland 下经 `wlr-data-control` / `ext-data-control`。文字本身在所有支持的情况下都拿得到，差异在周边信息。

| | X11 会话 | KDE Wayland | GNOME Wayland | XFCE |
|---|---|---|---|---|
| 选词文本 | 可用 | 可用 | 不可用¹ | 可用 |
| 光标坐标 | 可用 | 可用（KWin IPC） | 仅 XWayland 窗口 | 可用 |
| 应用名 | 可用（`WM_CLASS`） | 需兜底² | 需兜底² | 可用 |
| 指针/键盘事件 | 可用 | 需 `/dev/input` 权限³ | 需 `/dev/input` 权限³ | 可用 |
| 工具条定位 | 可用 | 需 XWayland⁴ | 需 XWayland⁴ | 可用 |

¹ GNOME 的 Mutter 不实现 data-control 协议，第三方无法读取其它客户端的选区。
² Wayland 下 selection-hook 的 `programName` 恒为空（`wayland.cc` 里直接 `return false`），由 `src/main/desktopCompanion/linuxActiveApp.ts` 反查前台应用补上：GNOME 走 `org.gnome.Shell.Introspect`，Sway/Hyprland 走各自命令，其余走 X11 的 `_NET_ACTIVE_WINDOW` + `WM_CLASS`。KWin 未提供非交互读取活动窗口的接口，所以 KWin 下原生 Wayland 窗口取不到名字——这条路径只覆盖 XWayland 客户端。
³ 没有 `/dev/input` 读权限时，selection-hook 退化为「data-control 防抖模式」，只发选区事件，不发鼠标键盘事件。此时工具条靠指针巡检关闭（光标离开选区和工具条约 220px 后隐藏），也靠前台应用切换关闭。
⁴ 见第一节。

### 缺少指针事件时会发生什么

这是 Wayland 上最容易漏的一点。正常情况下，点击别处关闭工具条靠 `mouse-down`，按 Esc 靠 `key-down`。没有 `/dev/input` 权限时这两个事件都不会来，工具条会一直挂着直到下一次选词覆盖它。

两条替补路径：

- `CompanionSelectionController.startPointerWatch()` —— 每 300ms 看一次光标，连续 4 次都在选区和工具条之外才关闭。只在钩子确实拿不到输入事件时启用。
- `onForegroundChange()` —— 前台应用切换时关闭工具条。这条本来就该有，Linux 之前因为前台监视器恒为 `Unsupported` 而失效，现在接上了。

想要完整的输入事件，把自己加进 `input` 组：

```bash
sudo usermod -aG input $USER
# 重新登录后生效
```

## 三、应用名兜底的语义

`programName` 为空时用「当前前台应用」代替「选区所属应用」。两者在选词场景下通常是同一个——选文字不会改变焦点——但严格说这是近似。

拿到的名字和 selection-hook 在 X11 下的取值语义一致：都是 `WM_CLASS` 的 `res_name`（`XClassHint.res_name`），也就是小写的可执行名。所以排除列表两边通用。

`src/shared/desktopCompanion/appCategories.ts` 里的 `LINUX_APPS` 表按这个约定登记了常见应用，默认排除表里加了文件管理器（`nautilus` / `thunar` / `dolphin` 等）——在文件管理器里选文件名不是在读文本，和 Finder、Explorer 同理。

## 四、相关文件

| 文件 | 职责 |
|---|---|
| `src/main/linuxOzone.ts` | Wayland 会话下重启到 XWayland |
| `src/main/desktopCompanion/linuxActiveApp.ts` | 按桌面反查前台应用 |
| `src/main/desktopCompanion/companionSelection.ts` | 选词控制器，含指针巡检与光标坐标 DIP 转换 |
| `src/main/desktopCompanion/foregroundAppMonitor.ts` | 前台应用轮询（Windows/macOS/Linux） |
| `src/shared/desktopCompanion/appCategories.ts` | 应用分类、默认排除列表 |
