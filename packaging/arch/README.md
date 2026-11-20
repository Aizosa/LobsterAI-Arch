# Arch Linux 打包

两个 PKGBUILD，按手上有什么选：

| 文件 | 装出什么 | 耗时 | 前提 |
|---|---|---|---|
| `PKGBUILD.bin` | `lobsterai-bin` | 几秒 | 先有一个 AppImage |
| `PKGBUILD` | `lobsterai` | 很久，且要联网拉几百 MB | 无 |

日常用 `PKGBUILD.bin`。源码版只在需要改代码重打包时用。

## 关于 .BUILDINFO

`.BUILDINFO` 不用手写，也不是能手写的东西——它是 `makepkg` 打完包后自动生成的元数据，描述的是**构建这个包时的环境**，不是包的配置。下面是从实际产出的包里取出来的（`bsdtar -xOf *.pkg.tar.zst .BUILDINFO`）：

```
format = 2
pkgname = lobsterai-bin
pkgver = 2026.11.16-1
pkgarch = x86_64
pkgbuild_sha256sum = 57a24a6b…     ← 用的哪份 PKGBUILD
packager = Unknown Packager
builddate = 1795134225
builddir = /tmp/pkgtest
buildtool = makepkg
buildtoolver = 7.1.0
buildenv = !distcc
buildenv = color
options = strip
options = debug
installed = 7zip-26.06-1-x86_64    ← 构建时机器上每个包的精确版本
installed = a52dec-0.8.0-3-x86_64
…
```

其中的 `installed` 列表是它主要的用途：日后出了问题，能查到当时的 glibc、gcc 具体是哪一版，用来复现构建。`makepkg` 会自己维护它，你唯一能影响的是 `options` 和 `buildenv`（通过 PKGBUILD 的 `options=()` 和 `/etc/makepkg.conf`）。

想手写的是 **PKGBUILD**，也就是本目录下的两个文件。

## 用法一：lobsterai-bin（推荐）

先在能构建的机器上产出 AppImage：

```bash
cd /path/to/LobsterAI
npm run dist        # 产出 dist/LobsterAI-*-x64.AppImage
```

把 AppImage 放到本目录，改文件名对上前面的 `pkgver`，然后：

```bash
cd packaging/arch
cp LobsterAI-2026.11.16.AppImage .
makepkg -g >> PKGBUILD.bin          # 把 SKIP 换成真实 sha256
mv PKGBUILD.bin PKGBUILD
makepkg -si
```

`-si` 会装依赖、打包、然后 `pacman -U` 装上。卸载走 `pacman -R lobsterai-bin`，和普通包一样。

装上后：菜单里有 LobsterAI 条目，`lobsterai` 命令可用，文件在 `/opt/lobsterai-bin/`。

等将来有 release 了，把 `source` 换成 URL 就不用本地文件了（PKGBUILD.bin 顶部有现成的注释行）。

## 用法二：从源码构建

```bash
cd packaging/arch
cp PKGBUILD.bin PKGBUILD.bin.bak   # 别覆盖
makepkg -si
```

几点需要知道：

**`vendor/openclaw-runtime/` 不在 git 里**（`.gitignore` 第 16 行），必须由 `npm run openclaw:runtime:linux-x64` 现编。这一步是整个构建里最重的，要拉 OpenClaw 源码、打补丁、打包插件、预编译。`PKGBUILD` 的 `build()` 里已经按顺序排好了。

**`build()` 里联网**。`npm ci` 和 runtime 组装都要下载。这违反 makepkg 的惯例（AUR 里构建 Node 应用的包都这样），所以需要可复现时请把 `_commit` 从 `main` 换成具体的 commit。

**Electron 二进制走镜像**。`npm ci` 会从 GitHub 下 Electron，国内经常失败，所以设了 `ELECTRON_MIRROR` 指向 npmmirror。想换源改环境变量即可。

**没有产出 AppImage**。源码版用 `electron-builder --dir`，直接得到 `dist/linux-unpacked/` 这个安装树。既然 pacman 已经负责安装，AppImage 那层自解压包装是多余的。

## 两个版本共同的说明

