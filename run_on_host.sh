#!/usr/bin/env bash
# =============================================================================
# RTL8188ETV Custom Driver - Auto Build, Load & Scan
# Hỗ trợ: Ubuntu/Zorin, CentOS/RHEL (x86_64 và aarch64)
# Cách dùng: sudo bash run_on_host.sh
# =============================================================================
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RESULT_FILE="/tmp/rtl8188_test_result.txt"
DRIVER_NAME="rtl8188_drv"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
info()  { echo -e "${GREEN}[+]${NC} $*"; }
warn()  { echo -e "${YELLOW}[!]${NC} $*"; }
error() { echo -e "${RED}[✗]${NC} $*"; }
step()  { echo -e "\n${GREEN}====== $* ======${NC}"; }

# Phải chạy bằng root
if [[ $EUID -ne 0 ]]; then
    error "Cần chạy bằng root: sudo bash $0"
    exit 1
fi

{
# ─── HEADER ──────────────────────────────────────────────────────────────────
echo "============================================================"
echo " RTL8188ETV Driver Test Report"
echo " Thời gian: $(date)"
echo "============================================================"
echo ""

# ─── BƯỚC 1: THÔNG TIN HỆ THỐNG ─────────────────────────────────────────────
step "THÔNG TIN HỆ THỐNG"

KERNEL=$(uname -r)
ARCH=$(uname -m)
DISTRO="Unknown"
[[ -f /etc/os-release ]] && DISTRO=$(grep PRETTY_NAME /etc/os-release | cut -d'"' -f2)

echo "Kernel  : $KERNEL"
echo "Arch    : $ARCH"
echo "Distro  : $DISTRO"
echo ""

# ─── BƯỚC 2: KIỂM TRA USB WIFI ───────────────────────────────────────────────
step "KIỂM TRA USB WIFI"

if ! lsusb | grep -q "0bda:0179"; then
    error "Không tìm thấy RTL8188ETV (0bda:0179). Hãy cắm USB WiFi vào!"
    echo "Các USB thiết bị hiện có:"
    lsusb
    exit 1
fi
info "Tìm thấy RTL8188ETV:"
lsusb | grep "0bda:0179"
echo ""

# ─── BƯỚC 3: CÀI ĐẶT BUILD DEPENDENCIES ────────────────────────────────────
step "KIỂM TRA BUILD DEPENDENCIES"

NEED_BUILD=false

if ! command -v gcc &>/dev/null || ! command -v make &>/dev/null; then
    NEED_BUILD=true
fi

KHEADERS="/lib/modules/$KERNEL/build"
if [[ ! -d "$KHEADERS" ]]; then
    NEED_BUILD=true
fi

if $NEED_BUILD; then
    warn "Thiếu build tools hoặc kernel headers. Đang cài..."

    if command -v apt-get &>/dev/null; then
        # Ubuntu / Zorin / Debian
        apt-get update -qq
        apt-get install -y -qq gcc make \
            "linux-headers-$KERNEL" 2>/dev/null || \
        apt-get install -y -qq gcc make \
            "linux-headers-generic" 2>/dev/null
    elif command -v dnf &>/dev/null; then
        # Fedora / CentOS 10 / RHEL 10
        dnf install -y -q gcc make \
            "kernel-devel-$KERNEL" 2>/dev/null || \
        dnf install -y -q gcc make kernel-devel 2>/dev/null
    elif command -v yum &>/dev/null; then
        # CentOS / RHEL cũ
        yum install -y -q gcc make \
            "kernel-devel-$KERNEL" 2>/dev/null || \
        yum install -y -q gcc make kernel-devel 2>/dev/null
    else
        error "Không xác định được package manager. Hãy cài gcc, make, kernel-headers thủ công."
        exit 1
    fi
fi

if [[ ! -d "$KHEADERS" ]]; then
    error "Kernel headers không tìm thấy tại $KHEADERS"
    error "Hãy cài: linux-headers-$(uname -r) (Ubuntu) hoặc kernel-devel (RHEL)"
    exit 1
fi

info "Build environment OK (kernel headers: $KHEADERS)"
echo ""

# ─── BƯỚC 4: BUILD DRIVER ────────────────────────────────────────────────────
step "BUILD DRIVER"

cd "$SCRIPT_DIR"

# Xóa binary cũ để đảm bảo build sạch
make clean 2>/dev/null || true

if make -j$(nproc) 2>&1; then
    info "Build thành công: $SCRIPT_DIR/$DRIVER_NAME.ko"
else
    error "Build thất bại!"
    exit 1
fi
echo ""

# ─── BƯỚC 5: DISABLE SYSTEM DRIVER ──────────────────────────────────────────
step "VÔ HIỆU HOÁ SYSTEM DRIVER"

BLACKLIST_FILE="/etc/modprobe.d/rtl8188-custom.conf"
cat > "$BLACKLIST_FILE" <<'EOF'
blacklist rtl8xxxu
blacklist r8188eu
blacklist rtl8188eu
EOF
info "Đã tạo blacklist: $BLACKLIST_FILE"

for mod in rtl8xxxu r8188eu rtl8188eu; do
    if lsmod | grep -q "^$mod "; then
        rmmod "$mod" 2>/dev/null && warn "Đã gỡ $mod" || true
    fi
done

# Gỡ custom driver cũ nếu đang chạy
rmmod "$DRIVER_NAME" 2>/dev/null || true
sleep 1
echo ""

# ─── BƯỚC 6: XÓA DMESG CŨ VÀ LOAD DRIVER ───────────────────────────────────
step "LOAD DRIVER"

dmesg -C

modprobe mac80211  2>/dev/null || true
modprobe cfg80211  2>/dev/null || true

if insmod "$SCRIPT_DIR/$DRIVER_NAME.ko"; then
    info "Driver load thành công!"
else
    error "Load driver thất bại!"
    dmesg | tail -20
    exit 1
fi

sleep 2

# Lấy tên interface
IFACE=$(iw dev 2>/dev/null | awk '/Interface/{print $2}' | head -1)
if [[ -z "$IFACE" ]]; then
    # Thử cách khác
    IFACE=$(ip link show | grep -oP 'wl[^\s:]+' | head -1)
fi

if [[ -z "$IFACE" ]]; then
    error "Không tìm thấy WiFi interface!"
    echo "Kernel log:"
    dmesg | grep -E "RTL8188|ieee80211|phy" | head -20
    exit 1
fi

info "WiFi interface: $IFACE"
echo ""

# ─── BƯỚC 7: THÔNG TIN INTERFACE ─────────────────────────────────────────────
step "THÔNG TIN INTERFACE"
iw dev
ip link show "$IFACE"
echo ""

# ─── BƯỚC 8: THIẾT LẬP REGULATORY ───────────────────────────────────────────
step "THIẾT LẬP REGULATORY DOMAIN"

iw reg set VN 2>/dev/null || iw reg set US 2>/dev/null || true
sleep 1
echo "Regulatory domain:"
iw reg get 2>/dev/null | head -5
echo ""

# ─── BƯỚC 9: CHẠY SCAN ───────────────────────────────────────────────────────
step "CHẠY WIFI SCAN"

ip link set "$IFACE" up 2>/dev/null || true
sleep 1

info "Đang scan (có thể mất 5-15 giây)..."
echo ""
echo "--- KẾT QUẢ SCAN ---"

SCAN_OUTPUT=$(iw dev "$IFACE" scan 2>&1)
if echo "$SCAN_OUTPUT" | grep -q "BSS\|SSID"; then
    echo "$SCAN_OUTPUT" | grep -E "BSS|SSID:|signal:|freq:|last seen" | head -80
    AP_COUNT=$(echo "$SCAN_OUTPUT" | grep -c "^BSS" || echo 0)
    echo ""
    info "Tìm thấy $AP_COUNT access point(s)"
else
    warn "Scan không thấy AP nào (hoặc lỗi)"
    echo "$SCAN_OUTPUT"
fi
echo ""

# ─── BƯỚC 10: THU THẬP KERNEL LOG ────────────────────────────────────────────
step "KERNEL LOG (dmesg)"
dmesg
echo ""

# ─── BƯỚC 11: THỐNG KÊ USB ───────────────────────────────────────────────────
step "THỐNG KÊ USB"
DEV_PATH=$(find /sys/bus/usb/devices -name "idVendor" -exec grep -l "0bda" {} \; 2>/dev/null | head -1 | xargs dirname 2>/dev/null)
if [[ -n "$DEV_PATH" ]]; then
    echo "USB device path: $DEV_PATH"
    echo "URB count: $(cat $DEV_PATH/urbnum 2>/dev/null || echo N/A)"
    echo "Speed: $(cat $DEV_PATH/speed 2>/dev/null || echo N/A) Mbps"
fi
echo ""

# ─── BƯỚC 12: NET DEVICE STATS ────────────────────────────────────────────────
step "NETWORK STATS"
cat /proc/net/dev | grep -E "wlan|wlp|wlx|$IFACE|Inter" || true
echo ""

echo "============================================================"
echo " HOÀN TẤT - $(date)"
echo "============================================================"

} 2>&1 | tee "$RESULT_FILE"

echo ""
echo -e "${GREEN}======================================================${NC}"
echo -e "${GREEN} KẾT QUẢ ĐÃ LƯU TẠI: $RESULT_FILE${NC}"
echo -e "${GREEN} Hãy copy nội dung file đó và gửi lại.${NC}"
echo -e "${GREEN}======================================================${NC}"
echo ""
echo "Lệnh xem nhanh:"
echo "  cat $RESULT_FILE"
echo ""
echo "Lệnh copy sang clipboard (nếu có xclip):"
echo "  xclip -selection clipboard < $RESULT_FILE"
