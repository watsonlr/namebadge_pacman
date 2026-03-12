/**
 * @file frogger_game.c
 * @brief Classic Frogger for BYUI e-Badge V3.0
 *
 * Layout (landscape 320×240):
 *   Row  0        : Home bases (5 slots, walls between)
 *   Rows 1–5      : River  – logs / turtles; frog must ride one
 *   Row  6        : Grass median (always safe)
 *   Rows 7–11     : Road   – cars / trucks; frog must dodge
 *   Rows 12–13    : Grass start zone (always safe)
 *   Y 224–239     : HUD strip (score, lives)
 *
 * Movement objects use fixed-point positions (1/16 pixel) for smooth motion.
 * Each game tick all lane objects advance by their lane's speed_fp, wrapping
 * when they leave the screen.  The frog hops one full cell per button press.
 * When on a river row the frog drifts with its log; falling off kills it.
 */

#include "frogger_game.h"
#include "lcd_driver.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "frogger";

// ── Fixed-point ───────────────────────────────────────────────────────────────
// Positions and speeds in units of 1/16 pixel.
#define FP_SHIFT  4
#define FP_ONE    (1 << FP_SHIFT)   // 16 fp-units == 1 pixel

// ── Layout helpers ────────────────────────────────────────────────────────────
#define GAME_W  (GAME_COLS * CELL)   // 320
#define GAME_H  (GAME_ROWS * CELL)   // 224

// Home slot left-edge x (pixels); each slot is HOME_W pixels wide.
static const int HOME_X[NUM_HOMES] = {
    1 * CELL,   //  16
    5 * CELL,   //  80
    9 * CELL,   // 144
    13 * CELL,  // 208
    17 * CELL,  // 272
};
#define HOME_W  (2 * CELL)   // 32 px

// ── Palette ───────────────────────────────────────────────────────────────────
#define COL_WATER      0x0318   // deep blue
#define COL_GRASS      0x02A0   // dark green
#define COL_ROAD       0x4208   // dark grey
#define COL_LOG        0x8200   // brown
#define COL_TURTLE     0x0360   // teal-green
#define COL_WALL       0x0140   // very dark green (home-row gaps)
#define COL_HOME_OPEN  0x0200   // near-black green (empty slot)
#define COL_HOME_FILL  0x07E0   // bright green (filled slot)
#define COL_CAR_RED    0xF800
#define COL_CAR_YEL    0xFFE0
#define COL_CAR_GRN    0x07E0
#define COL_TRUCK      0xFD20
#define COL_FROG       0xFFFF   // white — stands out on all backgrounds

// ── Lane data ─────────────────────────────────────────────────────────────────
#define MAX_OBJS  9

typedef struct {
    int32_t x_fp;   // left-edge x, fixed-point pixels
    int     w_px;   // width in pixels
} obj_t;

typedef enum {
    LTYPE_HOME,
    LTYPE_WATER,
    LTYPE_SAFE,
    LTYPE_ROAD,
} ltype_t;

typedef struct {
    ltype_t  type;
    int32_t  speed_fp;        // signed; negative = leftward
    int      obj_w_px;        // width of each object in pixels
    int      n_objs;
    obj_t    objs[MAX_OBJS];
    uint16_t obj_color;
    uint16_t bg_color;
    int32_t  dirty_fp;        // accumulated sub-pixel movement since last draw
} lane_t;

static lane_t g_lanes[GAME_ROWS];

// ── Game state ────────────────────────────────────────────────────────────────
typedef enum {
    ST_TITLE,
    ST_PLAYING,
    ST_PAUSED,
    ST_DYING,       // brief death flash before respawn
    ST_GAME_OVER,
    ST_WIN,         // all 5 homes filled; brief celebration then next round
} gstate_t;

static gstate_t  g_state;
static int32_t   g_frog_x_fp;   // frog left-edge x, fixed-point
static int       g_frog_row;
static int       g_lives;
static uint32_t  g_score;
static bool      g_homes[NUM_HOMES];
static int       g_die_timer;
static int       g_win_timer;
static bool      g_full_redraw;
static int       g_frog_min_row;  // highest row reached this life (for scoring)

// ── Button input ──────────────────────────────────────────────────────────────
#define DEBOUNCE_MS  100

