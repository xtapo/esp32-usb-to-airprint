# Hướng dẫn sử dụng ESP32-S3 AirPrint / AirScan

Tài liệu này hướng dẫn đầy đủ từ khâu chuẩn bị phần cứng, nạp firmware qua trình duyệt (**Web Flasher**), cấu hình WiFi / **IP tĩnh**, **bảo mật mật khẩu quản trị**, **cập nhật OTA qua WiFi**, đến thao tác các **nút cứng** trên mạch ESP32-S3.

---

## 1. Chuẩn bị phần cứng & máy in hỗ trợ

- **Mạch:** ESP32-S3 có 2 cổng USB-C (`COM/UART` và `USB/OTG`), ví dụ ESP32-S3-N16R8 hoặc N8R8.
  - ⚠️ **Quan trọng:** Cần hàn nối cầu hàn **`USB-OTG` (5V VBUS)** trên mạch ESP32-S3 để cấp nguồn nhận diện cho cổng USB máy in (nếu chưa nối cầu, máy in sẽ không nhận ra kết nối USB).
- **Máy in hỗ trợ (tự động nhận diện qua USB):**
  - **Canon PIXMA:** MG2500, MG2400, MG2900, MG3000, E470, E460, E3100, TS3100, MP230–MP495, MX340–MX420 (In + Scan), iP2700, iP2800, P200 (chỉ in).
  - **Canon LBP (CAPT):** LBP2900, LBP3000, LBP3050, **LBP3300 (hỗ trợ in 2 mặt tự động Duplex)**, LBP6018L, LBP6030, LBP6040.
  - **HP LaserJet:** ZjStream (`1000, 1005, 1018, 1020, 1022, M1319, P1102/w, P1566, P1606dn, M12a`) & XQX (`P1005–P1009, P1505, P2014, M1005, M1120, M1132, M1136, M1212nf`).
  - **Brother Laser đơn sắc:** HL-1110, HL-1210W, HL-2130, HL-2240, HL-L2300D, DCP-1510, DCP-1610W, DCP-L2520D (và driver generic VID `0x04f9`).

---

## 2. Nạp Firmware lần đầu qua Web Flasher (không cần cài công cụ phức tạp)

1. Cắm cáp USB vào cổng **`COM` / `UART`** trên mạch ESP32-S3 (không cắm vào cổng `USB`).
2. Mở thư mục `flasher/` và chạy:
   ```bash
   cd flasher
   npm install
   npm run dev
   ```
   *(Hoặc mở trang Web Flasher đã build sẵn trong `flasher/dist/` trên trình duyệt **Chrome** hoặc **Edge** bản Desktop hỗ trợ Web Serial API.)*
3. Trong giao diện **ESP32 AirPrint Studio**:
   - Chọn tốc độ nạp (mặc định `460,800 bps`), bấm **Kết Nối ESP** và chọn cổng COM của mạch (nếu mạch không tự vào bootloader, giữ nút `BOOT` khi cắm cáp).
   - Ở tab **Kho Firmware có sẵn**, chọn **Universal AirPrint & AirScan** (hoặc thẻ tương ứng như **Canon LBP3300 AirPrint (CAPT Duplex)** — các thẻ đều dùng chung bản Universal tự nhận diện máy in).
   - Hoặc chuyển sang tab **Tải file .bin riêng**:
     - Nếu nạp file gộp (`universal-airprint.bin` / `factory.bin`): giữ địa chỉ **`0x0`**.
     - Nếu nạp từng phần từ `.pio/build/esp32s3/`:
       - `bootloader.bin` → **`0x0`**
       - `partitions.bin` → **`0x8000`**
       - `ota_data_initial.bin` → **`0xf000`**
       - `firmware.bin` → **`0x20000`**
   - Bấm **Bắt đầu nạp Firmware**. Sau khi nạp xong, mạch tự động reset và nhả cổng Serial; bạn có thể chuyển sang tab **Serial Monitor** (115200 baud) để xem log khởi động.

*(Nếu build trực tiếp từ mã nguồn PlatformIO: chạy `pio run -e esp32s3` rồi `python tools/make_factory_bin.py --copy-to-flasher` để cập nhật `universal-airprint.bin`.)*

---

## 3. Kết nối WiFi & cài đặt IP tĩnh (Static IP)

1. Cấp nguồn cho mạch ESP32-S3 và cắm cáp máy in vào cổng **`USB`** trên ESP32-S3.
2. Khi chưa cấu hình WiFi (hoặc không kết nối được sau ~15 giây), mạch sẽ tự phát WiFi cài đặt tên **`AirPrint-Setup-XXXX`** (mặc định không mật khẩu).
3. Dùng điện thoại hoặc máy tính kết nối vào WiFi `AirPrint-Setup-XXXX`, trình duyệt thường tự mở trang cài đặt; nếu không, truy cập **`http://192.168.4.1/#/wifi`**.
4. Tại trang **WiFi & Cài đặt**:
   - Bấm **Quét WiFi** để chọn mạng WiFi **2.4 GHz** (ESP32-S3 không hỗ trợ 5 GHz) và nhập mật khẩu.
   - Tùy chọn chỉnh **Tên Hostname (.local)** (truy cập qua `http://<hostname>.local/`) và **Tên hiển thị AirPrint**.
   - **Nếu muốn dùng IP tĩnh (Static IP):** tích chọn **Dùng IP tĩnh (thay vì DHCP tự động)** và điền:
     - **Địa chỉ IP tĩnh:** ví dụ `192.168.1.200` (chọn ngoài dải cấp phát DHCP của router để tránh trùng IP).
     - **Subnet Mask:** thường là `255.255.255.0`.
     - **Gateway (IP Router):** ví dụ `192.168.1.1`.
     - **DNS Server:** tùy chọn (nếu để trống sẽ tự dùng Gateway).
