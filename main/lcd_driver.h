/**
 * @file lcd_driver.h
 * @brief ILI9341 LCD display driver - namebadge_tetris
 * Hardware: BYUI e-Badge V3.0 (ESP32-S3-Mini-1-N4R2)
 * Display:  ILI9341 240x320 native, mounted landscape (320x240)
 * MADCTL:   0x40 (MX=1) — landscape, FPC connector on left
 * RGB565:   big-endian over SPI (high byte first)
 */

#ifndef LCD_DRIVER_H
#define LCD_DRIVER_H

#include <stdint.h>
#include "driver/spi_master.h"
#include "esp_err.h"

// LCD dimensions (landscape)
#define LCD_WIDTH  320
#define LCD_HEIGHT 240

// Initialize LCD
esp_err_t lcd_init(void);

// Drawing primitives
void lcd_fill_screen(uint16_t color);
void lcd_draw_pixel(int x, int y, uint16_t color);
void lcd_fill_rect(int x, int y, int w, int h, uint16_t color);
void lcd_draw_rect_outline(int x, int y, int w, int h, uint16_t color);
void lcd_draw_string(int x, int y, const char *str, uint16_t color, uint16_t bg);
void lcd_draw_string_2x(int x, int y, const char *str, uint16_t color, uint16_t bg);

// Low-level access
void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

#endif // LCD_DRIVER_H
