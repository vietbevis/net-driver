KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build
PWD  := $(shell pwd)

MODULE := rtl8188_mon
CLI    := rtl8188_cli

# -----------------------------------------------------------------------------
# Kernel module (Kbuild section)
# -----------------------------------------------------------------------------
obj-m += $(MODULE).o

$(MODULE)-objs := \
	rtl8188_main.o \
	rtl8188_usb.o \
	rtl8188_netdev.o \
	rtl8188_pkt.o \
	rtl8188_cmd.o \
	rtl8188_proc.o \
	rtl8188_chrdev.o

# -----------------------------------------------------------------------------
# Userspace TUI/CLI build
# -----------------------------------------------------------------------------
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra
PKG_CONFIG ?= pkg-config

CLI_SRCS := \
	tui/rtl8188_main_tui.c \
	tui/rtl8188_tui.c \
	tui/rtl8188_tabs.c

CLI_CFLAGS := -I./tui

# Prefer pkg-config if available; fallback for systems without .pc file.
NCURSES_PKG := $(shell $(PKG_CONFIG) --exists ncursesw && echo ncursesw || \
	$(PKG_CONFIG) --exists ncurses && echo ncurses || true)

ifeq ($(NCURSES_PKG),)
NCURSES_CFLAGS :=
NCURSES_LIBS   := -lncursesw
else
NCURSES_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(NCURSES_PKG))
NCURSES_LIBS   := $(shell $(PKG_CONFIG) --libs $(NCURSES_PKG))
endif

.PHONY: all module cli clean load unload reload status help

all: module cli

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

cli: check-ncurses $(CLI)

check-ncurses:
	@printf '#include <ncurses.h>\n' | $(CC) -x c -E $(NCURSES_CFLAGS) - >/dev/null 2>&1 || { \
		echo "Missing ncurses headers (ncurses.h)."; \
		echo "Install dependency: dnf install ncurses-devel  (or apt install libncurses-dev)"; \
		exit 1; \
	}

$(CLI): $(CLI_SRCS) tui/rtl8188_cli.h
	$(CC) $(CFLAGS) $(CLI_CFLAGS) $(NCURSES_CFLAGS) -o $@ $(CLI_SRCS) $(NCURSES_LIBS)

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f $(CLI)

load: all
	@echo "=== Loading RTL8188 Companion Monitor ==="
	@echo "[1] Ensuring system driver (rtl8xxxu) is loaded..."
	-modprobe rtl8xxxu 2>/dev/null
	@echo "[2] Loading companion monitor module..."
	-rmmod $(MODULE) 2>/dev/null
	insmod $(MODULE).ko
	@sleep 1
	@echo "[3] Done! Device: /dev/rtl8188  Proc: /proc/rtl8188/"
	@echo ""
	@echo "Launch TUI:     ./$(CLI)"
	@echo "CLI mode:       ./$(CLI) scan|info|status|stats|capture"
	@echo "Connect WiFi:   ./$(CLI) connect SSID [password]"

unload:
	@echo "=== Unloading RTL8188 Companion Monitor ==="
	-rmmod $(MODULE) 2>/dev/null
	@echo "Done. System driver (rtl8xxxu) is still active."

reload: unload load

status:
	@echo "=== Module Status ==="
	@lsmod | grep -E "rtl8188|rtl8xxxu|cfg80211|mac80211" || echo "(none loaded)"
	@echo ""
	@echo "=== USB Device ==="
	@lsusb | grep 0bda:0179 || echo "(not found)"
	@echo ""
	@echo "=== Char Device ==="
	@ls -la /dev/rtl8188 2>/dev/null || echo "/dev/rtl8188 not found"
	@echo ""
	@echo "=== Proc Entries ==="
	@ls /proc/rtl8188/ 2>/dev/null || echo "/proc/rtl8188/ not found"

help:
	@echo "Makefile targets:"
	@echo "  make          - Build kernel module + TUI dashboard"
	@echo "  make module   - Build kernel module only"
	@echo "  make cli      - Build TUI/CLI tool only"
	@echo "  make load     - Load module (keeps rtl8xxxu running)"
	@echo "  make unload   - Unload module"
	@echo "  make reload   - Unload + load"
	@echo "  make status   - Show current status"
	@echo "  make clean    - Clean build artifacts"
