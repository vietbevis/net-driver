KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build
PWD  := $(shell pwd)

MODULE := rtl8188_drv

obj-m := $(MODULE).o
$(MODULE)-objs := rtl8188_main.o rtl8188_hw.o rtl8188_phy.o \
                  rtl8188_mac.o rtl8188_trx.o

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

load: all
	@echo "[*] Unloading system drivers..."
	-rmmod rtl8xxxu 2>/dev/null
	-rmmod r8188eu 2>/dev/null
	@echo "[*] Loading dependencies..."
	-modprobe mac80211 2>/dev/null
	-modprobe cfg80211 2>/dev/null
	@echo "[*] Loading $(MODULE).ko..."
	insmod $(MODULE).ko
	@echo "[+] Driver loaded."

unload:
	@echo "[*] Unloading $(MODULE)..."
	-rmmod $(MODULE) 2>/dev/null
	@echo "[+] Driver unloaded."

reload: unload load

status:
	@echo "=== Module status ==="
	@lsmod | grep -E "rtl8188|rtl8xxxu|cfg80211|mac80211" || true
	@echo ""
	@echo "=== USB device ==="
	@lsusb | grep 0bda || true
	@echo ""
	@echo "=== Network interface ==="
	@ip link show 2>/dev/null | grep -A1 -E "wlan|wlp|wlx" || true
	@echo ""
	@echo "=== Recent dmesg ==="
	@dmesg 2>/dev/null | grep -i "RTL8188" | tail -20 || true

.PHONY: all clean load unload reload status