**没有 `url` 也没有 `license`**，是刻意的。上游源码是 `netease-youdao/LobsterAI`，MIT 协议——但那授权的是源码，不是打出来的包。这个包是自用构建，不分发，也不对任何人授予授权；写上 `license=('MIT')` 会被读成「你对这些二进制也授予了 MIT」。`pacman -Qi` 里这两项显示为空是预期结果。

`source=` 里仍然指向 fork（含 Linux 适配的那份代码），那是构建时取代码用的，和包元数据无关。

**`options=('!strip' '!debug')`**。`!strip` 是必须的：Electron 的二进制和 V8 snapshot 被 strip 后会失效。`!debug` 是因为 Arch 默认会额外生成一个 `-debug` 包，对 Electron 应用就是几百 MB 且没人用的东西。

**两个 PKGBUILD 都覆盖了 `dirsize()`**，这是给 BSD 用户工具链打的补丁。makepkg 算安装体积时是 `…… | wc -c` 直接写进 `.PKGINFO`，而 BSD 的 `wc` 会把数字右对齐到固定列宽：

```
$ printf "hello\n" | wc -c | od -c
0000000                                6  \n
```

于是 `.PKGINFO` 变成 `size =        6`，pacman 不 trim，解析失败，`pacman -Qip` 显示 `Installed Size: 0.00 MiB`。GNU 的 `wc` 不填充，所以这个覆盖在正常 Arch 上是个 no-op。判断自己的机器有没有这个毛病：

```bash
printf "hello\n" | wc -c | od -c | head -1
```

`6` 前面没有多余空格就没事。

## pptx 的运行时依赖

`SKILLs/pptx` 装依赖时要 `sharp`（走 `@img/sharp-linux-x64` 预编译包，不需要编译）和 `playwright`。打包钩子只跑 `npm install`，成功即算通过。

`scripts/html2pptx.js` 用的是 `launchBrowser()`：**所有平台都先起系统 Chrome**（`channel: 'chrome'`，Windows/Linux/macOS 一致），找不到 Chrome 才回退到 playwright 自带的 Chromium。这样包里不用塞 playwright 的浏览器（约 300MB），只要机器上有 Chrome 就能用「HTML 转 PPTX」。

回退路径需要 playwright 自己的浏览器，没装就到这里为止：

```bash
cd SKILLs/pptx && npx playwright install chromium
```

纯 `pptxgenjs` 生成 PPTX 不经过浏览器，无所谓装没装 Chrome。

实测（Chrome 157 已装）：`channel: 'chrome'` 841ms 启动成功，页面渲染正常；回退路径报 `Executable doesn't exist` 符合预期。

**依赖列表**是 Electron 在 Arch 上的常规组合，外加 `libsecret`（`safeStorage` 存凭据要用）、`xdg-utils`（打开外部链接）、`libxkbcommon` 和 `wayland`（Wayland 会话）。`xdg-desktop-portal` 系放在 `optdepends`，只有 Wayland 下共享屏幕和远程输入才需要。

**没有 setuid sandbox**。项目在 `main.ts` 里对 Linux 已经 `appendSwitch('no-sandbox')`，所以不需要给 `chrome-sandbox` 设 4755，也不需要额外处理 user namespace。

**首次启动会重启一次**。Wayland 会话下应用会带 `--ozone-platform=x11` 重启自己，这是为了让桌面伴侣的窗口能够定位，详见 `docs/linux-wayland.zh-CN.md`。想留在原生 Wayland：

```bash
LOBSTERAI_OZONE_PLATFORM=native lobsterai
```

## 已验证到什么程度

`PKGBUILD.bin` 的完整流程跑通过一次（用一个响应 `--appimage-extract` 的假 AppImage 代替真实载荷）：`makepkg` 成功产出 `lobsterai-bin-2026.11.16-1-x86_64.pkg.tar.zst`，包内布局、`.desktop`、图标路径、`usr/bin` 符号链接都正确。

`PKGBUILD` 的 `build()` 没有实跑——本机工作副本缺 `vendor/openclaw-runtime/` 和 Electron 二进制，补齐要下载几百 MB。它的 `package()` 结构与 `.bin` 版一致，`makepkg --printsrcinfo` 通过。

所有 `depends` 包名都用 `pacman -Si` 核对过存在。
