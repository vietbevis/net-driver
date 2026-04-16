## 02. Build & cài đặt (kernel module + TUI/CLI)

### Các artifact được build

- **Kernel module**: `rtl8188_mon.ko`
- **Userspace tool**: `rtl8188_cli` (TUI/CLI dùng ncurses)

### Phụ thuộc (dependency)

Phụ thuộc tối thiểu theo README/`install.sh`:

- **Kernel headers / kernel-devel** khớp kernel đang chạy
- `gcc`, `make`
- `ncurses` headers (ví dụ `ncurses-devel` hoặc `libncurses-dev`)
- `iw` (bắt buộc cho scan/link)
- `pkg-config` (tùy hệ thống, để lấy flags cho ncurses)

### Build bằng Makefile

`Makefile` build 2 phần:

- **Phần kernel (Kbuild)**:
  - `obj-m += rtl8188_mon.o`
  - `rtl8188_mon-objs := rtl8188_main.o rtl8188_usb.o ... rtl8188_chrdev.o`
  - Gọi `$(MAKE) -C /lib/modules/$(uname -r)/build M=$(PWD) modules`
- **Phần userspace**:
  - Nguồn: `tui/rtl8188_main_tui.c`, `tui/rtl8188_tui.c`, `tui/rtl8188_tabs.c`
  - Link ncurses (`-lncursesw` hoặc theo pkg-config)

Các target hay dùng:

- `make`: build cả module + cli
- `make module`: chỉ build kernel module
- `make cli`: chỉ build `rtl8188_cli`
- `make load`: load module (giữ `rtl8xxxu` chạy song song)
- `make unload`, `make reload`
- `make status`: kiểm tra lsmod/lsusb/`/dev`/`/proc`

### Cách `make load` hoạt động (tại sao “không xung đột”)

Target `load` làm:

1. `modprobe rtl8xxxu` (best-effort) để đảm bảo driver thật đã chạy
2. `rmmod rtl8188_mon` (nếu đang có)
3. `insmod rtl8188_mon.ko`

Vì module companion **không dùng `usb_register()`**, nó không claim thiết bị USB, nên không tranh chấp với `rtl8xxxu`.

### Script `install.sh` làm gì

`install.sh` là “one-shot installer”:

- Tự cài dependencies theo distro: `dnf`/`yum` hoặc `apt-get`
- Nếu SELinux đang **Enforcing** → cố gắng set **Permissive** (runtime) vì dự án gọi `call_usermodehelper()` để chạy `iw/ip` từ kernel
- Cố gắng set capabilities cho `iw` (cap_net_admin, cap_net_raw) để thao tác WiFi dễ hơn (dù load module vẫn cần root)
- `make module` + `make cli`
- Nếu module đang load → `make reload`, ngược lại → `make load`

### Quyền hạn (permissions)

- Load/unload kernel module: cần **root**
- Truy cập `/dev/rtl8188`: thường cần **root** (trừ khi bạn tự chmod/udev rule)
- Các lệnh scan/connect ở phía kernel dùng `call_usermodehelper`:
  - Nếu SELinux chặn sẽ dễ gặp lỗi kiểu `ret=-13` (EACCES)

