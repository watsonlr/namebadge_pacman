# namebadge_tetris

Standalone bare-metal Tetris for the **BYUI e-Badge V3.0** (ESP32-S3-Mini-1-N4R2).

No OTA, no launcher, no Wi-Fi — just Tetris.

---

## Hardware

> Full pin assignments, peripheral datasheets, and troubleshooting: [HARDWARE.md](HARDWARE.md)

| Feature | Details |
|---------|---------|
| MCU | ESP32-S3-Mini-1-N4R2 (dual-core Xtensa LX7 @ 240 MHz) |
| Flash | 4 MB embedded (XMC) |
| PSRAM | 2 MB embedded (AP_3v3) |
| Display | ILI9341 2.4" TFT, 240×320, portrait, RGB565 little-endian |
| Interface | SPI2 @ 40 MHz |

### Pin Assignments

#### Display (ILI9341)
| Signal | GPIO |
|--------|------|
| MOSI   | 11   |
| CLK    | 12   |
| CS     | 9    |
| DC     | 13   |
| RST    | 48   |

#### Buttons (all active-LOW with internal pull-up)
| Button | GPIO | Tetris action |
|--------|------|---------------|
| Up     | 17   | Move left     |
| Down   | 16   | Move right    |
| Left   | 14   | Soft drop     |
| Right  | 15   | Rotate piece  |
| A      | 38   | Hard drop     |
| B      | 18   | Pause / Restart |

### RGB565 Byte Order

The ILI9341 on this board requires **little-endian** byte order over SPI:

```c
// CORRECT — low byte first
uint8_t pixel[2] = { color & 0xFF, color >> 8 };

// WRONG — do NOT use big-endian
// uint8_t pixel[2] = { color >> 8, color & 0xFF };
```

Example: `GREEN = 0x07E0` → send `0xE0, 0x07` on the SPI bus.

---

## Controls

| State     | Button | Action         |
|-----------|--------|----------------|
| Playing   | Up     | Move left      |
| Playing   | Right  | Rotate         |
| Playing   | Down   | Move right     |
| Playing   | Left   | Soft drop (+1 pt) |
| Playing   | A      | Hard drop (+2 pts) |
| Playing   | B      | Pause          |
| Paused    | A      | Resume         |
| Paused    | B      | Restart game   |
| Game Over | B      | Restart game   |

---

## Scoring

| Lines cleared | Points (× level) |
|---------------|-----------------|
| 1             | 100             |
| 2             | 300             |
| 3             | 500             |
| 4 (Tetris!)   | 800             |

Level increases every 10 lines cleared.  Drop speed increases each level.

---

## Building

> Full build steps, WSL setup, esptool commands, and troubleshooting: [BUILD_GUIDE.md](BUILD_GUIDE.md)

Requires **ESP-IDF v5.x** with ESP32-S3 support installed.

```bash
# From an ESP-IDF terminal (PowerShell or CMD)
cd namebadge_tetris
idf.py set-target esp32s3
idf.py build
idf.py -p COM<N> flash monitor
```

Or use the ESP-IDF VS Code extension (set target to `esp32s3` before first build).

### Flashing manually with esptool

```bash
python -m esptool --chip esp32s3 -p COM<N> -b 460800 \
    --before default_reset --after hard_reset write_flash \
    --flash_mode qio --flash_size 4MB --flash_freq 80m \
    0x0      build/bootloader/bootloader.bin \
    0x8000   build/partition_table/partition-table.bin \
    0x10000  build/tetris_game.bin
```

---

## Project Structure

```
namebadge_tetris/
├── CMakeLists.txt          # Top-level ESP-IDF project
├── partitions.csv          # Simple nvs + factory partition table
├── sdkconfig.defaults      # ESP32-S3-Mini-1-N4R2 defaults
├── README.md
└── main/
    ├── CMakeLists.txt
    ├── tetris_main.c        # app_main entry point
    ├── tetris_game.h        # Types, constants, public API
    ├── tetris_game.c        # Game logic + optimised renderer
    ├── lcd_driver.h         # Display API
    └── lcd_driver.c         # ILI9341 SPI driver (RGB565 little-endian)
```

---

## Rendering Notes

Performance was a key concern on this hardware (SPI display at 40 MHz, 2 MB PSRAM available but not used by the renderer).

- **Dirty tracking**: Only board cells that changed since the last frame are redrawn.
- **Piece delta**: The active piece is erased at its previous position before being drawn
  at the new one — no full-board clear each frame.
- **Line-buffer fill**: `lcd_fill_rect` sends one SPI transaction per row (128–240 bytes)
  rather than one per pixel.  A 12×12 block costs 12 transactions instead of 144.
- **Character blit**: Each 8×8 character is assembled into a 128-byte buffer and sent
  in a single SPI transaction.
- Result: solid 60 FPS with responsive button input.

---

## License

See [LICENSE](LICENSE).
