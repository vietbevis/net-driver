savedcmd_/root/net-driver/rtl8188_mon.mod := printf '%s\n'   rtl8188_mon.o | awk '!x[$$0]++ { print("/root/net-driver/"$$0) }' > /root/net-driver/rtl8188_mon.mod
