/**
 * @file pacman_game.c
 * @brief Classic Pac-Man — BYUI e-Badge V3.0
 *
 * Maze: 28×28 tiles, 8×8 px each, origin at screen (48, 0).
 * Movement: tile-based (one step per MOVE_INTERVAL frames).
 * Ghost AI: Blinky chases directly; Pinky targets 4 ahead; Inky/Clyde scatter.
 */

#include "pacman_game.h"
#include "lcd_driver.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "pacman";

// ── Tile types ────────────────────────────────────────────────────────────────
#define TILE_WALL   0
#define TILE_DOT    1
#define TILE_PELLET 2
#define TILE_EMPTY  3
#define TILE_DOOR   4   // ghost-house door — passable by ghosts, not Pac-Man

// ── Direction helpers ─────────────────────────────────────────────────────────
typedef enum { DIR_NONE=0, DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT } Dir;

static const int DIR_DC[5] = { 0,  0,  0, -1, +1 };  // col delta
static const int DIR_DR[5] = { 0, -1, +1,  0,  0 };  // row delta

static Dir dir_opposite(Dir d) {
    if (d == DIR_UP)    return DIR_DOWN;
    if (d == DIR_DOWN)  return DIR_UP;
    if (d == DIR_LEFT)  return DIR_RIGHT;
    if (d == DIR_RIGHT) return DIR_LEFT;
    return DIR_NONE;
}

// ── Ghost modes ───────────────────────────────────────────────────────────────
typedef enum {
    GMODE_HOUSE = 0,    // waiting in ghost house
    GMODE_CHASE,        // chasing Pac-Man
    GMODE_SCATTER,      // retreating to corner
    GMODE_FRIGHTENED,   // vulnerable (blue)
    GMODE_EATEN,        // returning to house (eyes only)
} GhostMode;

// ── Game states ───────────────────────────────────────────────────────────────
typedef enum {
    ST_TITLE = 0,
    ST_READY,
    ST_PLAYING,
    ST_DYING,
    ST_LEVEL_COMPLETE,
    ST_GAME_OVER,
} GameState;

// ── Timing constants ──────────────────────────────────────────────────────────
#define PAC_MOVE_INTERVAL    4    // frames per Pac-Man step (60/4 = 15 cells/s)
#define GHOST_MOVE_INTERVAL  6    // frames per ghost step
#define EATEN_MOVE_INTERVAL  3    // eaten ghosts move faster back to house
#define FRIGHTENED_TICKS     300  // ~5 seconds of frightened mode
#define SCATTER_TICKS        420  // ~7 seconds scatter
#define CHASE_TICKS          1200 // ~20 seconds chase
#define READY_TICKS          180  // 3 seconds READY! screen
#define DYING_TICKS          90   // 1.5 seconds death animation
#define LEVEL_COMPLETE_TICKS 120  // 2 seconds level complete flash

// ── Maze definition ───────────────────────────────────────────────────────────
// 28 columns × 28 rows.
// '#' = wall, '.' = dot, 'o' = power pellet, ' ' = empty, '-' = ghost door
static const char maze_template[MAZE_ROWS][MAZE_COLS + 1] = {
    "############################",  //  0
    "#............##............#",  //  1
    "#.####.#####.##.#####.####.#",  //  2
    "#o####.#####.##.#####.####o#",  //  3
    "#.####.#####.##.#####.####.#",  //  4
    "#..........................#",  //  5
    "#.####.##.########.##.####.#",  //  6
    "#.####.##.########.##.####.#",  //  7
    "#......##....##....##......#",  //  8
    "######.#####.##.#####.######",  //  9
    "     #.#####.##.#####.#     ",  // 10
    "     #.##          ##.#     ",  // 11
    "     #.## ###--### ##.#     ",  // 12
    "######.## #      # ##.######",  // 13
    "          #      #          ",  // 14
    "######.## #      # ##.######",  // 15
    "     #.## ######## ##.#     ",  // 16
    "     #.##          ##.#     ",  // 17
    "     #.## ######## ##.#     ",  // 18
    "######.## ######## ##.######",  // 19
    "#............##............#",  // 20
    "#.####.#####.##.#####.####.#",  // 21
    "#o..##................##..o#",  // 22
    "###.##.##.###  ###.##.##.###",  // 23  ← Pac-Man starts here col 13
    "#......##....##....##......#",  // 24
    "#.##########.##.##########.#",  // 25
    "#..........................#",  // 26
    "############################",  // 27
};

// Runtime tile map (modified as dots are eaten)
static uint8_t maze[MAZE_ROWS][MAZE_COLS];

// ── Structs ───────────────────────────────────────────────────────────────────
typedef struct {
    int col, row;
    Dir dir;
    Dir next_dir;
    int move_timer;
    int anim_frame;     // 0 = mouth open, 1 = mouth half, 2 = mouth closed
} PacState;

