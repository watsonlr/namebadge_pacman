/**
 * @file frogger_game.h
 * @brief Classic Frogger for BYUI e-Badge V3.0 (ESP32-S3-Mini-1-N4R2)
 *
 * Hardware: ILI9341 320×240 LCD (landscape), 6-button input
 *
 * Controls:
 *   Up    → hop toward homes (north)
 *   Down  → hop back (south)
 *   Left  → hop left
 *   Right → hop right
 *   A     → start / restart
 *   B     → pause / unpause
 */

#ifndef FROGGER_GAME_H
#define FROGGER_GAME_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// ── Button GPIOs (BYUI e-Badge V3.0) ─────────────────────────────────────────
#define BTN_UP     17
#define BTN_DOWN   16
#define BTN_LEFT   14
#define BTN_RIGHT  15
#define BTN_A      38
#define BTN_B      18

// ── Screen / game geometry ────────────────────────────────────────────────────
#define SCREEN_W    320
#define SCREEN_H    240

#define CELL        16          // pixels per grid cell
#define GAME_COLS   20          // 20 × 16 = 320 px
#define GAME_ROWS   14          // 14 × 16 = 224 px
#define HUD_Y       224         // 16-pixel HUD strip below game area
#define HUD_H       16

// Row indices (0 = top)
#define ROW_HOME         0
#define ROW_WATER_TOP    1
#define ROW_WATER_BOT    5
#define ROW_SAFE_MID     6
#define ROW_ROAD_TOP     7
#define ROW_ROAD_BOT     11
#define ROW_SAFE_START   12
#define ROW_START        13     // frog spawns here

// Home slots
#define NUM_HOMES   5

// Frog lives
#define MAX_LIVES   3

// ── RGB565 colours ────────────────────────────────────────────────────────────
#define COLOR_BLACK    0x0000
#define COLOR_WHITE    0xFFFF
#define COLOR_RED      0xF800
#define COLOR_GREEN    0x07E0
#define COLOR_BLUE     0x001F
#define COLOR_YELLOW   0xFFE0
#define COLOR_ORANGE   0xFD20
#define COLOR_CYAN     0x07FF

// ── Public interface ──────────────────────────────────────────────────────────

/** Initialise display and game state. Call once from app_main. */
esp_err_t frogger_init(void);

/** Main game loop tick. Call at ~60 Hz. */
void frogger_game_loop(void);

/** Hard-reset to initial state. */
void frogger_reset(void);

#endif // FROGGER_GAME_H
