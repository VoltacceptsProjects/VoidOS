# VoidOS desktop -- build
#
# Dependencies (Debian/Ubuntu):
#   sudo apt install build-essential pkg-config libx11-dev libxext-dev \
#                     libcairo2-dev libpango1.0-dev
# Dependencies (Fedora):
#   sudo dnf install gcc make pkgconf-pkg-config libX11-devel libXext-devel \
#                     cairo-devel pango-devel
# Dependencies (Arch):
#   sudo pacman -S base-devel pkgconf libx11 libxext cairo pango

PREFIX      ?= /usr/local
BINDIR       = $(PREFIX)/bin
DATADIR      = $(PREFIX)/share/voidos
XSESSIONDIR  = /usr/share/xsessions

CC      ?= cc
PKGS     = x11 xext cairo cairo-xlib pango pangocairo
CFLAGS  ?= -O2 -Wall -Wextra -std=c11 -D_DEFAULT_SOURCE
CFLAGS  += $(shell pkg-config --cflags $(PKGS))
LDLIBS   = $(shell pkg-config --libs $(PKGS)) -lm

SRC = src/voidwm.c src/draw.c src/bar.c src/dock.c
OBJ = $(SRC:.c=.o)
BIN = voidwm

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c src/voidwm.h src/config.h src/draw.h
	$(CC) $(CFLAGS) -c $< -o $@

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
	install -Dm755 voidwm-session $(DESTDIR)$(BINDIR)/voidwm-session
	install -Dm644 voidwm.desktop $(DESTDIR)$(XSESSIONDIR)/voidwm.desktop
	install -d $(DESTDIR)$(DATADIR)/wallpapers
	install -m644 wallpapers/*.jpg $(DESTDIR)$(DATADIR)/wallpapers/

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN) $(DESTDIR)$(BINDIR)/voidwm-session
	rm -f $(DESTDIR)$(XSESSIONDIR)/voidwm.desktop
	rm -rf $(DESTDIR)$(DATADIR)

clean:
	rm -f $(BIN) $(OBJ)
	$(MAKE) -C apps/voiddocs clean

# ---------------------------------------------------------------- #
# VoidDocs -- the dock's "VoidDocs" entry (.vdoc markdown editor).
# A separate GTK3 app, not part of voidwm itself; needs libgtk-3-dev.
# Not part of `all`/`install` above so a plain `make && sudo make
# install` never requires GTK just to get the window manager.
# ---------------------------------------------------------------- #
voiddocs:
	$(MAKE) -C apps/voiddocs

install-voiddocs: voiddocs
	$(MAKE) -C apps/voiddocs install PREFIX=$(PREFIX)

uninstall-voiddocs:
	$(MAKE) -C apps/voiddocs uninstall PREFIX=$(PREFIX)

.PHONY: all install uninstall clean voiddocs install-voiddocs uninstall-voiddocs