typedef struct {
    int col, row;
    Dir dir;
    GhostMode mode;
    uint16_t color;
    int scatter_col, scatter_row;   // corner target for scatter mode
    int move_timer;
    int house_timer;                // frames before leaving house
    int id;
} Ghost;

// ── Game state ────────────────────────────────────────────────────────────────
static GameState  g_state;
static PacState   pac;
static Ghost      ghosts[4];
static int        g_score;
static int        g_hiscore;
static int        g_lives;
static int        g_level;
static int        g_dots_left;
static int        g_total_dots;
static int        g_frightened_timer;
static int        g_mode_timer;         // scatter/chase toggle timer
static bool       g_scatter_phase;      // true = scatter, false = chase
static int        g_state_timer;        // generic state countdown
static int        g_ghost_eat_combo;    // 1/2/4/8 multiplier per power pellet
static bool       g_need_full_redraw;

// Button state
static bool       btn_state[6];         // current debounced state (true=pressed)
static bool       btn_prev[6];
static bool       btn_edge[6];          // true on press edge this frame
static const int  BTN_GPIOS[6] = { BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_A, BTN_B };

// ── Button indices ────────────────────────────────────────────────────────────
#define BI_UP    0
#define BI_DOWN  1
#define BI_LEFT  2
#define BI_RIGHT 3
#define BI_A     4
#define BI_B     5

// ── Screen coordinate helpers ─────────────────────────────────────────────────
static inline int tile_px(int col) { return MAZE_X + col * TILE_SIZE; }
static inline int tile_py(int row) { return MAZE_Y + row * TILE_SIZE; }

// ── Maze helpers ──────────────────────────────────────────────────────────────
static void maze_init(void) {
    g_total_dots = 0;
    for (int r = 0; r < MAZE_ROWS; r++) {
        for (int c = 0; c < MAZE_COLS; c++) {
            char ch = maze_template[r][c];
            if      (ch == '#') maze[r][c] = TILE_WALL;
            else if (ch == '.') { maze[r][c] = TILE_DOT;    g_total_dots++; }
            else if (ch == 'o') { maze[r][c] = TILE_PELLET; g_total_dots++; }
            else if (ch == '-') maze[r][c] = TILE_DOOR;
            else                maze[r][c] = TILE_EMPTY;
        }
    }
    g_dots_left = g_total_dots;
}

static bool in_bounds(int col, int row) {
    return col >= 0 && col < MAZE_COLS && row >= 0 && row < MAZE_ROWS;
}

// Can Pac-Man move to (col,row)?
static bool pac_can_enter(int col, int row) {
    if (!in_bounds(col, row)) return false;
    uint8_t t = maze[row][col];
    return t != TILE_WALL && t != TILE_DOOR;
}

// Can a ghost move to (col,row)?
static bool ghost_can_enter(int col, int row, GhostMode mode) {
    if (!in_bounds(col, row)) return false;
    uint8_t t = maze[row][col];
    if (t == TILE_WALL) return false;
    // Only eaten/house ghosts may pass through the door
    if (t == TILE_DOOR && mode != GMODE_EATEN && mode != GMODE_HOUSE) return false;
    return true;
}

// ── Drawing helpers ───────────────────────────────────────────────────────────

// Draw a single maze tile at grid position (col, row)
static void draw_tile(int col, int row) {
    int px = tile_px(col);
    int py = tile_py(row);
    uint8_t t = maze[row][col];

    if (t == TILE_WALL) {
        lcd_fill_rect(px, py, TILE_SIZE, TILE_SIZE, COLOR_MAZE);
    } else if (t == TILE_DOT) {
        lcd_fill_rect(px, py, TILE_SIZE, TILE_SIZE, COLOR_BLACK);
        // Small 2×2 dot centered in tile
        lcd_fill_rect(px + 3, py + 3, 2, 2, COLOR_WHITE);
    } else if (t == TILE_PELLET) {
        lcd_fill_rect(px, py, TILE_SIZE, TILE_SIZE, COLOR_BLACK);
        // 4×4 pellet centered in tile
        lcd_fill_rect(px + 2, py + 2, 4, 4, COLOR_YELLOW);
    } else if (t == TILE_DOOR) {
        lcd_fill_rect(px, py, TILE_SIZE, TILE_SIZE, COLOR_BLACK);
        // Draw door as a thin pink line across the center
        lcd_fill_rect(px, py + 3, TILE_SIZE, 2, COLOR_PINK);
    } else {
        // TILE_EMPTY — blank
        lcd_fill_rect(px, py, TILE_SIZE, TILE_SIZE, COLOR_BLACK);
    }
}

// Draw the complete maze (walls + all remaining dots/pellets)
static void draw_maze(void) {
    for (int r = 0; r < MAZE_ROWS; r++) {
        for (int c = 0; c < MAZE_COLS; c++) {
            draw_tile(c, r);
        }
    }
    // Fill side panels
    lcd_fill_rect(0,      0, MAZE_X,                      SCREEN_H, COLOR_BLACK);
    lcd_fill_rect(MAZE_X + MAZE_PX_W, 0, SCREEN_W - (MAZE_X + MAZE_PX_W), SCREEN_H, COLOR_BLACK);
}

