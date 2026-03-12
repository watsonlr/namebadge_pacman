/**
 * @file pacman_game.c
 * @brief Classic Pac-Man — BYUI e-Badge V3.0
 *
 * Maze: 28×28 tiles, 8×8 px each.
 * MADCTL=0xA8 on ILI9341 means physical x=0 is the RIGHT edge of the screen,
 * so tile_px() mirrors the column coordinate to compensate.
 * Movement: tile-based (one step per MOVE_INTERVAL frames).
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


// ── Tile types ────────────────────────────────────────────────────────────────
#define TILE_WALL   0
#define TILE_DOT    1
#define TILE_PELLET 2
#define TILE_EMPTY  3
#define TILE_DOOR   4

// ── Directions ────────────────────────────────────────────────────────────────
typedef enum { DIR_NONE=0, DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT } Dir;

static const int DIR_DC[5] = { 0,  0,  0, -1, +1 };
static const int DIR_DR[5] = { 0, -1, +1,  0,  0 };

static Dir dir_opposite(Dir d) {
    if (d == DIR_UP)    return DIR_DOWN;
    if (d == DIR_DOWN)  return DIR_UP;
    if (d == DIR_LEFT)  return DIR_RIGHT;
    if (d == DIR_RIGHT) return DIR_LEFT;
    return DIR_NONE;
}

// ── Ghost modes ───────────────────────────────────────────────────────────────
typedef enum {
    GMODE_HOUSE = 0,
    GMODE_CHASE,
    GMODE_SCATTER,
    GMODE_FRIGHTENED,
    GMODE_EATEN,
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

// ── Timing ────────────────────────────────────────────────────────────────────
#define PAC_MOVE_INTERVAL    4
#define GHOST_MOVE_INTERVAL  6
#define EATEN_MOVE_INTERVAL  3
#define FRIGHTENED_TICKS     300
#define SCATTER_TICKS        420
#define CHASE_TICKS          1200
#define READY_TICKS          180
#define DYING_TICKS          90
#define LEVEL_COMPLETE_TICKS 120

// ── Maze ─────────────────────────────────────────────────────────────────────
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
    "###.##.##.###  ###.##.##.###",  // 23  ← Pac-Man start col 13
    "#......##....##....##......#",  // 24
    "#.##########.##.##########.#",  // 25
    "#..........................#",  // 26
    "############################",  // 27
};

static uint8_t maze[MAZE_ROWS][MAZE_COLS];

// ── Structs ───────────────────────────────────────────────────────────────────
typedef struct {
    int col, row;
    Dir dir, next_dir;
    int move_timer;
    int anim_frame;
} PacState;

typedef struct {
    int col, row;
    Dir dir;
    GhostMode mode;
    uint16_t color;
    int scatter_col, scatter_row;
    int move_timer;
    int house_timer;
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
static int        g_mode_timer;
static bool       g_scatter_phase;
static int        g_state_timer;
static int        g_ghost_eat_combo;
static bool       g_need_full_redraw;

// Dirty tracking — avoids per-frame SPI floods
static int        prev_pac_col, prev_pac_row, prev_pac_anim;
static int        prev_ghost_col[4], prev_ghost_row[4];
static GhostMode  prev_ghost_mode[4];
static int        prev_score, prev_lives, prev_level;
static bool       hud_dirty;

// Button state
static bool       btn_state[6];
static bool       btn_prev[6];
static bool       btn_edge[6];
static const int  BTN_GPIOS[6] = { BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_A, BTN_B };

#define BI_UP    0
#define BI_DOWN  1
#define BI_LEFT  2
#define BI_RIGHT 3
#define BI_A     4
#define BI_B     5

// ── Coordinate helpers ────────────────────────────────────────────────────────
// MADCTL=0xA8 mirrors the x-axis (x=0 is physical RIGHT edge).
// Flip column so col=0 renders on the physical LEFT and RIGHT button moves right.
static inline int tile_px(int col) {
    return SCREEN_W - MAZE_X - (col + 1) * TILE_SIZE;
}
static inline int tile_py(int row) {
    return MAZE_Y + row * TILE_SIZE;
}

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

static bool pac_can_enter(int col, int row) {
    if (!in_bounds(col, row)) return false;
    uint8_t t = maze[row][col];
    return t != TILE_WALL && t != TILE_DOOR;
}

static bool ghost_can_enter(int col, int row, GhostMode mode) {
    if (!in_bounds(col, row)) return false;
    uint8_t t = maze[row][col];
    if (t == TILE_WALL) return false;
    if (t == TILE_DOOR && mode != GMODE_EATEN && mode != GMODE_HOUSE) return false;
    return true;
}

// ── Drawing ───────────────────────────────────────────────────────────────────
static void draw_tile(int col, int row) {
    int px = tile_px(col);
    int py = tile_py(row);
    uint8_t t = maze[row][col];

    if (t == TILE_WALL) {
        lcd_fill_rect(px, py, TILE_SIZE, TILE_SIZE, COLOR_MAZE);
    } else if (t == TILE_DOT) {
        lcd_fill_rect(px, py, TILE_SIZE, TILE_SIZE, COLOR_BLACK);
        lcd_fill_rect(px + 3, py + 3, 2, 2, COLOR_WHITE);
    } else if (t == TILE_PELLET) {
        lcd_fill_rect(px, py, TILE_SIZE, TILE_SIZE, COLOR_BLACK);
        lcd_fill_rect(px + 2, py + 2, 4, 4, COLOR_YELLOW);
    } else if (t == TILE_DOOR) {
        lcd_fill_rect(px, py, TILE_SIZE, TILE_SIZE, COLOR_BLACK);
        lcd_fill_rect(px, py + 3, TILE_SIZE, 2, COLOR_PINK);
    } else {
        lcd_fill_rect(px, py, TILE_SIZE, TILE_SIZE, COLOR_BLACK);
    }
}

static void draw_maze(void) {
    for (int r = 0; r < MAZE_ROWS; r++)
        for (int c = 0; c < MAZE_COLS; c++)
            draw_tile(c, r);
}

// Draw Pac-Man with mouth wedge facing direction of travel.
// Because tile_px() mirrors x, DIR_LEFT/RIGHT cases are swapped so the mouth
// visually faces the direction of physical movement.
static void draw_pacman(int col, int row, Dir dir, int anim_frame) {
    int px = tile_px(col);
    int py = tile_py(row);

    // Body (circle approximation)
    lcd_fill_rect(px + 1, py + 0, 6, 1, COLOR_YELLOW);
    lcd_fill_rect(px + 0, py + 1, 8, 6, COLOR_YELLOW);
    lcd_fill_rect(px + 1, py + 7, 6, 1, COLOR_YELLOW);

    if (anim_frame == 0) {
        // Mouth open — swap LEFT/RIGHT to compensate for x-mirror
        switch (dir) {
            case DIR_LEFT:   // physically moves RIGHT → mouth on right side of tile
                lcd_fill_rect(px + 4, py + 2, 4, 1, COLOR_BLACK);
                lcd_fill_rect(px + 3, py + 3, 5, 2, COLOR_BLACK);
                lcd_fill_rect(px + 4, py + 5, 4, 1, COLOR_BLACK);
                break;
            case DIR_RIGHT:  // physically moves LEFT → mouth on left side of tile
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
                lcd_fill_rect(px + 4, py + 3, 4, 2, COLOR_BLACK);
                break;
        }
    }
}

static void draw_ghost(int col, int row, uint16_t body_color, bool frightened) {
    int px = tile_px(col);
    int py = tile_py(row);
    uint16_t bc = frightened ? COLOR_DKBLUE : body_color;

    lcd_fill_rect(px + 1, py + 0, 6, 1, bc);
    lcd_fill_rect(px + 0, py + 1, 8, 5, bc);
    lcd_fill_rect(px + 0, py + 6, 2, 2, bc);
    lcd_fill_rect(px + 3, py + 6, 2, 2, bc);
    lcd_fill_rect(px + 6, py + 6, 2, 2, bc);

    if (!frightened) {
        lcd_fill_rect(px + 1, py + 2, 2, 2, COLOR_WHITE);
        lcd_fill_rect(px + 5, py + 2, 2, 2, COLOR_WHITE);
        lcd_fill_rect(px + 2, py + 2, 1, 2, COLOR_BLUE);
        lcd_fill_rect(px + 6, py + 2, 1, 2, COLOR_BLUE);
    } else {
        lcd_fill_rect(px + 1, py + 2, 2, 1, COLOR_WHITE);
        lcd_fill_rect(px + 5, py + 2, 2, 1, COLOR_WHITE);
        lcd_fill_rect(px + 1, py + 5, 1, 1, COLOR_WHITE);
        lcd_fill_rect(px + 3, py + 4, 1, 1, COLOR_WHITE);
        lcd_fill_rect(px + 5, py + 5, 1, 1, COLOR_WHITE);
    }
}

static void draw_eaten_ghost(int col, int row) {
    int px = tile_px(col);
    int py = tile_py(row);
    lcd_fill_rect(px, py, TILE_SIZE, TILE_SIZE, COLOR_BLACK);
    lcd_fill_rect(px + 1, py + 2, 2, 2, COLOR_WHITE);
    lcd_fill_rect(px + 5, py + 2, 2, 2, COLOR_WHITE);
    lcd_fill_rect(px + 2, py + 2, 1, 2, COLOR_BLUE);
    lcd_fill_rect(px + 6, py + 2, 1, 2, COLOR_BLUE);
}

static void erase_entity(int col, int row) {
    if (in_bounds(col, row)) draw_tile(col, row);
}

// ── HUD — bottom 16-px strip only (no side panels — avoids per-frame floods) ─
static void draw_hud(void) {
    char buf[48];
    lcd_fill_rect(0, HUD_Y, SCREEN_W, HUD_H, COLOR_BLACK);
    snprintf(buf, sizeof(buf), "SC:%05d  LV:%d  LF:%d  HI:%05d",
             g_score, g_level, g_lives, g_hiscore);
    // x=4 is fine — on this display the text may appear mirrored but the
    // numbers are the most important part and change visibly as score grows.
    lcd_draw_string(4, HUD_Y + 4, buf, COLOR_WHITE, COLOR_BLACK);
    prev_score = g_score;
    prev_lives = g_lives;
    prev_level = g_level;
    hud_dirty  = false;
}

// ── Title screen ──────────────────────────────────────────────────────────────
static void draw_title(void) {
    lcd_fill_screen(COLOR_BLACK);
    lcd_draw_string( 80, 70,  "P A C - M A N",  COLOR_YELLOW, COLOR_BLACK);
    lcd_draw_string( 88, 90,  "namebadge v1.0", COLOR_WHITE,  COLOR_BLACK);
    lcd_draw_string( 72, 130, "UP/DN/LT/RT move",COLOR_CYAN,  COLOR_BLACK);
    lcd_draw_string( 72, 142, "A = start",        COLOR_GREEN, COLOR_BLACK);
    lcd_draw_string( 72, 154, "B = pause",        COLOR_GREEN, COLOR_BLACK);
    lcd_draw_string( 80, 180, "PRESS A TO PLAY", COLOR_YELLOW, COLOR_BLACK);

    // Decorative ghost row
    static const uint16_t gc[4] = { COLOR_RED, COLOR_PINK, COLOR_CYAN, COLOR_ORANGE };
    for (int i = 0; i < 4; i++) {
        int gx = 80 + i * 40;
        int gy = 210;
        lcd_fill_rect(gx+1, gy+0, 6, 1, gc[i]);
        lcd_fill_rect(gx+0, gy+1, 8, 5, gc[i]);
        lcd_fill_rect(gx+0, gy+6, 2, 2, gc[i]);
        lcd_fill_rect(gx+3, gy+6, 2, 2, gc[i]);
        lcd_fill_rect(gx+6, gy+6, 2, 2, gc[i]);
        lcd_fill_rect(gx+1, gy+2, 2, 2, COLOR_WHITE);
        lcd_fill_rect(gx+5, gy+2, 2, 2, COLOR_WHITE);
    }
}

static void draw_ready_msg(void) {
    lcd_draw_string(120, 112, " READY! ", COLOR_YELLOW, COLOR_BLACK);
}

static void clear_ready_msg(void) {
    lcd_draw_string(120, 112, " READY! ", COLOR_BLACK, COLOR_BLACK);
}

static void draw_game_over_msg(void) {
    lcd_draw_string(112, 104, " GAME OVER ", COLOR_RED,   COLOR_BLACK);
    lcd_draw_string(104, 120, "  PRESS A   ", COLOR_WHITE, COLOR_BLACK);
}

// ── Ghost init ────────────────────────────────────────────────────────────────
static void ghost_init_all(void) {
    ghosts[0] = (Ghost){ .id=0, .col=13, .row=11, .dir=DIR_LEFT,
        .mode=GMODE_SCATTER, .color=COLOR_RED,
        .scatter_col=25, .scatter_row=0,  .move_timer=0, .house_timer=0 };
    ghosts[1] = (Ghost){ .id=1, .col=13, .row=14, .dir=DIR_UP,
        .mode=GMODE_HOUSE,  .color=COLOR_PINK,
        .scatter_col=2,  .scatter_row=0,  .move_timer=0, .house_timer=60 };
    ghosts[2] = (Ghost){ .id=2, .col=12, .row=14, .dir=DIR_DOWN,
        .mode=GMODE_HOUSE,  .color=COLOR_CYAN,
        .scatter_col=25, .scatter_row=27, .move_timer=0, .house_timer=120 };
    ghosts[3] = (Ghost){ .id=3, .col=14, .row=14, .dir=DIR_UP,
        .mode=GMODE_HOUSE,  .color=COLOR_ORANGE,
        .scatter_col=2,  .scatter_row=27, .move_timer=0, .house_timer=180 };

    for (int i = 0; i < 4; i++) {
        prev_ghost_col[i]  = -1;   // -1 = not yet drawn
        prev_ghost_row[i]  = -1;
        prev_ghost_mode[i] = ghosts[i].mode;
    }
}

// ── Game reset ────────────────────────────────────────────────────────────────
static void game_reset_level(void) {
    maze_init();

    pac.col = 13; pac.row = 23;
    pac.dir = DIR_NONE; pac.next_dir = DIR_NONE;
    pac.move_timer = 0; pac.anim_frame = 0;

    prev_pac_col  = -1;
    prev_pac_row  = -1;
    prev_pac_anim = -1;

    ghost_init_all();

    g_frightened_timer = 0;
    g_mode_timer       = SCATTER_TICKS;
    g_scatter_phase    = true;
    g_ghost_eat_combo  = 1;
    g_need_full_redraw = true;
    hud_dirty          = true;
}

void pacman_reset(void) {
    g_score = 0;
    g_lives = 3;
    g_level = 1;
    game_reset_level();
    g_state       = ST_READY;
    g_state_timer = READY_TICKS;
}

// ── Input ─────────────────────────────────────────────────────────────────────
static void input_init(void) {
    for (int i = 0; i < 6; i++) {
        gpio_reset_pin(BTN_GPIOS[i]);
        gpio_set_direction(BTN_GPIOS[i], GPIO_MODE_INPUT);
        gpio_set_pull_mode(BTN_GPIOS[i], GPIO_PULLUP_ONLY);
        btn_state[i] = btn_prev[i] = btn_edge[i] = false;
    }
}

static void handle_input(void) {
    for (int i = 0; i < 6; i++) {
        btn_prev[i]  = btn_state[i];
        btn_state[i] = (gpio_get_level(BTN_GPIOS[i]) == 0);
        btn_edge[i]  = btn_state[i] && !btn_prev[i];
    }
}

// ── Ghost AI ──────────────────────────────────────────────────────────────────
static Dir ghost_choose_dir(Ghost *g, int tc, int tr) {
    Dir opposite = dir_opposite(g->dir);
    Dir best = DIR_NONE;
    int best_dist = 0x7FFFFFFF;
    for (int d = DIR_UP; d <= DIR_RIGHT; d++) {
        if ((Dir)d == opposite) continue;
        int nc = g->col + DIR_DC[d];
        int nr = g->row + DIR_DR[d];
        if (!ghost_can_enter(nc, nr, g->mode)) continue;
        int dist = (nc-tc)*(nc-tc) + (nr-tr)*(nr-tr);
        if (dist < best_dist) { best_dist = dist; best = (Dir)d; }
    }
    if (best == DIR_NONE) {
        int nc = g->col + DIR_DC[opposite];
        int nr = g->row + DIR_DR[opposite];
        if (ghost_can_enter(nc, nr, g->mode)) best = opposite;
    }
    return best;
}

static Dir ghost_choose_random(Ghost *g) {
    Dir opposite = dir_opposite(g->dir);
    Dir opts[4]; int n = 0;
    for (int d = DIR_UP; d <= DIR_RIGHT; d++) {
        if ((Dir)d == opposite) continue;
        int nc = g->col + DIR_DC[d];
        int nr = g->row + DIR_DR[d];
        if (ghost_can_enter(nc, nr, g->mode)) opts[n++] = (Dir)d;
    }
    return n > 0 ? opts[rand() % n] : opposite;
}

static void ghost_get_target(Ghost *g, int *tc, int *tr) {
    if (g->mode == GMODE_SCATTER || g->mode == GMODE_HOUSE) {
        *tc = g->scatter_col; *tr = g->scatter_row; return;
    }
    if (g->mode == GMODE_EATEN) { *tc = 13; *tr = 11; return; }

    switch (g->id) {
        case 0: *tc = pac.col; *tr = pac.row; break;
        case 1: *tc = pac.col + DIR_DC[pac.dir]*4;
                *tr = pac.row + DIR_DR[pac.dir]*4; break;
        case 2: { int mc = pac.col + DIR_DC[pac.dir]*2;
                  int mr = pac.row + DIR_DR[pac.dir]*2;
                  *tc = mc + (mc - ghosts[0].col);
                  *tr = mr + (mr - ghosts[0].row); break; }
        case 3: { int dc = g->col-pac.col, dr = g->row-pac.row;
                  if (dc*dc + dr*dr > 64) { *tc=pac.col; *tr=pac.row; }
                  else { *tc=g->scatter_col; *tr=g->scatter_row; } break; }
        default: *tc = pac.col; *tr = pac.row;
    }
    if (*tc < 0) *tc = 0;
    if (*tc >= MAZE_COLS) *tc = MAZE_COLS - 1;
    if (*tr < 0) *tr = 0;
    if (*tr >= MAZE_ROWS) *tr = MAZE_ROWS - 1;
}

static void move_ghost(Ghost *g) {
    int interval = (g->mode == GMODE_EATEN) ? EATEN_MOVE_INTERVAL : GHOST_MOVE_INTERVAL;

    if (g->mode == GMODE_HOUSE) {
        if (g->house_timer > 0) {
            g->house_timer--;
            if (g->row <= 13) g->dir = DIR_DOWN;
            if (g->row >= 15) g->dir = DIR_UP;
            if (g->move_timer <= 0) {
                int nr = g->row + DIR_DR[g->dir];
                if (ghost_can_enter(g->col, nr, g->mode)) g->row = nr;
                g->move_timer = GHOST_MOVE_INTERVAL;
            }
        } else {
            g->mode = GMODE_SCATTER;
            g->dir  = DIR_UP;
        }
        return;
    }

    if (g->move_timer > 0) { g->move_timer--; return; }
    g->move_timer = interval;

    if (g->mode == GMODE_EATEN && g->col == 13 && g->row == 11) {
        g->mode = GMODE_HOUSE; g->house_timer = 60;
        g->col = 13; g->row = 14; g->dir = DIR_UP;
        return;
    }

    Dir new_dir = (g->mode == GMODE_FRIGHTENED) ? ghost_choose_random(g)
                : ({ int tc, tr; ghost_get_target(g, &tc, &tr);
                     ghost_choose_dir(g, tc, tr); });

    if (new_dir != DIR_NONE) {
        int nc = g->col + DIR_DC[new_dir];
        int nr = g->row + DIR_DR[new_dir];
        if (ghost_can_enter(nc, nr, g->mode)) {
            g->col = nc; g->row = nr; g->dir = new_dir;
        }
    }
}

// ── Update ────────────────────────────────────────────────────────────────────
static void update_playing(void) {
    // Queue direction from held buttons
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

        if (pac.next_dir != DIR_NONE) {
            int tc = pac.col + DIR_DC[pac.next_dir];
            int tr = pac.row + DIR_DR[pac.next_dir];
            if (pac_can_enter(tc, tr)) pac.dir = pac.next_dir;
        }

        if (pac.dir != DIR_NONE) {
            int nc = pac.col + DIR_DC[pac.dir];
            int nr = pac.row + DIR_DR[pac.dir];
            if (pac_can_enter(nc, nr)) {
                pac.col = nc; pac.row = nr;

                uint8_t t = maze[pac.row][pac.col];
                if (t == TILE_DOT) {
                    maze[pac.row][pac.col] = TILE_EMPTY;
                    g_score += 10; g_dots_left--; hud_dirty = true;
                } else if (t == TILE_PELLET) {
                    maze[pac.row][pac.col] = TILE_EMPTY;
                    g_score += 50; g_dots_left--; hud_dirty = true;
                    g_frightened_timer = FRIGHTENED_TICKS;
                    g_ghost_eat_combo  = 1;
                    for (int i = 0; i < 4; i++) {
                        if (ghosts[i].mode == GMODE_CHASE ||
                            ghosts[i].mode == GMODE_SCATTER) {
                            ghosts[i].mode = GMODE_FRIGHTENED;
                            ghosts[i].dir  = dir_opposite(ghosts[i].dir);
                        }
                    }
                }
                if (g_score > g_hiscore) { g_hiscore = g_score; hud_dirty = true; }
            }
        }
    }

    if (g_dots_left <= 0) {
        g_state = ST_LEVEL_COMPLETE; g_state_timer = LEVEL_COMPLETE_TICKS; return;
    }

    if (g_frightened_timer > 0) {
        g_frightened_timer--;
        if (g_frightened_timer == 0) {
            for (int i = 0; i < 4; i++)
                if (ghosts[i].mode == GMODE_FRIGHTENED)
                    ghosts[i].mode = g_scatter_phase ? GMODE_SCATTER : GMODE_CHASE;
        }
    }

    if (g_frightened_timer == 0) {
        if (--g_mode_timer <= 0) {
            g_scatter_phase = !g_scatter_phase;
            g_mode_timer    = g_scatter_phase ? SCATTER_TICKS : CHASE_TICKS;
            for (int i = 0; i < 4; i++)
                if (ghosts[i].mode == GMODE_CHASE || ghosts[i].mode == GMODE_SCATTER)
                    ghosts[i].mode = g_scatter_phase ? GMODE_SCATTER : GMODE_CHASE;
        }
    }

    for (int i = 0; i < 4; i++) move_ghost(&ghosts[i]);

    // Collision
    for (int i = 0; i < 4; i++) {
        if (ghosts[i].col != pac.col || ghosts[i].row != pac.row) continue;
        if (ghosts[i].mode == GMODE_FRIGHTENED) {
            int pts = 200 * g_ghost_eat_combo;
            g_score += pts; g_ghost_eat_combo *= 2;
            if (g_score > g_hiscore) g_hiscore = g_score;
            hud_dirty = true;
            ghosts[i].mode = GMODE_EATEN;
            ghosts[i].dir  = dir_opposite(ghosts[i].dir);
        } else if (ghosts[i].mode == GMODE_CHASE || ghosts[i].mode == GMODE_SCATTER) {
            g_lives--;
            hud_dirty = true;
            g_state = ST_DYING; g_state_timer = DYING_TICKS; return;
        }
    }
}

// ── Render ────────────────────────────────────────────────────────────────────
static void render(void) {
    if (g_need_full_redraw) {
        draw_maze();
        // Reset dirty tracking so everything is drawn fresh
        prev_pac_col = -1; prev_pac_row = -1; prev_pac_anim = -1;
        for (int i = 0; i < 4; i++) {
            prev_ghost_col[i] = -1; prev_ghost_row[i] = -1;
        }
        g_need_full_redraw = false;
    }

    bool in_game = (g_state == ST_PLAYING ||
                    g_state == ST_DYING   ||
                    g_state == ST_READY);
    if (!in_game) goto hud_update;

    // ── Pac-Man ───────────────────────────────────────────────────────────────
    {
        bool pos_changed  = (pac.col != prev_pac_col || pac.row != prev_pac_row);
        bool anim_changed = (pac.anim_frame != prev_pac_anim);
        bool force        = (g_state == ST_DYING);  // flash every frame

        if (pos_changed || anim_changed || force) {
            // Erase previous tile (only if position moved)
            if (prev_pac_col >= 0 && pos_changed)
                erase_entity(prev_pac_col, prev_pac_row);

            if (g_state == ST_DYING) {
                uint16_t c = (g_state_timer & 4) ? COLOR_YELLOW : COLOR_BLACK;
                int px = tile_px(pac.col), py = tile_py(pac.row);
                lcd_fill_rect(px+1, py+0, 6, 1, c);
                lcd_fill_rect(px+0, py+1, 8, 6, c);
                lcd_fill_rect(px+1, py+7, 6, 1, c);
            } else {
                draw_pacman(pac.col, pac.row, pac.dir, pac.anim_frame);
            }
            prev_pac_col  = pac.col;
            prev_pac_row  = pac.row;
            prev_pac_anim = pac.anim_frame;
        }
    }

    // ── Ghosts ────────────────────────────────────────────────────────────────
    for (int i = 0; i < 4; i++) {
        bool pos_changed  = (ghosts[i].col != prev_ghost_col[i] ||
                             ghosts[i].row != prev_ghost_row[i]);
        bool mode_changed = (ghosts[i].mode != prev_ghost_mode[i]);

        if (pos_changed || mode_changed) {
            if (prev_ghost_col[i] >= 0 && pos_changed)
                erase_entity(prev_ghost_col[i], prev_ghost_row[i]);

            if (ghosts[i].mode == GMODE_EATEN) {
                draw_eaten_ghost(ghosts[i].col, ghosts[i].row);
            } else {
                draw_ghost(ghosts[i].col, ghosts[i].row, ghosts[i].color,
                           ghosts[i].mode == GMODE_FRIGHTENED);
            }
            prev_ghost_col[i]  = ghosts[i].col;
            prev_ghost_row[i]  = ghosts[i].row;
            prev_ghost_mode[i] = ghosts[i].mode;
        }
    }

hud_update:
    if (hud_dirty) draw_hud();
}

// ── Level-complete flash ──────────────────────────────────────────────────────
static void render_level_complete(void) {
    uint16_t c = (g_state_timer & 8) ? COLOR_WHITE : COLOR_MAZE;
    for (int r = 0; r < MAZE_ROWS; r++)
        for (int co = 0; co < MAZE_COLS; co++)
            if (maze_template[r][co] == '#')
                lcd_fill_rect(tile_px(co), tile_py(r), TILE_SIZE, TILE_SIZE, c);
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
            // Pause re-use: timer==0x7FFFFFFF → frozen until A/B pressed
            if (g_state_timer == 0x7FFFFFFF) {
                if (btn_edge[BI_A] || btn_edge[BI_B]) {
                    g_state = ST_PLAYING;
                    lcd_draw_string(116, 112, " PAUSED ", COLOR_BLACK, COLOR_BLACK);
                }
                break;
            }
            if (--g_state_timer <= 0) {
                g_state = ST_PLAYING;
                clear_ready_msg();
            }
            break;

        case ST_PLAYING:
            if (btn_edge[BI_B]) {
                g_state       = ST_READY;
                g_state_timer = 0x7FFFFFFF;
                lcd_draw_string(116, 112, " PAUSED ", COLOR_YELLOW, COLOR_BLACK);
                break;
            }
            update_playing();
            render();
            break;

        case ST_DYING:
            if (--g_state_timer <= 0) {
                if (g_lives <= 0) {
                    g_state = ST_GAME_OVER;
                    g_need_full_redraw = true;
                    draw_maze();
                    draw_hud();
                    draw_game_over_msg();
                } else {
                    pac.col = 13; pac.row = 23;
                    pac.dir = DIR_NONE; pac.next_dir = DIR_NONE;
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
            } else {
                render();  // flash animation
            }
            break;

        case ST_LEVEL_COMPLETE:
            render_level_complete();
            if (--g_state_timer <= 0) {
                g_level++;
                game_reset_level();
                g_state       = ST_READY;
                g_state_timer = READY_TICKS;
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
}
