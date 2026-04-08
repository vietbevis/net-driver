KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build
PWD  := $(shell pwd)

MODULE := rtl8188_mon
CLI    := rtl8188_cli

obj-m := $(MODULE).o

.PHONY: all module cli clean load unload reload status help

all: module cli

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

cli: $(CLI).c
	gcc -Wall -O2 -o $(CLI) $(CLI).c -lncurses

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