static bool     btn_down[6];
static uint32_t btn_ms[6];

static const int BTN_GPIOS[6] = {
    BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_A, BTN_B
};

#define IDX_UP    0
#define IDX_DOWN  1
#define IDX_LEFT  2
#define IDX_RIGHT 3
#define IDX_A     4
#define IDX_B     5

static void init_buttons(void) {
    gpio_config_t c = {
        .pin_bit_mask = (1ULL << BTN_UP)    | (1ULL << BTN_DOWN)  |
                        (1ULL << BTN_LEFT)   | (1ULL << BTN_RIGHT) |
                        (1ULL << BTN_A)      | (1ULL << BTN_B),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&c);
}

static bool btn_edge(int i) {
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (gpio_get_level(BTN_GPIOS[i]) == 0) {
        if (!btn_down[i] && (now - btn_ms[i]) > DEBOUNCE_MS) {
            btn_down[i] = true;
            btn_ms[i]   = now;
            return true;
        }
    } else {
        btn_down[i] = false;
    }
    return false;
}

// ── Lane helpers ──────────────────────────────────────────────────────────────

// Populate a lane with evenly-spaced objects starting just off-screen.
static void setup_lane(lane_t *l, ltype_t type, int32_t speed_fp,
                       int obj_w_cells, int gap_cells,
                       uint16_t obj_color, uint16_t bg_color)
{
    l->type      = type;
    l->speed_fp  = speed_fp;
    l->obj_w_px  = obj_w_cells * CELL;
    l->obj_color = obj_color;
    l->bg_color  = bg_color;

    int gap_px  = gap_cells * CELL;
    int period  = l->obj_w_px + gap_px;
    int n       = (GAME_W + l->obj_w_px) / period + 1;
    if (n > MAX_OBJS) n = MAX_OBJS;
    l->n_objs = n;

    for (int i = 0; i < n; i++) {
        l->objs[i].x_fp = (int32_t)(i * period) * FP_ONE;
        l->objs[i].w_px = l->obj_w_px;
    }
}

static void init_lanes(void) {
    // Row 0: home — rendered specially; no moving objects
    memset(&g_lanes[0], 0, sizeof(lane_t));
    g_lanes[0].type     = LTYPE_HOME;
    g_lanes[0].bg_color = COL_WALL;

    // Row 1: logs right, slow  (3-cell wide, 4-cell gap)
    setup_lane(&g_lanes[1],  LTYPE_WATER,  8,  3, 4, COL_LOG,    COL_WATER);
    // Row 2: logs left, medium (2-cell wide, 3-cell gap)
    setup_lane(&g_lanes[2],  LTYPE_WATER, -10, 2, 3, COL_LOG,    COL_WATER);
    // Row 3: turtles right, slow (2-cell wide, 3-cell gap)
    setup_lane(&g_lanes[3],  LTYPE_WATER,  6,  2, 3, COL_TURTLE, COL_WATER);
    // Row 4: logs left, fast (4-cell wide, 3-cell gap)
    setup_lane(&g_lanes[4],  LTYPE_WATER, -14, 4, 3, COL_LOG,    COL_WATER);
    // Row 5: logs right, medium (3-cell wide, 5-cell gap)
    setup_lane(&g_lanes[5],  LTYPE_WATER,  10, 3, 5, COL_LOG,    COL_WATER);

    // Row 6: grass median — safe, static
    memset(&g_lanes[6], 0, sizeof(lane_t));
    g_lanes[6].type     = LTYPE_SAFE;
    g_lanes[6].bg_color = COL_GRASS;

    // Row  7: cars left, fast  (1-cell, 3-cell gap)
    setup_lane(&g_lanes[7],  LTYPE_ROAD, -14, 1, 3, COL_CAR_RED, COL_ROAD);
    // Row  8: cars right, medium (1-cell, 4-cell gap)
    setup_lane(&g_lanes[8],  LTYPE_ROAD,  10, 1, 4, COL_CAR_YEL, COL_ROAD);
    // Row  9: trucks left, slow (3-cell, 4-cell gap)
    setup_lane(&g_lanes[9],  LTYPE_ROAD,  -8, 3, 4, COL_TRUCK,   COL_ROAD);
    // Row 10: cars right, fast (1-cell, 3-cell gap)
    setup_lane(&g_lanes[10], LTYPE_ROAD,  12, 1, 3, COL_CAR_GRN, COL_ROAD);
    // Row 11: cars left, medium (2-cell, 5-cell gap)
    setup_lane(&g_lanes[11], LTYPE_ROAD, -10, 2, 5, COL_CAR_RED, COL_ROAD);

    // Rows 12–13: safe start grass
    memset(&g_lanes[12], 0, sizeof(lane_t));
    g_lanes[12].type     = LTYPE_SAFE;
    g_lanes[12].bg_color = COL_GRASS;
    memset(&g_lanes[13], 0, sizeof(lane_t));
    g_lanes[13].type     = LTYPE_SAFE;
    g_lanes[13].bg_color = COL_GRASS;
}