// Pac-Man sprite: 8×8, facing given direction, anim_frame 0=open, 1=closed
static void draw_pacman(int col, int row, Dir dir, int anim_frame) {
    int px = tile_px(col);
    int py = tile_py(row);
    // Clear tile first (erase background)
    lcd_fill_rect(px, py, TILE_SIZE, TILE_SIZE, COLOR_BLACK);

    // Draw body as a circle approximation (yellow)
    // Rows 0,7: cols 1-6
    lcd_fill_rect(px + 1, py + 0, 6, 1, COLOR_YELLOW);
    lcd_fill_rect(px + 0, py + 1, 8, 6, COLOR_YELLOW);
    lcd_fill_rect(px + 1, py + 7, 6, 1, COLOR_YELLOW);

    if (anim_frame == 0) {
        // Mouth open — cut a wedge in the direction of travel
        switch (dir) {
            case DIR_RIGHT:
                lcd_fill_rect(px + 4, py + 2, 4, 1, COLOR_BLACK);
                lcd_fill_rect(px + 3, py + 3, 5, 2, COLOR_BLACK);
                lcd_fill_rect(px + 4, py + 5, 4, 1, COLOR_BLACK);
                break;
            case DIR_LEFT:
                lcd_fill_rect(px + 0, py + 2, 4, 1, COLOR_BLACK);
                lcd_fill_rect(px + 0, py + 3, 5, 2, COLOR_BLACK);
                lcd_fill_rect(px + 0, py + 5, 4, 1, COLOR_BLACK);
                break;
            case DIR_UP:
                lcd_fill_rect(px + 2, py + 0, 1, 4, COLOR_BLACK);
                lcd_fill_rect(px + 3, py + 0, 2, 3, COLOR_BLACK);
                lcd_fill_rect(px + 5, py + 0, 1, 4, COLOR_BLACK);
                break;
            case DIR_DOWN:
                lcd_fill_rect(px + 2, py + 4, 1, 4, COLOR_BLACK);
                lcd_fill_rect(px + 3, py + 5, 2, 3, COLOR_BLACK);
                lcd_fill_rect(px + 5, py + 4, 1, 4, COLOR_BLACK);
                break;
            default:
                lcd_fill_rect(px + 4, py + 3, 4, 2, COLOR_BLACK); // default right
                break;
        }
    }
    // anim_frame == 1: closed mouth — full circle already drawn
}

// Draw a ghost at grid position
static void draw_ghost(int col, int row, uint16_t body_color, bool frightened) {
    int px = tile_px(col);
    int py = tile_py(row);
    lcd_fill_rect(px, py, TILE_SIZE, TILE_SIZE, COLOR_BLACK);

    uint16_t bc = frightened ? COLOR_DKBLUE : body_color;

    // Dome
    lcd_fill_rect(px + 1, py + 0, 6, 1, bc);
    // Body
    lcd_fill_rect(px + 0, py + 1, 8, 5, bc);
    // Feet (3 bumps at bottom)
    lcd_fill_rect(px + 0, py + 6, 2, 2, bc);
    lcd_fill_rect(px + 3, py + 6, 2, 2, bc);
    lcd_fill_rect(px + 6, py + 6, 2, 2, bc);

    if (!frightened) {
        // White eye whites
        lcd_fill_rect(px + 1, py + 2, 2, 2, COLOR_WHITE);
        lcd_fill_rect(px + 5, py + 2, 2, 2, COLOR_WHITE);
        // Blue pupils
        lcd_fill_rect(px + 2, py + 2, 1, 2, COLOR_BLUE);
        lcd_fill_rect(px + 6, py + 2, 1, 2, COLOR_BLUE);
    } else {
        // Frightened face — white eyes + jagged "mouth"
        lcd_fill_rect(px + 1, py + 2, 2, 1, COLOR_WHITE);
        lcd_fill_rect(px + 5, py + 2, 2, 1, COLOR_WHITE);
        lcd_fill_rect(px + 1, py + 5, 1, 1, COLOR_WHITE);
        lcd_fill_rect(px + 3, py + 4, 1, 1, COLOR_WHITE);
        lcd_fill_rect(px + 5, py + 5, 1, 1, COLOR_WHITE);
    }
}

// Draw eaten ghost: just eyes drifting home
static void draw_eaten_ghost(int col, int row) {
    int px = tile_px(col);
    int py = tile_py(row);
    lcd_fill_rect(px, py, TILE_SIZE, TILE_SIZE, COLOR_BLACK);
    // White eye whites
    lcd_fill_rect(px + 1, py + 2, 2, 2, COLOR_WHITE);
    lcd_fill_rect(px + 5, py + 2, 2, 2, COLOR_WHITE);
    // Blue pupils
    lcd_fill_rect(px + 2, py + 2, 1, 2, COLOR_BLUE);
    lcd_fill_rect(px + 6, py + 2, 1, 2, COLOR_BLUE);
}

