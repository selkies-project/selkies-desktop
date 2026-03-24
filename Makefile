CC = gcc
CFLAGS = -Wall -O3
LDFLAGS = -lwayland-client -lcairo -lm
TARGET = selkies-desktop
SRC = selkies-desktop.c
PROTO_SRC = wlr-layer-shell.c wlr-foreign-toplevel-management-unstable-v1.c xdg-shell-protocol.c
PROTO_HDR = $(PROTO_SRC:.c=.h)
OBJ = $(SRC:.c=.o) $(PROTO_SRC:.c=.o)
all: $(TARGET)
$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $(TARGET) $(LDFLAGS)
%.o: %.c $(PROTO_HDR)
	$(CC) $(CFLAGS) -c $< -o $@
wlr-layer-shell.h:
	wayland-scanner client-header protocols/wlr-layer-shell-unstable-v1.xml $@
wlr-layer-shell.c: wlr-layer-shell.h
	wayland-scanner private-code protocols/wlr-layer-shell-unstable-v1.xml $@
wlr-foreign-toplevel-management-unstable-v1.h:
	wayland-scanner client-header protocols/wlr-foreign-toplevel-management-unstable-v1.xml $@
wlr-foreign-toplevel-management-unstable-v1.c: wlr-foreign-toplevel-management-unstable-v1.h
	wayland-scanner private-code protocols/wlr-foreign-toplevel-management-unstable-v1.xml $@
xdg-shell-protocol.h:
	wayland-scanner client-header /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml $@
xdg-shell-protocol.c: xdg-shell-protocol.h
	wayland-scanner private-code /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml $@
clean:
	rm -f $(TARGET) $(OBJ) $(PROTO_SRC) $(PROTO_HDR)
fetch-deps:
	mkdir -p protocols vendor
	curl -s -o protocols/wlr-layer-shell-unstable-v1.xml https://raw.githubusercontent.com/swaywm/wlr-protocols/master/unstable/wlr-layer-shell-unstable-v1.xml
	curl -s -o protocols/wlr-foreign-toplevel-management-unstable-v1.xml https://gitlab.freedesktop.org/wlroots/wlr-protocols/-/raw/master/unstable/wlr-foreign-toplevel-management-unstable-v1.xml
	curl -s -o vendor/nanosvg.h https://raw.githubusercontent.com/memononen/nanosvg/master/src/nanosvg.h
	curl -s -o vendor/nanosvgrast.h https://raw.githubusercontent.com/memononen/nanosvg/master/src/nanosvgrast.h

.PHONY: all clean fetch-deps
