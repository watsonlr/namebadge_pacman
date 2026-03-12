/**
 * @file pacman_game.h
 * @brief Classic Pac-Man for BYUI e-Badge V3.0 (ESP32-S3-Mini-1-N4R2)
 *
 * Hardware: ILI9341 320×240 LCD (landscape), 6-button input
 *
 * Screen layout:
 *   Maze: 28×28 tiles × 8px = 224×224px, centered at x=48, y=0
 *   Side panels: x=0..47 (left), x=272..319 (right) — score/lives display
 *   HUD strip: y=224..239 (16px)
 *
 * Controls:
 *   Up/Down/Left/Right → queue movement direction
 *   A → start / restart
 *   B → pause / unpause
 */

#ifndef PACMAN_GAME_H
#define PACMAN_GAME_H

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

// ── Screen geometry ───────────────────────────────────────────────────────────
#define SCREEN_W    320
#define SCREEN_H    240

// ── Maze geometry ─────────────────────────────────────────────────────────────
#define TILE_SIZE   8           // pixels per tile
#define MAZE_COLS   28
#define MAZE_ROWS   28
#define MAZE_X      48          // left edge of maze on screen
#define MAZE_Y      0           // top edge of maze on screen
#define MAZE_PX_W   (MAZE_COLS * TILE_SIZE)   // 224
#define MAZE_PX_H   (MAZE_ROWS * TILE_SIZE)   // 224
#define HUD_Y       224
#define HUD_H       16

// ── RGB565 colours ────────────────────────────────────────────────────────────
#define COLOR_BLACK    0x0000
#define COLOR_WHITE    0xFFFF
#define COLOR_RED      0xF800
#define COLOR_GREEN    0x07E0
#define COLOR_BLUE     0x001F
#define COLOR_YELLOW   0xFFE0
#define COLOR_ORANGE   0xFD20
#define COLOR_CYAN     0x07FF
#define COLOR_PINK     0xF81F
#define COLOR_DKBLUE   0x000A   // dark blue for frightened ghosts
#define COLOR_MAZE     0x001F   // wall colour (blue)
#define COLOR_PANEL    0x0000   // side panel background

// ── Public interface ──────────────────────────────────────────────────────────
esp_err_t pacman_init(void);
void      pacman_game_loop(void);
void      pacman_reset(void);

#endif // PACMAN_GAME_H