// Erase an entity by redrawing the tile beneath it
static void erase_entity(int col, int row) {
    if (in_bounds(col, row)) {
        draw_tile(col, row);
    }
}

// ── HUD ───────────────────────────────────────────────────────────────────────
static void draw_hud(void) {
    char buf[32];
    // Score on left panel
    lcd_fill_rect(0, 0, MAZE_X, MAZE_PX_H, COLOR_BLACK);
    lcd_draw_string(2, 4,  "SCORE",  COLOR_WHITE,  COLOR_BLACK);
    snprintf(buf, sizeof(buf), "%05d", g_score);
    lcd_draw_string(2, 14, buf, COLOR_YELLOW, COLOR_BLACK);

    lcd_draw_string(2, 40, "HI",     COLOR_WHITE,  COLOR_BLACK);
    snprintf(buf, sizeof(buf), "%05d", g_hiscore);
    lcd_draw_string(2, 50, buf, COLOR_CYAN,   COLOR_BLACK);

    // Lives on right panel
    lcd_fill_rect(MAZE_X + MAZE_PX_W, 0, SCREEN_W - (MAZE_X + MAZE_PX_W), MAZE_PX_H, COLOR_BLACK);
    lcd_draw_string(MAZE_X + MAZE_PX_W + 2, 4, "LIVES", COLOR_WHITE, COLOR_BLACK);
    for (int i = 0; i < g_lives && i < 5; i++) {
        int lx = MAZE_X + MAZE_PX_W + 4;
        int ly = 18 + i * 10;
        // Draw small Pac-Man icon (6×6 yellow)
        lcd_fill_rect(lx + 1, ly + 0, 4, 1, COLOR_YELLOW);
        lcd_fill_rect(lx + 0, ly + 1, 6, 4, COLOR_YELLOW);
        lcd_fill_rect(lx + 1, ly + 5, 4, 1, COLOR_YELLOW);
        lcd_fill_rect(lx + 3, ly + 2, 3, 2, COLOR_BLACK);
    }

    // Level
    snprintf(buf, sizeof(buf), "LVL %d", g_level);
    lcd_draw_string(MAZE_X + MAZE_PX_W + 2, 80, buf, COLOR_GREEN, COLOR_BLACK);

    // Bottom HUD bar
    lcd_fill_rect(0, HUD_Y, SCREEN_W, HUD_H, COLOR_BLACK);
    snprintf(buf, sizeof(buf), "SCORE:%05d  LIVES:%d  LVL:%d", g_score, g_lives, g_level);
    lcd_draw_string(4, HUD_Y + 4, buf, COLOR_WHITE, COLOR_BLACK);
}

// ── Title screen ──────────────────────────────────────────────────────────────
static void draw_title(void) {
    lcd_fill_screen(COLOR_BLACK);

    // Pac-Man in yellow, large-ish text
    lcd_draw_string( 80, 60,  "P A C - M A N",  COLOR_YELLOW, COLOR_BLACK);
    lcd_draw_string(100, 80,  "namebadge v1.0", COLOR_WHITE,  COLOR_BLACK);

    lcd_draw_string( 80, 120, "UP/DOWN/LEFT/RIGHT", COLOR_CYAN,  COLOR_BLACK);
    lcd_draw_string( 80, 132, "  to move",          COLOR_CYAN,  COLOR_BLACK);
    lcd_draw_string( 80, 148, "  A - start",        COLOR_GREEN, COLOR_BLACK);
    lcd_draw_string( 80, 160, "  B - pause",        COLOR_GREEN, COLOR_BLACK);

    // Blinking prompt (show always on title for simplicity)
    lcd_draw_string( 96, 190, "PRESS A TO PLAY", COLOR_YELLOW, COLOR_BLACK);

    // Draw ghost row for decoration
    int ghost_colors[4] = { COLOR_RED, COLOR_PINK, COLOR_CYAN, COLOR_ORANGE };
    for (int i = 0; i < 4; i++) {
        int gx = 80 + i * 40;
        int gy = 210;
        lcd_fill_rect(gx + 1, gy + 0, 6, 1, ghost_colors[i]);
        lcd_fill_rect(gx + 0, gy + 1, 8, 5, ghost_colors[i]);
        lcd_fill_rect(gx + 0, gy + 6, 2, 2, ghost_colors[i]);
        lcd_fill_rect(gx + 3, gy + 6, 2, 2, ghost_colors[i]);
        lcd_fill_rect(gx + 6, gy + 6, 2, 2, ghost_colors[i]);
        lcd_fill_rect(gx + 1, gy + 2, 2, 2, COLOR_WHITE);
        lcd_fill_rect(gx + 5, gy + 2, 2, 2, COLOR_WHITE);
    }
}

static void draw_ready_msg(void) {
    lcd_draw_string(120, 112, " READY! ", COLOR_YELLOW, COLOR_BLACK);
}