static void update_lanes(void) {
    for (int r = 0; r < GAME_ROWS; r++) {
        lane_t *l = &g_lanes[r];
        if (l->speed_fp == 0 || l->n_objs == 0) continue;
        l->dirty_fp += l->speed_fp;
        for (int i = 0; i < l->n_objs; i++) {
            l->objs[i].x_fp += l->speed_fp;
            int x = l->objs[i].x_fp >> FP_SHIFT;
            if (l->speed_fp > 0) {
                if (x >= GAME_W)
                    l->objs[i].x_fp -= (int32_t)(GAME_W + l->obj_w_px) * FP_ONE;
            } else {
                if (x + l->obj_w_px <= 0)
                    l->objs[i].x_fp += (int32_t)(GAME_W + l->obj_w_px) * FP_ONE;
            }
        }
    }
}

// ── Frog helpers ──────────────────────────────────────────────────────────────

static void respawn_frog(void) {
    g_frog_row     = ROW_START;
    g_frog_x_fp    = (int32_t)(GAME_COLS / 2) * CELL * FP_ONE;
    g_frog_min_row = ROW_START;
}

static void kill_frog(void) {
    g_lives--;
    ESP_LOGI(TAG, "Frog died! Lives left: %d", g_lives);
    if (g_lives <= 0) {
        g_state       = ST_GAME_OVER;
        g_full_redraw = true;
    } else {
        g_state      = ST_DYING;
        g_die_timer  = 45;   // ~0.75 s at 60 fps
    }
}

// True if the frog's centre pixel lands on any object in the current water lane.
static bool on_log(void) {
    lane_t *l  = &g_lanes[g_frog_row];
    int     cx = (g_frog_x_fp >> FP_SHIFT) + CELL / 2;
    for (int i = 0; i < l->n_objs; i++) {
        int ox = l->objs[i].x_fp >> FP_SHIFT;
        if (cx >= ox && cx < ox + l->obj_w_px) return true;
    }
    return false;
}

// True if the frog's bounding box (1-pixel inset) overlaps any car in the lane.
static bool car_hit(void) {
    lane_t *l    = &g_lanes[g_frog_row];
    int     fx_l = (g_frog_x_fp >> FP_SHIFT) + 2;
    int     fx_r = fx_l + CELL - 4;
    for (int i = 0; i < l->n_objs; i++) {
        int ox = l->objs[i].x_fp >> FP_SHIFT;
        int ow = l->obj_w_px;
        if (fx_l < ox + ow && fx_r > ox) return true;
    }
    return false;
}

// Return which home slot the frog centre is in, or -1 if between slots.
static int get_home_idx(void) {
    int cx = (g_frog_x_fp >> FP_SHIFT) + CELL / 2;
    for (int i = 0; i < NUM_HOMES; i++) {
        if (cx >= HOME_X[i] && cx < HOME_X[i] + HOME_W) return i;
    }
    return -1;
}

// ── Hop logic ─────────────────────────────────────────────────────────────────

