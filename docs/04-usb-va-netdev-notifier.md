## 04. USB notifier và Netdev notifier (cơ chế phát hiện thiết bị + interface)

Tài liệu này giải thích 2 bài toán nền tảng:

- Làm sao biết **USB WiFi RTL8188ETV** đang cắm/rút?
- Làm sao tìm đúng **interface wlan*** do driver hệ thống tạo ra cho USB đó?

---

### 4.1 USB device detection: “passive observe” (không claim device)

File liên quan: `rtl8188_usb.c`

#### Vì sao không dùng `usb_register()`

Nếu module đăng ký `usb_driver` bằng `usb_register()` thì kernel sẽ “match” và module có thể **claim interface**. Điều này xung đột trực tiếp với driver hệ thống `rtl8xxxu` (đang cần claim để điều khiển phần cứng).

Do đó dự án chọn:

- `usb_register_notify()` để nhận **thông báo** add/remove
- `usb_for_each_dev()` để quét thiết bị đang có khi mới insmod

#### Flow khi insmod (đã cắm USB từ trước)

1. `rtl8188_usb_init()` gọi `usb_register_notify(&g_mon->usb_nb)`
2. Gọi `usb_for_each_dev(NULL, find_our_usb)`
3. Nếu match VID/PID:
   - `g_mon->udev = usb_get_dev(udev)`
   - `g_mon->dev_present = true`

Điểm đáng nói trong báo cáo:

- `usb_get_dev()` tăng refcount để `usb_device` không bị free khi module còn dùng.

#### Flow khi cắm/rút USB

Callback `rtl8188_usb_notify(nb, action, data)`:

- **USB_DEVICE_ADD**:
  - Nếu chưa `dev_present`:
    - `g_mon->udev = usb_get_dev(udev)`
    - `dev_present = true`
    - gọi `scan_existing_netdev()` để tìm interface tương ứng
- **USB_DEVICE_REMOVE**:
  - Nếu đúng device mình đang theo dõi:
    - nếu packet handler đang đăng ký → `dev_remove_pack()`
    - clear `ndev`, `ifname`
    - `usb_put_dev(g_mon->udev)` và set `udev=NULL`, `dev_present=false`

---

### 4.2 Netdev tracking: tìm đúng interface wlan* của USB

File liên quan: `rtl8188_netdev.c`

Kernel có nhiều interface (eth0, lo, wlan0…), nên module cần tìm đúng interface thuộc về RTL8188ETV.

#### Ý tưởng: dò device tree của net_device

Hàm `is_our_iface(struct net_device *ndev)`:

- Bắt đầu từ `ndev->dev.parent`
- Lặp: `d = d->parent`
- Nếu gặp `&g_mon->udev->dev` → **đây là interface do USB device đó tạo**

Ưu điểm:

- Không phụ thuộc tên interface (wlan0, wlp10s0u4u1…)
- Không cần hardcode theo driver name

#### Netdev notifier làm gì

`register_netdevice_notifier(&g_mon->net_nb)` nhận event:

- **NETDEV_REGISTER / NETDEV_UP**:
  - nếu `is_our_iface(dev)`:
    - `g_mon->ndev = dev`
    - `strscpy(g_mon->ifname, dev->name, IFNAMSIZ)`
- **NETDEV_UNREGISTER**:
  - dừng monitor nếu cần (`dev_remove_pack`)
  - clear `ndev`, `ifname`
- **NETDEV_CHANGENAME**:
  - cập nhật `ifname` theo tên mới

#### Rescan khi cần: `scan_existing_netdev()`

Trong các tình huống:

- USB vừa cắm nhưng netdev đã tồn tại (race)
- netdev rename/replug
- cache `ifname` không còn đúng

Module gọi `scan_existing_netdev()`:

- `rtnl_lock()`
- `for_each_netdev(&init_net, dev)`:
  - nếu `is_our_iface(dev)` thì lưu `ndev/ifname`
- `rtnl_unlock()`

---

### 4.3 Những race condition “thực tế” và cách dự án giảm rủi ro

- **USB ADD trước khi netdev REGISTER**:
  - USB notifier gọi `scan_existing_netdev()` nhưng chưa thấy interface.
  - Sau đó netdev notifier sẽ nhận NETDEV_REGISTER và cập nhật.
- **Netdev rename**:
  - notifier NETDEV_CHANGENAME cập nhật `ifname`
- **USB REMOVE trong khi đang monitor**:
  - USB remove handler và netdev unregister đều có logic dừng `dev_add_pack` để tránh callback chạy vào state đã mất.