static void draw_game_over_msg(void) {
    lcd_draw_string(112, 104, " GAME OVER ", COLOR_RED, COLOR_BLACK);
    lcd_draw_string(104, 120, "  PRESS A   ", COLOR_WHITE, COLOR_BLACK);
}

// ── Ghost initialisation ──────────────────────────────────────────────────────
static void ghost_init_all(void) {
    // Blinky (red) — starts just above ghost house door
    ghosts[0].id          = 0;
    ghosts[0].col         = 13;
    ghosts[0].row         = 11;
    ghosts[0].dir         = DIR_LEFT;
    ghosts[0].mode        = GMODE_SCATTER;
    ghosts[0].color       = COLOR_RED;
    ghosts[0].scatter_col = 25;
    ghosts[0].scatter_row = 0;
    ghosts[0].move_timer  = 0;
    ghosts[0].house_timer = 0;

    // Pinky (pink) — starts in house center
    ghosts[1].id          = 1;
    ghosts[1].col         = 13;
    ghosts[1].row         = 14;
    ghosts[1].dir         = DIR_UP;
    ghosts[1].mode        = GMODE_HOUSE;
    ghosts[1].color       = COLOR_PINK;
    ghosts[1].scatter_col = 2;
    ghosts[1].scatter_row = 0;
    ghosts[1].move_timer  = 0;
    ghosts[1].house_timer = 60;   // exits after 1 second

    // Inky (cyan) — starts in house left
    ghosts[2].id          = 2;
    ghosts[2].col         = 12;
    ghosts[2].row         = 14;
    ghosts[2].dir         = DIR_DOWN;
    ghosts[2].mode        = GMODE_HOUSE;
    ghosts[2].color       = COLOR_CYAN;
    ghosts[2].scatter_col = 25;
    ghosts[2].scatter_row = 27;
    ghosts[2].move_timer  = 0;
    ghosts[2].house_timer = 120;  // exits after 2 seconds

    // Clyde (orange) — starts in house right
    ghosts[3].id          = 3;
    ghosts[3].col         = 14;
    ghosts[3].row         = 14;
    ghosts[3].dir         = DIR_UP;
    ghosts[3].mode        = GMODE_HOUSE;
    ghosts[3].color       = COLOR_ORANGE;
    ghosts[3].scatter_col = 2;
    ghosts[3].scatter_row = 27;
    ghosts[3].move_timer  = 0;
    ghosts[3].house_timer = 180;  // exits after 3 seconds
}

// ── Game reset ────────────────────────────────────────────────────────────────
static void game_reset_level(void) {
    maze_init();

    pac.col        = 13;
    pac.row        = 23;
    pac.dir        = DIR_NONE;
    pac.next_dir   = DIR_NONE;
    pac.move_timer = 0;
    pac.anim_frame = 0;

    ghost_init_all();

    g_frightened_timer = 0;
    g_mode_timer       = SCATTER_TICKS;
    g_scatter_phase    = true;
    g_ghost_eat_combo  = 1;
    g_need_full_redraw = true;
}

void pacman_reset(void) {
    g_score  = 0;
    g_lives  = 3;
    g_level  = 1;
    game_reset_level();
    g_state       = ST_READY;
    g_state_timer = READY_TICKS;
    g_need_full_redraw = true;
}

// ── Input ─────────────────────────────────────────────────────────────────────
static void input_init(void) {
    for (int i = 0; i < 6; i++) {
        gpio_reset_pin(BTN_GPIOS[i]);
        gpio_set_direction(BTN_GPIOS[i], GPIO_MODE_INPUT);
        gpio_set_pull_mode(BTN_GPIOS[i], GPIO_PULLUP_ONLY);
        btn_state[i] = false;
        btn_prev[i]  = false;
        btn_edge[i]  = false;
    }
}

static void handle_input(void) {
    for (int i = 0; i < 6; i++) {
        btn_prev[i]  = btn_state[i];
        btn_state[i] = (gpio_get_level(BTN_GPIOS[i]) == 0); // active low
        btn_edge[i]  = btn_state[i] && !btn_prev[i];
    }
}

// ── Ghost AI ──────────────────────────────────────────────────────────────────

static Dir ghost_choose_dir(Ghost *g, int target_col, int target_row) {
    Dir opposite = dir_opposite(g->dir);
    Dir best     = DIR_NONE;
    int best_dist = 0x7FFFFFFF;

    for (int d = DIR_UP; d <= DIR_RIGHT; d++) {
        if ((Dir)d == opposite) continue;
        int nc = g->col + DIR_DC[d];
        int nr = g->row + DIR_DR[d];
        if (!ghost_can_enter(nc, nr, g->mode)) continue;
        int dist = (nc - target_col) * (nc - target_col) +
                   (nr - target_row) * (nr - target_row);
        if (dist < best_dist) {
            best_dist = dist;
            best      = (Dir)d;
        }
    }
    // Fallback: if no direction found (dead end), allow reversal
    if (best == DIR_NONE) {
        int nc = g->col + DIR_DC[opposite];
        int nr = g->row + DIR_DR[opposite];
        if (ghost_can_enter(nc, nr, g->mode)) best = opposite;
    }
    return best;
}