5. Bấm **Lưu & Khởi động lại**. Mạch sẽ kết nối WiFi mới và dùng IP tĩnh bạn đã đặt.
   - *Lưu ý:* Nếu nhập nhầm IP/Gateway làm mạch không vào được mạng, sau ~15 giây mạch sẽ tự bật lại AP `AirPrint-Setup-XXXX` để bạn vào sửa lại.

---

## 4. Chuyển ngôn ngữ Việt / Anh trên Web UI

Giao diện Web điều khiển của mạch (truy cập tại `http://<IP>/` hoặc `http://<hostname>.local/`) hỗ trợ đầy đủ **Tiếng Việt** và **Tiếng Anh**:
- Lần đầu mở, giao diện tự nhận ngôn ngữ theo trình duyệt.
- Bạn có thể bấm nút **`VI` / `EN`** ở góc dưới thanh menu bên trái để chuyển đổi bất kỳ lúc nào (lựa chọn được lưu trên trình duyệt).

---

## 5. Bảo mật mật khẩu quản trị & tùy biến AP cài đặt

Trong trang **WiFi & Cài đặt (`#/wifi`)**, thẻ **Bảo mật & WiFi AP cài đặt** cho phép:
- **Tên WiFi AP cài đặt:** Đặt tên riêng thay cho `AirPrint-Setup-XXXX` mặc định (tối đa 32 ký tự; để trống để về mặc định).
- **Mật khẩu AP cài đặt:** Đặt mật khẩu WPA2 (8–63 ký tự) cho mạng AP cấu hình để người khác không tùy tiện kết nối khi AP đang bật.
- **Mật khẩu quản trị (Admin Password):** Khi được đặt, mọi thao tác thay đổi WiFi/IP tĩnh, reset WiFi, đổi cài đặt bảo mật và cập nhật firmware OTA đều yêu cầu nhập đúng mật khẩu quản trị (`X-Admin-Password`).

---

## 6. Cập nhật Firmware qua WiFi (Web OTA)

Không cần tháo mạch ra cắm cáp USB, bạn có thể cập nhật trực tiếp từ điện thoại/máy tính ngay cả khi mạch đang ở `MODE=normal`:
1. Mở trang **`#/wifi`** trên trình duyệt → kéo xuống thẻ **Cập nhật Firmware qua WiFi (OTA)**.
2. Nếu đã bật mật khẩu quản trị, nhập mật khẩu quản trị ở thanh trên cùng.
3. Bấm **Chọn file firmware.bin** và chọn file ảnh ứng dụng **`.pio/build/esp32s3/firmware.bin`** (~1.9 MB).
   - ⚠️ **Lưu ý:** **KHÔNG** chọn file gộp `factory.bin` / `universal-airprint.bin` ở mục Web OTA này (file gộp chỉ dùng khi nạp qua USB ở địa chỉ `0x0`).
4. Bấm **Tải lên & Cập nhật**. Sau khi ghi xong vào phân vùng OTA dự phòng (`ota_0`/`ota_1`), mạch tự khởi động lại. Nếu firmware mới không kết nối WiFi ổn định, bootloader sẽ tự **rollback** về bản cũ an toàn.

---

## 7. Thao tác nút cứng & khôi phục cài đặt gốc

| Nút | Chân ESP32-S3 | Thao tác |
|---|---|---|
| **Nút Scan / Print** | `BOOT` (`GPIO0`) hoặc `GPIO4` | • **Bấm 1 lần:** Scan 1 trang A4 (150 dpi) gửi tới mọi tab Web đang mở<br>• **Bấm 2 lần nhanh (đúp):** In trang thử (hoặc dry-run nếu chưa gắn máy in)<br>• **Nhấn giữ 2 giây** (khi đang in/scan): Hủy lệnh đang chạy<br>• **Nhấn giữ 8 giây** (khi đang rảnh): **Xóa sạch WiFi, IP tĩnh & mật khẩu**, khởi động lại về AP cài đặt |
| **Nút Cài đặt (User)** | `GPIO5` | • **Bấm ngắn:** Bật lại WiFi AP cài đặt (`AirPrint-Setup-XXXX`) mà không mất cấu hình cũ<br>• **Nhấn giữ 5 giây:** **Xóa sạch WiFi, IP tĩnh & mật khẩu**, khởi động lại về AP cài đặt (dùng để cứu mạch khi quên mật khẩu hoặc đổi router) |

*(Khi màn hình OLED đang tắt để chống burn-in, lần bấm đầu tiên chỉ đánh thức màn hình.)*

### Sơ đồ nối màn hình OLED (tùy chọn)
| Chân OLED | ESP32-S3 |
|---|---|
| `VCC` | `3V3` |
| `GND` | `GND` |
| `SCL` | `GPIO41` |
| `SDA` | `GPIO42` |