static void do_hop(int dx, int dy) {
    int     new_row  = g_frog_row + dy;
    int32_t new_x_fp = g_frog_x_fp + (int32_t)dx * CELL * FP_ONE;

    // Clamp / die on vertical edges
    if (new_row < 0)           new_row = 0;
    if (new_row >= GAME_ROWS)  new_row = GAME_ROWS - 1;

    // Die if hopping horizontally off-screen
    int new_x = new_x_fp >> FP_SHIFT;
    if (new_x < 0 || new_x + CELL > GAME_W) {
        kill_frog();
        return;
    }

    g_frog_x_fp = new_x_fp;
    g_frog_row  = new_row;

    // Score for each new row reached northward
    if (g_frog_row < g_frog_min_row) {
        g_frog_min_row = g_frog_row;
        g_score += 10;
    }

    // Handle arrival at the home row
    if (g_frog_row == ROW_HOME) {
        int h = get_home_idx();
        if (h < 0 || g_homes[h]) {
            kill_frog();   // missed slot or slot already filled
        } else {
            g_homes[h] = true;
            g_score   += 50;
            ESP_LOGI(TAG, "Home %d filled! Score %lu", h, (unsigned long)g_score);
            respawn_frog();

            // Check for round clear
            bool all = true;
            for (int i = 0; i < NUM_HOMES; i++) if (!g_homes[i]) { all = false; break; }
            if (all) {
                g_state     = ST_WIN;
                g_win_timer = 120;
            }
        }
    }
}

// ── Input ─────────────────────────────────────────────────────────────────────

static void handle_input(void) {
    switch (g_state) {
        case ST_TITLE:
        case ST_GAME_OVER:
            if (btn_edge(IDX_A) || btn_edge(IDX_B)) frogger_reset();
            return;

        case ST_PAUSED:
            if (btn_edge(IDX_B)) { g_state = ST_PLAYING; g_full_redraw = true; }
            if (btn_edge(IDX_A)) frogger_reset();
            return;

        case ST_DYING:
        case ST_WIN:
            return;   // no player input during animations

        case ST_PLAYING:
            break;
    }

    if (btn_edge(IDX_UP))    do_hop( 0, -1);
    if (btn_edge(IDX_DOWN))  do_hop( 0,  1);
    if (btn_edge(IDX_LEFT))  do_hop(-1,  0);
    if (btn_edge(IDX_RIGHT)) do_hop( 1,  0);
    if (btn_edge(IDX_B))     { g_state = ST_PAUSED; g_full_redraw = true; }
}

// ── Update ────────────────────────────────────────────────────────────────────

static void update(void) {
    switch (g_state) {
        case ST_DYING:
            if (--g_die_timer <= 0) {
                g_state       = ST_PLAYING;
                g_full_redraw = true;
                respawn_frog();
            }
            return;

        case ST_WIN:
            if (--g_win_timer <= 0) {
                // Next round: clear homes, slightly increase speed, respawn
                memset(g_homes, 0, sizeof(g_homes));
                for (int r = 0; r < GAME_ROWS; r++) {
                    if (g_lanes[r].speed_fp > 0) g_lanes[r].speed_fp += 1;
                    if (g_lanes[r].speed_fp < 0) g_lanes[r].speed_fp -= 1;
                }
                respawn_frog();
                g_state       = ST_PLAYING;
                g_full_redraw = true;
            }
            update_lanes();
            return;

        case ST_PLAYING:
            break;

        default:
            return;
    }

    update_lanes();

    // River: frog must be on a log; if yes, drift with it
    if (g_frog_row >= ROW_WATER_TOP && g_frog_row <= ROW_WATER_BOT) {
        if (!on_log()) {
            kill_frog();
            return;
        }
        g_frog_x_fp += g_lanes[g_frog_row].speed_fp;
        int fx = g_frog_x_fp >> FP_SHIFT;
        if (fx < 0 || fx + CELL > GAME_W) {
            kill_frog();
            return;
        }
    }

    // Road: frog must dodge cars
    if (g_frog_row >= ROW_ROAD_TOP && g_frog_row <= ROW_ROAD_BOT) {
        if (car_hit()) {
            kill_frog();
        }
    }
}

// ── Rendering ─────────────────────────────────────────────────────────────────