static Dir ghost_choose_random(Ghost *g) {
    Dir opposite = dir_opposite(g->dir);
    Dir options[4];
    int n = 0;
    for (int d = DIR_UP; d <= DIR_RIGHT; d++) {
        if ((Dir)d == opposite) continue;
        int nc = g->col + DIR_DC[d];
        int nr = g->row + DIR_DR[d];
        if (ghost_can_enter(nc, nr, g->mode)) options[n++] = (Dir)d;
    }
    if (n == 0) return opposite;
    return options[rand() % n];
}

static void ghost_get_target(Ghost *g, int *tc, int *tr) {
    if (g->mode == GMODE_SCATTER || g->mode == GMODE_HOUSE) {
        *tc = g->scatter_col;
        *tr = g->scatter_row;
        return;
    }
    if (g->mode == GMODE_EATEN) {
        // Target: just above ghost door
        *tc = 13;
        *tr = 11;
        return;
    }
    // CHASE mode — each ghost has different target logic
    switch (g->id) {
        case 0: // Blinky: direct chase
            *tc = pac.col;
            *tr = pac.row;
            break;
        case 1: // Pinky: 4 tiles ahead of Pac-Man
            *tc = pac.col + DIR_DC[pac.dir] * 4;
            *tr = pac.row + DIR_DR[pac.dir] * 4;
            break;
        case 2: // Inky: 2 tiles ahead of Pac-Man + Blinky offset
            {
                int mid_c = pac.col + DIR_DC[pac.dir] * 2;
                int mid_r = pac.row + DIR_DR[pac.dir] * 2;
                *tc = mid_c + (mid_c - ghosts[0].col);
                *tr = mid_r + (mid_r - ghosts[0].row);
            }
            break;
        case 3: // Clyde: if far, chase; if close, scatter
            {
                int dc = g->col - pac.col;
                int dr = g->row - pac.row;
                if (dc * dc + dr * dr > 64) { // > 8 tiles away
                    *tc = pac.col;
                    *tr = pac.row;
                } else {
                    *tc = g->scatter_col;
                    *tr = g->scatter_row;
                }
            }
            break;
        default:
            *tc = pac.col;
            *tr = pac.row;
    }
    // Clamp target to maze bounds
    if (*tc < 0) *tc = 0;
    if (*tc >= MAZE_COLS) *tc = MAZE_COLS - 1;
    if (*tr < 0) *tr = 0;
    if (*tr >= MAZE_ROWS) *tr = MAZE_ROWS - 1;
}

static void move_ghost(Ghost *g) {
    int interval = (g->mode == GMODE_EATEN) ? EATEN_MOVE_INTERVAL : GHOST_MOVE_INTERVAL;

    if (g->mode == GMODE_HOUSE) {
        // Bounce up/down in house
        if (g->house_timer > 0) {
            g->house_timer--;
            // Gentle bounce between rows 13 and 15
            if (g->row <= 13) g->dir = DIR_DOWN;
            if (g->row >= 15) g->dir = DIR_UP;
            if (g->move_timer <= 0) {
                int nr = g->row + DIR_DR[g->dir];
                if (ghost_can_enter(g->col, nr, g->mode)) {
                    g->row = nr;
                }
                g->move_timer = GHOST_MOVE_INTERVAL;
            }
        } else {
            // Time to exit — target the door
            g->mode = GMODE_SCATTER;
            g->dir  = DIR_UP;
        }
        return;
    }

    if (g->move_timer > 0) {
        g->move_timer--;
        return;
    }
    g->move_timer = interval;

    // Check if eaten ghost reached the house door area
    if (g->mode == GMODE_EATEN && g->col == 13 && g->row == 11) {
        g->mode        = GMODE_HOUSE;
        g->house_timer = 60;
        g->col         = 13;
        g->row         = 14;
        g->dir         = DIR_UP;
        return;
    }

    Dir new_dir;
    if (g->mode == GMODE_FRIGHTENED) {
        new_dir = ghost_choose_random(g);
    } else {
        int tc, tr;
        ghost_get_target(g, &tc, &tr);
        new_dir = ghost_choose_dir(g, tc, tr);
    }

    if (new_dir != DIR_NONE) {
        int nc = g->col + DIR_DC[new_dir];
        int nr = g->row + DIR_DR[new_dir];
        if (ghost_can_enter(nc, nr, g->mode)) {
            g->col = nc;
            g->row = nr;
            g->dir = new_dir;
        }
    }
}

// ── Update ────────────────────────────────────────────────────────────────────

