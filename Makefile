CC = gcc
CFLAGS = -Wall -O3
LDFLAGS = -lwayland-client -lcairo -lm -lwayland-cursor
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
# The revisions the committed protocol and vendor files were taken from. Each
# fetch names one, so re-running this target reproduces the tree rather than
# whatever upstream happens to be today; bump a revision to take an update.
LAYER_SHELL_REV = d1598e82240d6e8ca57729495a94d4e11d222033
FOREIGN_TOPLEVEL_REV = 005d69d048ccceb2af3f5b86665821e8fa9a87b8
NANOSVG_REV = e6d3dd25415539b24e08bf54ef7f3cd37e60152e
NANOSVGRAST_REV = 2dba6182462275e67a59df251fff17ecdfb93770

fetch-deps:
	mkdir -p protocols vendor
	curl -fsS -o protocols/wlr-layer-shell-unstable-v1.xml https://raw.githubusercontent.com/swaywm/wlr-protocols/$(LAYER_SHELL_REV)/unstable/wlr-layer-shell-unstable-v1.xml
	curl -fsS -o protocols/wlr-foreign-toplevel-management-unstable-v1.xml https://gitlab.freedesktop.org/wlroots/wlr-protocols/-/raw/$(FOREIGN_TOPLEVEL_REV)/unstable/wlr-foreign-toplevel-management-unstable-v1.xml
	curl -fsS -o vendor/nanosvg.h https://raw.githubusercontent.com/memononen/nanosvg/$(NANOSVG_REV)/src/nanosvg.h
	curl -fsS -o vendor/nanosvgrast.h https://raw.githubusercontent.com/memononen/nanosvg/$(NANOSVGRAST_REV)/src/nanosvgrast.h

.PHONY: all clean fetch-deps
