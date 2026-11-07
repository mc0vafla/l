CC = gcc

CFLAGS = -O3 -Wall $(shell pkg-config --cflags wayland-client wayland-cursor xkbcommon cairo pango pangocairo vterm)
LIBS = $(shell pkg-config --libs wayland-client wayland-cursor xkbcommon cairo pango pangocairo vterm) -lutil

XDG_SHELL_XML = /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml

all: src/xdg-shell-protocol.c src/xdg-shell-client-protocol.h l-term

src/xdg-shell-protocol.c:
	mkdir -p src
	wayland-scanner private-code $(XDG_SHELL_XML) src/xdg-shell-protocol.c

src/xdg-shell-client-protocol.h:
	mkdir -p src
	wayland-scanner client-header $(XDG_SHELL_XML) src/xdg-shell-client-protocol.h

l-term: src/main.c src/xdg-shell-protocol.c
	$(CC) $(CFLAGS) -o $@ src/main.c src/xdg-shell-protocol.c $(LIBS)

clean:
	rm -f l src/xdg-shell-protocol.c src/xdg-shell-client-protocol.h
