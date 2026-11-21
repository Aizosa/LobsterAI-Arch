# LobsterAI Computer Use — Linux helper
#
# Build:   make
# Clean:   make clean
#
# Dependencies (Arch): at-spi2-core json-glib cairo libx11 libxtst libei
#   pacman -S at-spi2-core json-glib cairo libx11 libxtst libei
#
# The binary is deliberately built without runtime assumptions about the session
# type: X11 support is compiled in whenever the headers are present, and the
# Wayland path is detected at run time from XDG_SESSION_TYPE.

CC      ?= cc
PKGS     = atspi-2 json-glib-1.0 cairo glib-2.0 gobject-2.0 gio-2.0 gio-unix-2.0
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11
CFLAGS  += $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS))

# X11 is optional: a headless or pure-Wayland build still produces a valid
# helper, it just reports capture/injection as unsupported.
X11_CFLAGS := $(shell pkg-config --cflags x11 xtst 2>/dev/null)
X11_LIBS   := $(shell pkg-config --libs x11 xtst 2>/dev/null)
ifneq ($(strip $(X11_LIBS)),)
  CFLAGS += -DCU_HAVE_X11 $(X11_CFLAGS)
  LDLIBS += $(X11_LIBS)
  $(info [cu-linux] X11 backend enabled)
else
  $(info [cu-linux] X11 headers missing; building Wayland-only)
endif

# libei is optional: it is the EIS virtual-seat client used to synthesize
# pointer/keyboard/text input through the RemoteDesktop portal on Wayland.
# Without it the Wayland input path falls back to the deprecated Notify*
# portal methods, which several backends (KDE included) do not honour.
EIS_CFLAGS := $(shell pkg-config --cflags libei-1.0 2>/dev/null)
EIS_LIBS   := $(shell pkg-config --libs libei-1.0 2>/dev/null)
ifneq ($(strip $(EIS_LIBS)),)
  CFLAGS += -DCU_HAVE_EIS $(EIS_CFLAGS)
  LDLIBS += $(EIS_LIBS)
  $(info [cu-linux] libei EIS backend enabled)
else
  $(info [cu-linux] libei headers missing; Wayland input falls back to Notify*)
endif

# Source layout, by concern:
#   main/json     protocol and serialisation
#   a11y          AT-SPI2 tree (the portable part: same on every desktop)
#   capture       screen capture dispatcher + the backends it picks from
#   input         input injection dispatcher + XTest
#   desktop_env   desktop/session detection and the capability matrix
#   gnome         GNOME Shell private API (Eval, Screenshot, accessibility gate)
#   xfce          XFCE specifics (xfce4-screenshooter, session detection)
#   wayland       portals: RemoteDesktop input, ScreenCast probe, wlroots probes
#   desktop       .desktop discovery and launching
#   window        EWMH activation/closing, window-manager probing
SRC = src/main.c src/json.c src/a11y.c src/capture.c src/input.c \
      src/desktop_env.c src/gnome.c src/xfce.c src/wayland.c \
      src/desktop.c src/window.c
OBJ = $(SRC:.c=.o)
BIN = lobsterai-computer-use-linux

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDLIBS)

%.o: %.c $(wildcard src/*.h)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: all clean