static void update_playing(void) {
    // Queue Pac-Man direction from buttons
    if (btn_state[BI_UP])    pac.next_dir = DIR_UP;
    if (btn_state[BI_DOWN])  pac.next_dir = DIR_DOWN;
    if (btn_state[BI_LEFT])  pac.next_dir = DIR_LEFT;
    if (btn_state[BI_RIGHT]) pac.next_dir = DIR_RIGHT;

    // Move Pac-Man
    if (pac.move_timer > 0) {
        pac.move_timer--;
    } else {
        pac.move_timer = PAC_MOVE_INTERVAL;
        pac.anim_frame ^= 1;

        // Try to turn in queued direction
        if (pac.next_dir != DIR_NONE) {
            int tc = pac.col + DIR_DC[pac.next_dir];
            int tr = pac.row + DIR_DR[pac.next_dir];
            if (pac_can_enter(tc, tr)) {
                pac.dir = pac.next_dir;
            }
        }

        // Move in current direction
        if (pac.dir != DIR_NONE) {
            int nc = pac.col + DIR_DC[pac.dir];
            int nr = pac.row + DIR_DR[pac.dir];
            if (pac_can_enter(nc, nr)) {
                // Erase old position
                erase_entity(pac.col, pac.row);
                pac.col = nc;
                pac.row = nr;

                // Eat dot or pellet
                uint8_t t = maze[pac.row][pac.col];
                if (t == TILE_DOT) {
                    maze[pac.row][pac.col] = TILE_EMPTY;
                    g_score += 10;
                    g_dots_left--;
                } else if (t == TILE_PELLET) {
                    maze[pac.row][pac.col] = TILE_EMPTY;
                    g_score += 50;
                    g_dots_left--;
                    g_frightened_timer = FRIGHTENED_TICKS;
                    g_ghost_eat_combo  = 1;
                    // Make all non-eaten, non-house ghosts frightened
                    for (int i = 0; i < 4; i++) {
                        if (ghosts[i].mode == GMODE_CHASE ||
                            ghosts[i].mode == GMODE_SCATTER) {
                            ghosts[i].mode = GMODE_FRIGHTENED;
                            // Reverse direction on mode change
                            ghosts[i].dir = dir_opposite(ghosts[i].dir);
                        }
                    }
                }

                if (g_score > g_hiscore) g_hiscore = g_score;
            }
        }
    }

    // Level complete?
    if (g_dots_left <= 0) {
        g_state       = ST_LEVEL_COMPLETE;
        g_state_timer = LEVEL_COMPLETE_TICKS;
        return;
    }

    // Frightened timer
    if (g_frightened_timer > 0) {
        g_frightened_timer--;
        if (g_frightened_timer == 0) {
            for (int i = 0; i < 4; i++) {
                if (ghosts[i].mode == GMODE_FRIGHTENED) {
                    ghosts[i].mode = g_scatter_phase ? GMODE_SCATTER : GMODE_CHASE;
                }
            }
        }
    }

    // Scatter/chase global mode timer (only when not all frightened)
    if (g_frightened_timer == 0) {
        g_mode_timer--;
        if (g_mode_timer <= 0) {
            g_scatter_phase = !g_scatter_phase;
            g_mode_timer    = g_scatter_phase ? SCATTER_TICKS : CHASE_TICKS;
            for (int i = 0; i < 4; i++) {
                if (ghosts[i].mode == GMODE_CHASE || ghosts[i].mode == GMODE_SCATTER) {
                    ghosts[i].mode = g_scatter_phase ? GMODE_SCATTER : GMODE_CHASE;
                }
            }
        }
    }

    // Move ghosts
    for (int i = 0; i < 4; i++) {
        move_ghost(&ghosts[i]);
    }

    // Ghost-Pac-Man collision
    for (int i = 0; i < 4; i++) {
        if (ghosts[i].col != pac.col || ghosts[i].row != pac.row) continue;
        if (ghosts[i].mode == GMODE_FRIGHTENED) {
            // Eat ghost
            int pts = 200 * g_ghost_eat_combo;
            g_score += pts;
            if (g_score > g_hiscore) g_hiscore = g_score;
            g_ghost_eat_combo *= 2;
            ghosts[i].mode = GMODE_EATEN;
            ghosts[i].dir  = dir_opposite(ghosts[i].dir);
            // Show score flash (simplified: just update HUD)
            ESP_LOGI(TAG, "Ghost eaten! +%d", pts);
        } else if (ghosts[i].mode == GMODE_CHASE ||
                   ghosts[i].mode == GMODE_SCATTER) {
            // Pac-Man dies
            g_lives--;
            g_state       = ST_DYING;
            g_state_timer = DYING_TICKS;
            return;
        }
    }
}

