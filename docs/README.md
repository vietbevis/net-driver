## Tài liệu dự án (phục vụ báo cáo)

Mỗi file dưới đây tương ứng một “chức năng/module” của hệ thống.

- `00-gioi-thieu.md`: mục tiêu, phạm vi, những gì dự án làm/không làm
- `01-kien-truc-tong-quan.md`: kiến trúc tổng quan và tương tác các thành phần
- `02-build-va-cai-dat.md`: build, cài deps, load/unload, quyền hạn
- `03-kernel-module-lifecycle.md`: init/exit, state `g_mon`, đồng bộ hóa
- `04-usb-va-netdev-notifier.md`: phát hiện USB + map USB→interface wlan*
- `05-giao-tiep-char-device.md`: protocol `/dev/rtl8188` (sync/async/timeout)
- `06-packet-monitoring-va-capture.md`: dev_add_pack, deep inspect, ring buffer, filter
- `07-crypto-chat-frame.md`: nhận diện/parse `chat_frame`, hiển thị ciphertext
- `08-procfs.md`: `/proc/rtl8188/*` semantics
- `09-userspace-tui-cli.md`: kiến trúc TUI/CLI, parser output
- `10-troubleshooting.md`: lỗi hay gặp + checklist xử lý

