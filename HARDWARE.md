# Hardware Configuration

## Target Device

| Item | Spec |
| --- | --- |
| MCU | ESP32-S3-Mini-1-N8 |
| Architecture | Xtensa dual-core LX7, 160 MHz default |
| Flash | 4 MB |
| PSRAM | 2 MB (AP_3v3) |
| Wi-Fi | 2.4 GHz 802.11 b/g/n |
| Bluetooth | BLE 5.0 |

> **Note**: GPIO 26 is reserved for octal PSRAM — do not use.

---

## BYUI eBadge V4.0 — Full Pinout

### Display — ILI9341 2.4" TFT (SPI2, write-only)

| Signal | GPIO |
| --- | --- |
| CS | 0 |
| DC | 45 |
| RST | 1 |
| CLK | 46 |
| MOSI | 3 |

- Resolution: 320×240 (landscape)
- Orientation: MADCTL `0x40` (MX bit) — MX=1 mirrors the column address so x=0 is the physical right edge; game code compensates via `tile_px()` mirror
- Display inversion: send `INVON` (0x21) during init — panel powers up inverted by default
- Color format: RGB565, big-endian (high byte first), 40 MHz SPI Mode 0
- No MISO — display is write-only

### Buttons — active LOW, internal pull-up

| Button | GPIO |
| --- | --- |
| Up | 11 |
| Down | 47 |
| Left | 21 |
| Right | 10 |
| A | 34 |
| B | 33 |

> **Note**: GPIO 33 and 34 are listed in some ESP32-S3 documentation as connected to internal flash on certain flash-size variants. On the N8 (8 MB flash) these GPIOs are available as general-purpose I/O. Verify against your specific module's schematic before use.

### LEDs

| Signal | GPIO |
| --- | --- |
| RGB Red | 2 |
| RGB Green | 4 |
| RGB Blue | 5 |
| Single LED | 6 |
| WS2813B strip (RMT) | 7 |

### Buzzer (piezo)

| Signal | GPIO |
| --- | --- |
| Buzzer | 48 |

Drive with LEDC PWM or GPIO toggle for tones.

### Joystick

| Axis | GPIO | Note |
| --- | --- | --- |
| X | 8 | ADC |
| Y | 9 | ADC |

### Accelerometer — MMA8452Q (I2C address 0x1C)

| Signal | GPIO |
| --- | --- |
| SDA | 41 |
| SCL | 42 |

### Battery Voltage

| Signal | GPIO |
| --- | --- |
| ADC input | 12 |

### SD Card — SPI3

| Signal | GPIO |
| --- | --- |
| CS | 40 |
| CLK | 38 |
| MOSI | 39 |
| MISO | 37 |

### USB-Serial Bridge — CP2102N

| Signal | GPIO |
| --- | --- |
| UART0 TX | 43 |
| UART0 RX | 44 |

Baud: 115200, 8N1. USB is device-mode only (no host); native USB on J2 is USB-Serial/JTAG.

---

## Reserved / Do Not Use

| GPIO | Reason |
| --- | --- |
| 26 | Octal PSRAM |
| 33–37 | Connected to internal flash on some variants (see button note above re: 33–34 on N8) |

---

## Power Requirements

| Mode | Typical Current |
| --- | --- |
| Active Wi-Fi TX | 120–140 mA |
| Active Wi-Fi RX | 80–95 mA |
| Modem sleep | 15–20 mA |
| Light sleep | ~0.8 mA |
| Deep sleep | ~5 µA |

Supply: 3.0–3.6 V, ≥500 mA recommended.

---

## Flash Layout

See `partitions.csv` for the full partition table.

| Partition | Type | Offset | Size |
| --- | --- | --- | --- |
| nvs | data/nvs | 0x9000 | 24 KB |
| otadata | data/ota | 0xF000 | 8 KB |
| phy_init | data/phy | 0x11000 | 4 KB |
| factory | app/factory | 0x20000 | 960 KB |
| ota_0 | app/ota_0 | 0x110000 | 960 KB |
| ota_1 | app/ota_1 | 0x200000 | 960 KB |
| ota_2 | app/ota_2 | 0x2F0000 | 960 KB |

---

## Component Datasheets

| Component | Part | Link |
| --- | --- | --- |
| MCU | ESP32-S3-Mini-1-N8 | [Datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-s3-mini-1_mini-1u_datasheet_en.pdf) |
| Accelerometer | MMA8452QR1 | [Datasheet](https://lcsc.com/datasheet/lcsc_datasheet_2405281404_NXP-Semicon-MMA8452QR1_C11360.pdf) |
| Buzzer | MLT-5020 | [Datasheet](https://lcsc.com/datasheet/lcsc_datasheet_2410121451_Jiangsu-Huaneng-Elec-MLT-5020_C94598.pdf) |
| Charger | TP4056 | [Datasheet](https://www.lcsc.com/datasheet/lcsc_datasheet_1809261820_TOPPOWER-Nanjing-Extension-Microelectronics-TP4056-42-ESOP8_C16581.pdf) |
| RGB LED | RS-3535MWAM | [Datasheet](https://lcsc.com/datasheet/lcsc_datasheet_2410121728_Foshan-NationStar-Optoelectronics-RS-3535MWAM_C842778.pdf) |
| SD Card socket | TF PUSH | [Datasheet](https://lcsc.com/datasheet/lcsc_datasheet_2504101957_SHOU-HAN-TF-PUSH_C393941.pdf) |
| USB-Serial | CP2102N-A02-GQFN28R | [Datasheet](https://lcsc.com/datasheet/lcsc_datasheet_2304140030_SKYWORKS-SILICON-LABS-CP2102N-A02-GQFN28R_C964632.pdf) |
| Addressable LEDs | WS2813B-2121 | [Product page](https://item.szlcsc.com/datasheet/WS2813B-2121/23859548.html) |

---

## Troubleshooting

| Symptom | Common Cause | Fix |
| --- | --- | --- |
| Device not detected | Charge-only USB cable | Use a data USB cable |
| `python\r: No such…` | CRLF line endings in WSL | `.gitattributes` enforces LF; run `git reset --hard` |
| Flash failure | Baud rate too high | Add `-b 115200` to `idf.py flash` |
| Wi-Fi not working | 5 GHz network | Switch to 2.4 GHz |
| Brownout / resets | Weak power supply | Add bulk cap; check >500 mA supply |

---

**Last Updated**: March 2026 — V4.0 hardware
