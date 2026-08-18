# VoidDocs -- the VoidOS markdown editor (.vdoc)
#
# Separate from the voidwm build: this is a GTK3 app, not part of
# the window manager itself.
#
# Dependencies (Debian/Ubuntu): sudo apt install libgtk-3-dev
# Dependencies (Fedora):        sudo dnf install gtk3-devel
# Dependencies (Arch):          sudo pacman -S gtk3

PREFIX      ?= /usr/local
BINDIR       = $(PREFIX)/bin
APPDIR       = $(PREFIX)/share/applications
MIMEDIR      = $(PREFIX)/share/mime/packages

CC      ?= cc
PKGS     = gtk+-3.0
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
CFLAGS  += $(shell pkg-config --cflags $(PKGS))
LDLIBS   = $(shell pkg-config --libs $(PKGS))

BIN = voiddocs

all: $(BIN)

$(BIN): voiddocs.c
	$(CC) $(CFLAGS) -o $@ voiddocs.c $(LDLIBS)

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
	install -Dm644 voiddocs.desktop $(DESTDIR)$(APPDIR)/voiddocs.desktop
	install -Dm644 voiddocs-mime.xml $(DESTDIR)$(MIMEDIR)/voiddocs.xml
	-update-mime-database $(DESTDIR)$(PREFIX)/share/mime 2>/dev/null
	-update-desktop-database $(DESTDIR)$(APPDIR) 2>/dev/null

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)
	rm -f $(DESTDIR)$(APPDIR)/voiddocs.desktop
	rm -f $(DESTDIR)$(MIMEDIR)/voiddocs.xml

clean:
	rm -f $(BIN)

.PHONY: all install uninstall clean