// ── Render ────────────────────────────────────────────────────────────────────
static void render(void) {
    if (g_need_full_redraw) {
        draw_maze();
        draw_hud();
        g_need_full_redraw = false;
    }

    if (g_state == ST_PLAYING || g_state == ST_DYING || g_state == ST_READY) {
        // Draw Pac-Man
        if (g_state == ST_DYING) {
            // Flash yellow/black
            uint16_t c = (g_state_timer & 4) ? COLOR_YELLOW : COLOR_BLACK;
            int px = tile_px(pac.col);
            int py = tile_py(pac.row);
            lcd_fill_rect(px + 1, py, 6, 1, c);
            lcd_fill_rect(px, py + 1, 8, 6, c);
            lcd_fill_rect(px + 1, py + 7, 6, 1, c);
        } else {
            draw_pacman(pac.col, pac.row, pac.dir, pac.anim_frame);
        }

        // Draw ghosts
        for (int i = 0; i < 4; i++) {
            if (ghosts[i].mode == GMODE_EATEN) {
                draw_eaten_ghost(ghosts[i].col, ghosts[i].row);
            } else {
                bool frightened = (ghosts[i].mode == GMODE_FRIGHTENED);
                draw_ghost(ghosts[i].col, ghosts[i].row, ghosts[i].color, frightened);
            }
        }
    }

    if (g_state == ST_READY) {
        draw_ready_msg();
    }

    if (g_state == ST_GAME_OVER) {
        draw_game_over_msg();
    }
}

// ── Level complete ────────────────────────────────────────────────────────────
static void render_level_complete(void) {
    // Flash maze walls white/blue
    uint16_t c = (g_state_timer & 8) ? COLOR_WHITE : COLOR_MAZE;
    for (int r = 0; r < MAZE_ROWS; r++) {
        for (int co = 0; co < MAZE_COLS; co++) {
            if (maze_template[r][co] == '#') {
                lcd_fill_rect(tile_px(co), tile_py(r), TILE_SIZE, TILE_SIZE, c);
            }
        }
    }
}

// ── Public API ────────────────────────────────────────────────────────────────
esp_err_t pacman_init(void) {
    esp_err_t err = lcd_init();
    if (err != ESP_OK) return err;

    input_init();

    g_hiscore = 0;
    g_state   = ST_TITLE;

    draw_title();
    return ESP_OK;
}

void pacman_game_loop(void) {
    handle_input();

    switch (g_state) {
        case ST_TITLE:
            if (btn_edge[BI_A]) {
                pacman_reset();
                g_need_full_redraw = true;
                draw_maze();
                draw_hud();
                draw_ready_msg();
            }
            break;

        case ST_READY:
            g_state_timer--;
            if (g_state_timer <= 0) {
                g_state = ST_PLAYING;
                // Clear the READY text
                lcd_draw_string(120, 112, " READY! ", COLOR_BLACK, COLOR_BLACK);
            }
            break;

        case ST_PLAYING:
            if (btn_edge[BI_B]) {
                // Pause — just stop updating, loop still renders
                // Simple toggle: change state to ST_READY to freeze
                // (re-use state_timer as pause indicator)
                // For simplicity, use a dedicated pause flag:
                g_state       = ST_READY;
                g_state_timer = 0x7FFFFFFF; // infinite until A or B pressed
                lcd_draw_string(116, 112, " PAUSED ", COLOR_YELLOW, COLOR_BLACK);
                break;
            }
            update_playing();
            render();
            draw_hud();
            break;

        case ST_DYING:
            g_state_timer--;
            render(); // flash animation
            if (g_state_timer <= 0) {
                if (g_lives <= 0) {
                    g_state = ST_GAME_OVER;
                    g_need_full_redraw = true;
                    draw_maze();
                    draw_hud();
                    draw_game_over_msg();
                } else {
                    // Respawn
                    pac.col       = 13;
                    pac.row       = 23;
                    pac.dir       = DIR_NONE;
                    pac.next_dir  = DIR_NONE;
                    pac.move_timer = 0;
                    ghost_init_all();
                    g_frightened_timer = 0;
                    g_ghost_eat_combo  = 1;
                    g_state       = ST_READY;
                    g_state_timer = READY_TICKS;
                    g_need_full_redraw = true;
                    draw_maze();
                    draw_hud();
                    draw_ready_msg();
                }
            }
            break;

        case ST_LEVEL_COMPLETE:
            g_state_timer--;
            render_level_complete();
            if (g_state_timer <= 0) {
                g_level++;
                // Speed up ghosts a bit (reduce intervals by 1 per level, min 2)
                game_reset_level();
                g_state       = ST_READY;
                g_state_timer = READY_TICKS;
                g_need_full_redraw = true;
                draw_maze();
                draw_hud();
                draw_ready_msg();
            }
            break;

        case ST_GAME_OVER:
            if (btn_edge[BI_A]) {
                pacman_reset();
                g_need_full_redraw = true;
                draw_maze();
                draw_hud();
                draw_ready_msg();
            }
            break;
    }

    // Pause: handle A or B to resume
    if (g_state == ST_READY && g_state_timer == 0x7FFFFFFF) {
        if (btn_edge[BI_A] || btn_edge[BI_B]) {
            g_state = ST_PLAYING;
            lcd_draw_string(116, 112, " PAUSED ", COLOR_BLACK, COLOR_BLACK);
        }
    }
}