static void draw_lane_row(int row) {
    lane_t *l  = &g_lanes[row];
    int     y  = row * CELL;

    // Row background
    lcd_fill_rect(0, y, GAME_W, CELL, l->bg_color);

    // Objects (clip to screen edges for wrapping objects)
    for (int i = 0; i < l->n_objs; i++) {
        int x = l->objs[i].x_fp >> FP_SHIFT;
        int w = l->obj_w_px;
        if (x + w <= 0 || x >= GAME_W) continue;
        int cx = x, cw = w;
        if (cx < 0)           { cw += cx; cx = 0; }
        if (cx + cw > GAME_W) { cw = GAME_W - cx; }
        lcd_fill_rect(cx, y, cw, CELL, l->obj_color);
    }
}

static void draw_home_row(void) {
    // Fill row with wall colour, then punch out home slots
    lcd_fill_rect(0, 0, GAME_W, CELL, COL_WALL);
    for (int i = 0; i < NUM_HOMES; i++) {
        uint16_t col = g_homes[i] ? COL_HOME_FILL : COL_HOME_OPEN;
        lcd_fill_rect(HOME_X[i], 0, HOME_W, CELL, col);
        lcd_draw_rect_outline(HOME_X[i], 0, HOME_W, CELL, COLOR_WHITE);
    }
}

static void draw_frog(uint16_t color) {
    int x = g_frog_x_fp >> FP_SHIFT;
    int y = g_frog_row * CELL;
    lcd_fill_rect(x + 2, y + 2, CELL - 4, CELL - 4, color);
}

static void draw_hud(void) {
    lcd_fill_rect(0, HUD_Y, SCREEN_W, HUD_H, COLOR_BLACK);
    char buf[48];
    snprintf(buf, sizeof(buf), "SCORE: %lu   LIVES: %d", (unsigned long)g_score, g_lives);
    lcd_draw_string(4, HUD_Y + 4, buf, COLOR_WHITE, COLOR_BLACK);
}

static void draw_safe_rows(void) {
    for (int r = 0; r < GAME_ROWS; r++) {
        if (g_lanes[r].type == LTYPE_SAFE)
            lcd_fill_rect(0, r * CELL, GAME_W, CELL, g_lanes[r].bg_color);
    }
}

