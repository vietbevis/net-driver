#!/bin/bash
#
# manage_driver.sh - Disable/enable system drivers for RTL8188ETV
#

BLACKLIST_FILE="/etc/modprobe.d/rtl_blacklist.conf"

case "$1" in
    disable)
        echo "[*] Unloading system Realtek USB WiFi drivers..."
        rmmod rtl8xxxu 2>/dev/null
        rmmod r8188eu 2>/dev/null

        echo "[*] Blacklisting system drivers..."
        cat > "$BLACKLIST_FILE" <<EOF
blacklist rtl8xxxu
blacklist r8188eu
EOF
        echo "[+] System drivers disabled. Device is free for custom driver."
        ;;

    enable)
        echo "[*] Removing blacklist..."
        rm -f "$BLACKLIST_FILE"

        echo "[*] Loading system driver..."
        modprobe rtl8xxxu 2>/dev/null
        echo "[+] System drivers re-enabled."
        ;;

    *)
        echo "Usage: $0 {disable|enable}"
        echo "  disable - Unload and blacklist rtl8xxxu/r8188eu"
        echo "  enable  - Remove blacklist and reload rtl8xxxu"
        exit 1
        ;;
esac
