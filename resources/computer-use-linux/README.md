# LobsterAI Computer Use — Linux helper

The native side of Computer Use on Linux. Speaks JSON-RPC 2.0 over stdin/stdout,
one JSON object per line, and is driven by `client/index.mjs` from the Electron
main process.

## What it does

Reads the AT-SPI2 accessibility tree, captures window pixels, injects input, and
launches applications. It has to do each of those differently depending on two
independent facts about the machine it is running on, and getting that matrix
right is most of the work:

| | X11 session | Wayland session |
|---|---|---|
| **Accessibility** | AT-SPI2 over D-Bus | identical — AT-SPI2 does not care |
| **Capture** | `XGetImage` on the window | portal `Screenshot`, or a desktop-specific API |
| **Input** | `XTest` (no permission needed) | portal `RemoteDesktop` (consent dialog) |
| **Raise window** | EWMH `_NET_ACTIVE_WINDOW` | not possible for a client; AT-SPI `grab_focus` at best |
| **Close window** | EWMH `_NET_CLOSE_WINDOW` | AT-SPI `close` action only |

Within each column the desktop still matters. `describe_environment` reports the
result of that whole matrix so callers can plan instead of discovering it by
failing.

### Per-desktop specifics

- **KDE Plasma** — portal backend `kde`. Capture goes through the screenshot
  portal; after granting it once with "remember" ticked, it is silent. Owns
  `org.kde.KWin` but exposes no public capture or input API to third parties.
- **GNOME** — portal backend `gnome`, plus two private interfaces used directly
  here: `org.gnome.Shell.Screenshot` (silent capture on both session types,
  cropped in the compositor) and `org.gnome.Shell.Eval` (shell JavaScript, X11
  sessions only since GNOME 41). GNOME also ships
  `toolkit-accessibility=false` by default, which hides every GTK app from the
  accessibility tree — the most common cause of "nothing works on GNOME".
- **XFCE** — no native Wayland session, so the X11 column applies in full.
  Capture has to go through `xfce4-screenshooter`, because
  xdg-desktop-portal has no XFCE backend and `Screenshot` may be unimplemented
  in the GTK fallback.
- **wlroots compositors** (sway, Hyprland, river, Wayfire) — expose
  `zwlr_screencopy_manager_v1`, `zwlr_virtual_pointer_v1` and
  `zwp_virtual_keyboard_v1` instead of a portal. Those need a Wayland client
  connection, which this process does not have, so they are detected and
  reported rather than used.

## Methods

`list_windows` `list_apps` `get_window` `get_window_state` `activate_window`
`close` `launch_app` `resolve_app` `click` `press_key` `type_text` `scroll`
`drag` `set_value` `perform_secondary_action` `check_permissions`
`request_permissions` `request_input_access` `describe_environment`
`set_accessibility` `shutdown`

`describe_environment` and `request_input_access` are Linux-only additions:
the first reports the capability matrix above, the second opens the Wayland
consent dialog as an explicit step so the wait does not look like a hang. Both,
plus `set_accessibility`, are registered as MCP tools only when the client
provides them, so the Windows and macOS surface is unchanged.

### Accessibility reports two separate facts

`describe_environment` reports `accessibility.bus` and `accessibility.attached`
separately, and the distinction matters:

- `bus` — at-spi2-core answers `org.a11y.Bus.GetAddress` on the session bus.
- `attached` — *this process* got a working accessibility connection
  (`atspi_init` succeeded).

They can disagree: a helper launched from a shell that lacks
`DBUS_SESSION_BUS_ADDRESS` reports `bus: true, attached: false`, and in that
state `list_windows` returns an empty list. Only `attached` predicts what the
next tool call will do.

### Window handles and the X server

`activate_window` and `close` accept an AT-SPI window id, which the X server does
not know. When an `x11_window` is not supplied the helper resolves one itself,
matching `_NET_CLIENT_LIST` (and, when that is incomplete, a depth-limited walk
of the window tree) by pid first, then by `WM_CLASS` against the desktop entry's
`StartupWMClass` / app id / exec basename. The title only ranks candidates, never
qualifies one — closing the wrong window is worse than falling back to AT-SPI.

Class matches on unmapped windows are rejected. An app like awesun keeps several
never-managed helper windows with the right `WM_CLASS`; raising one of those
succeeds at the protocol level and does visibly nothing, which is the worst
possible outcome because the caller believes it worked.

## Building

```sh
# Arch
sudo pacman -S at-spi2-core json-glib cairo glib2 libx11 libxtst

# Debian/Ubuntu
sudo apt install libatspi2.0-dev libjson-glib-dev libcairo2-dev libglib2.0-dev \
                 libx11-dev libxtst-dev

make
./lobsterai-computer-use-linux --self-test
```

X11 support is compiled in whenever the headers are present and detected at
runtime, so one binary covers both session types. Without the X11 headers the
helper still builds and works on Wayland, reporting capture and injection as
unavailable on X11.

## Runtime requirements

- `at-spi2-core` running, and applications opted in. On GNOME that means
  `gsettings set org.gnome.desktop.interface toolkit-accessibility true` plus an
  app restart; elsewhere it is usually on by default.
- `xdg-desktop-portal` plus a desktop backend (`-kde`, `-gnome`, `-gtk`) for
  Wayland capture and input.

## Debugging

Diagnostics go to stderr; stdout carries protocol only.

```sh
# Full environment report against the live session
echo '{"jsonrpc":"2.0","id":1,"method":"describe_environment"}' \
  | ./lobsterai-computer-use-linux

# Shorten the consent-dialog wait when testing headlessly
LOBSTER_CU_RD_TIMEOUT=6 ./lobsterai-computer-use-linux
```

`G_MESSAGES_DEBUG=all` turns on the detailed backend logging.
