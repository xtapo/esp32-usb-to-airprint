# Add AirPrint / AirScan support to your old printer, using just a cheap ESP32-S3

> [!IMPORTANT]
> This project is actively looking for contributors to add support for more printers. Most importantly, you need to really own the printer you add: every driver must be tested on the real hardware, not only in emulation. See [Adding a printer](#adding-a-printer) to get started.

📖 **Tiếng Việt:** Xem hướng dẫn sử dụng chi tiết tại [docs/HUONG_DAN.md](docs/HUONG_DAN.md).

Turns any USB-only printers into a wireless printer and scanner. An ESP32-S3 board plugs into the printer's USB port and joins your WiFi:

- **Print** from iPhone, iPad and Mac (AirPrint), Android (Mopria) and Linux, with no drivers or apps.
- **Scan** from any browser, from Image Capture and Preview on a Mac, or with the button on the board.
- **A bilingual (EN / VI) web app** on your phone or computer: print PDFs and pictures, scan, keep the files in the browser, put pages together (rotate, crop, ID card copies) into a PDF, configure WiFi / Static IP / AP & Admin password, and update firmware over WiFi (Web OTA).

![](./docs/IMG_3577.jpeg)

| | |
|---|---|
| ![](./docs/screenshot-printer.png) | ![](./docs/screenshot-scanned.png) |
| ![](./docs/screenshot-files.png) | ![](./docs/screenshot-process.png) |

## Supported printers

| Series | Models | Scan |
|---|---|---|
| 🟢 Canon MG2500 | MG2550S | yes |
| 🟡 Canon MG2400 | MG2410 to MG2490 | yes |
| 🟡 Canon MG2500 | MG2510 to MG2590 (other than the MG2500S) | yes |
| 🟡 Canon P200 | P200 | no scanner |
| 🟡 Canon MG2900 | MG2910 to MG2990 | yes |
| 🟡 Canon MG3000 | MG3010 to MG3090 | yes |
| 🟡 Canon E470 | E470 to E478 | yes |
| 🟡 Canon E460 | E460 to E468 | yes |
| 🟡 Canon iP2800 | iP2810 to iP2899 | no scanner |
| 🟡 Canon E3100 | E3170 to E3195 | yes |
| 🟡 Canon TS3100 | TS3110 to TS3195 | yes |
| 🟡 Canon MP230 | MP230 to MP237 | yes |
| 🟡 Canon iP2700 | iP2700 to iP2788 | no scanner |
| 🟡 Canon MP250, MP270, MP280 | MP250 to MP288 | yes |
| 🟡 Canon MP490, MP493, MP495 | MP490 to MP499 | yes |
| 🟡 Canon MX340, MX350 | MX340 to MX358 | yes (glass only) |
| 🟡 Canon MX360, MX410, MX420 | MX360 to MX428 | yes (glass only) |
| 🟡 Canon LBP (CAPT) | LBP2900, LBP3000, LBP3050, LBP3300 (auto duplex), LBP6018L, LBP6030, LBP6040 | no scanner |
| 🟡 HP LaserJet (ZjStream) | 1000, 1005, 1018, 1020, 1022, M1319, P1102/w, P1566, P1606dn, M12a | no scanner |
| 🟡 HP LaserJet (XQX) | P1005–P1009, P1505, P2014, M1005, M1120, M1132, M1136, M1212nf | no scanner |
| 🟡 Brother Laser | HL-1110, HL-1210W, HL-2130, HL-2240, HL-L2300D, DCP-1510, DCP-1610W, DCP-L2520D (+ generic `0x04f9`) | no scanner |

- 🟢 Validated on real printer
- 🟡 Expected to work, not tested on real hardware yet.

The firmware recognises the printer when it is plugged in. Other printers? Please open an issue or a PR.

## What you need

Just one thing: An ESP32-S3 dev board with two USB-C ports:

![](./docs/amazon.png)

Optionally: a SSD1315 OLED screen and a 3D-printed case, see later in this tutorial

## Setup

**Step 1: Hardware.** Plug the ESP32-S3 into your computer via the COM port on the ESP32 board (NOT the port labelled "USB")

![](./docs/IMG_3538.jpeg)

**Step 2: Settings.** Either configure WiFi via `.env` (build once with `pio run` to generate `.env`), OR leave `WIFI_SSID` empty to use the built-in **WiFi Captive Portal** (`AirPrint-Setup-XXXX` at `http://192.168.4.1/#/wifi`):
   ```
   WIFI_SSID=your-network
   WIFI_PASSWORD=your-password
   PRINTER_NAME=Canon MG2500 (ESP32)
   HOSTNAME=canon-mg2500
   MODE=normal
   ```
   Web UI firmware updates (Web OTA at `#/wifi`) work in both `MODE=normal` and `MODE=debug`. `MODE=debug` additionally enables remote logs, crash core dumps, and CLI OTA (`tools/remote.py`). `.env` is git-ignored, so your WiFi password stays local. You can also reconfigure WiFi, Static IP, and security later from `#/wifi` or by pressing `GPIO5`.

**Step 3: Flash.** Connect the board's **COM** port to your computer and run `pio run -t upload` — or use the browser-based **Web Flasher & Serial Monitor** in `flasher/`:
- After `pio run -e esp32s3`, run `python tools/make_factory_bin.py --copy-to-flasher`, then `cd flasher && npm install && npm run dev`.
- In Web Flasher, flash the merged image (`universal-airprint.bin` / `factory.bin` at `0x0`), or upload split `.bin` files at `0x0` (`bootloader.bin`), `0x8000` (`partitions.bin`), `0xf000` (`ota_data_initial.bin`), and `0x20000` (`firmware.bin`).
- PlatformIO finds the board's serial port; if several are connected, add `--upload-port /dev/cu.usbmodem…`.

## Using it

Plug the printer to the port labelled "USB"

NOTE: You may also need to bridge the "USB-OTG" solder join on the board, so that it supplies 5V to the printer. While the printer doesn't actively use 5V, it sense the USB plug using this signal.

- **The web app:** open `http://canon-mg2500.local/` (your `HOSTNAME`) or the board's IP on your phone or computer. Switch language anytime with the **`VI` / `EN`** button at the bottom of the sidebar.
- **WiFi & Static IP (`#/wifi`):** connect to a 2.4 GHz network using DHCP or enable **Use Static IP** (`IP`, `Subnet Mask`, `Gateway`, optional `DNS`). If the board cannot reach WiFi with the saved settings, it opens the Setup AP again after ~15 s.
- **Security & Setup AP (`#/wifi`):** set an **Admin Password** to protect WiFi, security, and OTA endpoints (`X-Admin-Password`), and customize the Setup AP name and optional WPA2 password (8–63 chars).
- **Web OTA firmware update (`#/wifi`):** upload the app-only `.pio/build/esp32s3/firmware.bin` directly from the browser (works in `MODE=normal`; do not upload merged `factory.bin` / `universal-airprint.bin`). New builds roll back automatically if WiFi fails to stabilize after reboot.
- **The button on the board (`BOOT` / `GPIO4`):** press once to scan, press twice for a test page, hold 2 s to cancel a running job, or hold 8 s while idle to reset WiFi & passwords.
- **The user button (`GPIO5`):** short press enables the Setup AP on demand; hold 5 s to reset WiFi, Static IP, and passwords to factory defaults.
- **Print from a phone or a Mac:** on an iPhone, Share → Print and pick the printer. On a Mac, add it in System Settings → Printers & Scanners.
- **Scan on a Mac:** open Image Capture (or Preview → File → Import from Scanner) and pick the printer.

## Adding OLED screen and buttons

This step is **optional**, you can connect a SSD1315 OLED screen with the pinout below:

| OLED | ESP32-S3 |
|---|---|
| VCC | 3V3 |
| GND | GND |
| SCL | GPIO41 |
| SDA | GPIO42 |

Connect it before powering on: the board only looks for it at startup.

![](./docs/IMG_3568.jpeg)

For buttons, wire each one between a pin and GND:

| Button | ESP32-S3 | Action |
|---|---|---|
| 1 | `BOOT` (`GPIO0`) / `GPIO4` | Press = scan, double press = test page, hold 2 s = cancel job, hold 8 s (idle) = reset WiFi & passwords |
| 2 | `GPIO5` | Short press = start Setup AP (`#/wifi`), hold 5 s = reset WiFi, Static IP & passwords |

While the screen is off, a press only wakes it up.

## Development

Set `MODE=dev` and flash: print jobs are processed but not printed, and scans show a built-in test page.

On the computer, without the board: `make -C host && cd host && ./airprint_host`, then open http://localhost:8631/ (an emulated printer and scanner). To work on the web app with live reloading: `cd web && npm run dev` (it talks to that server; `DEVICE=http://<board IP> npm run dev` talks to the board).

Technical details are in [AGENTS.md](AGENTS.md).

## Adding a printer

Each printer model is a driver in `components/printers/`, listed in `printers.c`; the firmware picks the one of the printer that is plugged in. A Canon PIXMA whose stream is close to the MG2500's is one line in `canon_pixma.c`; another printer is a new file in the same folder. See [AGENTS.md](AGENTS.md) for details.

## License

MIT, see [LICENSE](LICENSE).

The printer and scanner protocols follow [Gutenprint](https://gimp-print.sourceforge.io/) and [SANE](http://www.sane-project.org/)'s pixma backend. The JPEG encoder's DCT is a translation of libjpeg's `jfdctint.c`: this software is based in part on the work of the Independent JPEG Group. The web UI bundles [Svelte](https://svelte.dev/) (MIT) and [pdf.js](https://mozilla.github.io/pdf.js/) (Apache-2.0).