static void render(void) {
    static uint32_t prev_score    = UINT32_MAX;
    static int      prev_lives    = -1;
    static int      die_flash     = 0;
    static int32_t  prev_frog_x   = -1;
    static int      prev_frog_row = -1;

    switch (g_state) {
        case ST_TITLE:
            if (g_full_redraw) {
                lcd_fill_screen(COLOR_BLACK);
                lcd_draw_string(104, 88,  "FROGGER",          COLOR_GREEN,  COLOR_BLACK);
                lcd_draw_string(72,  112, "A or B  to start", COLOR_WHITE,  COLOR_BLACK);
                lcd_draw_string(72,  130, "UP/DN/LT/RT : hop", COLOR_WHITE, COLOR_BLACK);
                lcd_draw_string(72,  148, "B : pause",         COLOR_WHITE, COLOR_BLACK);
                g_full_redraw = false;
            }
            return;

        case ST_GAME_OVER:
            if (g_full_redraw) {
                lcd_fill_screen(COLOR_BLACK);
                lcd_draw_string(104, 84, "GAME OVER", COLOR_RED, COLOR_BLACK);
                char buf[32];
                snprintf(buf, sizeof(buf), "SCORE: %lu", (unsigned long)g_score);
                lcd_draw_string(96, 108, buf, COLOR_WHITE, COLOR_BLACK);
                lcd_draw_string(64, 132, "A or B to restart", COLOR_WHITE, COLOR_BLACK);
                g_full_redraw = false;
            }
            return;

        case ST_DYING:
            die_flash++;
            draw_frog((die_flash & 4) ? COLOR_RED : g_lanes[g_frog_row].bg_color);
            return;

        case ST_PAUSED:
            if (!g_full_redraw) return;   // screen is stable; nothing to update
            /* fall through */

        case ST_WIN:
        case ST_PLAYING:
            break;
    }

    bool force_draw = g_full_redraw;   // capture before clearing below
    if (g_full_redraw) {
        draw_safe_rows();
        prev_score    = UINT32_MAX;
        prev_lives    = -1;
        die_flash     = 0;
        prev_frog_row = -1;   // invalidate so erase logic doesn't run stale data
        prev_frog_x   = -1;
        g_full_redraw = false;
    }

    // Dynamic rows (water + road): skip lanes that haven't moved a full pixel
    // yet.  On a full-redraw (force_draw) always paint regardless of dirty state
    // so the initial scene is correct.
    for (int r = ROW_WATER_TOP; r <= ROW_ROAD_BOT; r++) {
        lane_t *l = &g_lanes[r];
        if (l->type == LTYPE_SAFE) continue;
        int32_t dirty = (l->dirty_fp < 0) ? -l->dirty_fp : l->dirty_fp;
        if (!force_draw && dirty < FP_ONE) continue;
        draw_lane_row(r);
        l->dirty_fp = 0;
    }

    // Home row
    draw_home_row();

    // Erase frog ghost when the frog has moved.
    // Safe rows are never redrawn by the lane loop, so always need manual erase.
    // Dynamic rows self-erase only when their lane was redrawn this frame;
    // if the lane was skipped (dirty threshold not reached) we erase manually.
    if (prev_frog_row >= 0 &&
        (prev_frog_row != g_frog_row || prev_frog_x != (g_frog_x_fp >> FP_SHIFT))) {
        lane_t *pl = &g_lanes[prev_frog_row];
        bool lane_was_redrawn = force_draw ||
                                (pl->type != LTYPE_SAFE && pl->dirty_fp == 0);
        if (!lane_was_redrawn) {
            int ex = prev_frog_x;
            int ey = prev_frog_row * CELL;
            lcd_fill_rect(ex + 2, ey + 2, CELL - 4, CELL - 4, pl->bg_color);
        }
    }

    // Frog
    if (g_state == ST_PLAYING || g_state == ST_WIN || g_state == ST_PAUSED) {
        draw_frog(COL_FROG);
        prev_frog_row = g_frog_row;
        prev_frog_x   = g_frog_x_fp >> FP_SHIFT;
    }

    // Overlays
    if (g_state == ST_PAUSED) {
        lcd_fill_rect(80, 86, 160, 68, COLOR_BLACK);
        lcd_draw_rect_outline(80, 86, 160, 68, COLOR_YELLOW);
        lcd_draw_string(112, 98,  "PAUSED",         COLOR_YELLOW, COLOR_BLACK);
        lcd_draw_string(88,  116, "B : resume",     COLOR_WHITE,  COLOR_BLACK);
        lcd_draw_string(88,  130, "A : restart",    COLOR_WHITE,  COLOR_BLACK);
    }

    if (g_state == ST_WIN) {
        lcd_fill_rect(72, 102, 176, 36, COLOR_BLACK);
        lcd_draw_rect_outline(72, 102, 176, 36, COLOR_YELLOW);
        lcd_draw_string(88, 114, "LEVEL CLEAR! +200", COLOR_YELLOW, COLOR_BLACK);
    }

    // HUD strip — only repaint when values change
    if (g_score != prev_score || g_lives != prev_lives) {
        draw_hud();
        prev_score = g_score;
        prev_lives = g_lives;
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

void frogger_reset(void) {
    ESP_LOGI(TAG, "Resetting Frogger");
    init_lanes();
    g_lives       = MAX_LIVES;
    g_score       = 0;
    g_die_timer   = 0;
    g_win_timer   = 0;
    g_full_redraw = true;
    memset(g_homes, 0, sizeof(g_homes));
    // Sync btn_down to actual hardware state and reset debounce timers.
    // Reading the GPIO here prevents a held button (e.g. B used to start the
    // game) from firing again immediately as "newly pressed" on the first tick.
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    for (int i = 0; i < 6; i++) {
        btn_ms[i]   = now;
        btn_down[i] = (gpio_get_level(BTN_GPIOS[i]) == 0);
    }
    respawn_frog();
    g_state = ST_PLAYING;
}

esp_err_t frogger_init(void) {
    ESP_LOGI(TAG, "Initialising Frogger");
    ESP_ERROR_CHECK(lcd_init());
    init_buttons();
    memset(btn_down, 0, sizeof(btn_down));
    memset(btn_ms,   0, sizeof(btn_ms));
    frogger_reset();
    g_state = ST_TITLE;
    g_full_redraw = true;
    return ESP_OK;
}

void frogger_game_loop(void) {
    handle_input();
    update();
    render();
}
