/**
 * LanPlay Setup v2.0 — graphical configurator with full touch keyboard.
 *
 * Uses the libnx Framebuffer API to render a Switch-style keyboard with
 * large, touchable key rectangles and scaled bitmap text.
 * Supports both touch input and D-Pad/buttons for docked mode.
 *
 * Config file: sdmc:/config/lan-play/config.ini
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <switch.h>

/* ═══════════════════════════════════════════════════════════════════════ */
/*  Constants                                                             */
/* ═══════════════════════════════════════════════════════════════════════ */
#define FB_W  1280
#define FB_H  720
#define STR_MAX 256
#define CONFIG_DIR  "sdmc:/config/lan-play"
#define CONFIG_PATH "sdmc:/config/lan-play/config.ini"
#define MAX_CFG_LINES 64
#define MAX_LINE 512

#define KEY_BS  '\x08'
#define KEY_OK  '\x0D'

/* ═══════════════════════════════════════════════════════════════════════ */
/*  Colors (RGBA8 little-endian: R | G<<8 | B<<16 | A<<24)               */
/* ═══════════════════════════════════════════════════════════════════════ */
#define C_BG       RGBA8_MAXALPHA(40,  40,  46)
#define C_FIELD_BG RGBA8_MAXALPHA(28,  28,  34)
#define C_FIELD_BD RGBA8_MAXALPHA(0,  160, 220)
#define C_KEY      RGBA8_MAXALPHA(80,  80,  88)
#define C_KEY_HI   RGBA8_MAXALPHA(60,  60,  68)   /* top/left  3D edge   */
#define C_KEY_LO   RGBA8_MAXALPHA(50,  50,  55)   /* bot/right 3D edge   */
#define C_KEY_SEL  RGBA8_MAXALPHA(0,  140, 255)    /* pad-cursor outline  */
#define C_KEY_TAP  RGBA8_MAXALPHA(255, 255, 255)   /* flash on tap        */
#define C_DEL      RGBA8_MAXALPHA(160, 55,  55)
#define C_DEL_HI   RGBA8_MAXALPHA(180, 80,  80)
#define C_DEL_LO   RGBA8_MAXALPHA(110, 35,  35)
#define C_DONE     RGBA8_MAXALPHA(50,  160, 70)
#define C_DONE_HI  RGBA8_MAXALPHA(70,  190, 90)
#define C_DONE_LO  RGBA8_MAXALPHA(30,  110, 50)
#define C_SPC      RGBA8_MAXALPHA(70,  70,  78)
#define C_TXT      RGBA8_MAXALPHA(255, 255, 255)
#define C_TXT_DIM  RGBA8_MAXALPHA(160, 160, 160)
#define C_CURSOR   RGBA8_MAXALPHA(0,  200, 255)
#define C_TITLE_BG RGBA8_MAXALPHA(0,  100, 180)
#define C_GREEN_T  RGBA8_MAXALPHA(80,  220, 100)
#define C_RED_T    RGBA8_MAXALPHA(220, 80,  80)

/* ═══════════════════════════════════════════════════════════════════════ */
/*  Embedded 8×8 bitmap font (printable ASCII 32-126, public domain)      */
/*  Bit 0 = leftmost pixel.  Scaled up at render time.                    */
/* ═══════════════════════════════════════════════════════════════════════ */
static const uint8_t FONT[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 32 ' ' */
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, /* 33 '!' */
    {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00}, /* 34 '"' */
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, /* 35 '#' */
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, /* 36 '$' */
    {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00}, /* 37 '%' */
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, /* 38 '&' */
    {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00}, /* 39 ''' */
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, /* 40 '(' */
    {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00}, /* 41 ')' */
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, /* 42 '*' */
    {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00}, /* 43 '+' */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, /* 44 ',' */
    {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00}, /* 45 '-' */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, /* 46 '.' */
    {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00}, /* 47 '/' */
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, /* 48 '0' */
    {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00}, /* 49 '1' */
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, /* 50 '2' */
    {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00}, /* 51 '3' */
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, /* 52 '4' */
    {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00}, /* 53 '5' */
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, /* 54 '6' */
    {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00}, /* 55 '7' */
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, /* 56 '8' */
    {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00}, /* 57 '9' */
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, /* 58 ':' */
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06}, /* 59 ';' */
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, /* 60 '<' */
    {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00}, /* 61 '=' */
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, /* 62 '>' */
    {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00}, /* 63 '?' */
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, /* 64 '@' */
    {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00}, /* 65 'A' */
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, /* 66 'B' */
    {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00}, /* 67 'C' */
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, /* 68 'D' */
    {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00}, /* 69 'E' */
    {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00}, /* 70 'F' */
    {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00}, /* 71 'G' */
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, /* 72 'H' */
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* 73 'I' */
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, /* 74 'J' */
    {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00}, /* 75 'K' */
    {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00}, /* 76 'L' */
    {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00}, /* 77 'M' */
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, /* 78 'N' */
    {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00}, /* 79 'O' */
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00}, /* 80 'P' */
    {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00}, /* 81 'Q' */
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, /* 82 'R' */
    {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00}, /* 83 'S' */
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* 84 'T' */
    {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00}, /* 85 'U' */
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, /* 86 'V' */
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, /* 87 'W' */
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, /* 88 'X' */
    {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00}, /* 89 'Y' */
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00}, /* 90 'Z' */
    {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00}, /* 91 '[' */
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00}, /* 92 '\' */
    {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00}, /* 93 ']' */
    {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00}, /* 94 '^' */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, /* 95 '_' */
    {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00}, /* 96 '`' */
    {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00}, /* 97 'a' */
    {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00}, /* 98 'b' */
    {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00}, /* 99 'c' */
    {0x38,0x30,0x30,0x3E,0x33,0x33,0x6E,0x00}, /*100 'd' */
    {0x00,0x00,0x1E,0x33,0x3F,0x03,0x1E,0x00}, /*101 'e' */
    {0x1C,0x36,0x06,0x0F,0x06,0x06,0x0F,0x00}, /*102 'f' */
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F}, /*103 'g' */
    {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00}, /*104 'h' */
    {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00}, /*105 'i' */
    {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E}, /*106 'j' */
    {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00}, /*107 'k' */
    {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /*108 'l' */
    {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00}, /*109 'm' */
    {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00}, /*110 'n' */
    {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00}, /*111 'o' */
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F}, /*112 'p' */
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78}, /*113 'q' */
    {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00}, /*114 'r' */
    {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00}, /*115 's' */
    {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00}, /*116 't' */
    {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00}, /*117 'u' */
    {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00}, /*118 'v' */
    {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00}, /*119 'w' */
    {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00}, /*120 'x' */
    {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F}, /*121 'y' */
    {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00}, /*122 'z' */
    {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00}, /*123 '{' */
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, /*124 '|' */
    {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00}, /*125 '}' */
    {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00}, /*126 '~' */
};

/* ═══════════════════════════════════════════════════════════════════════ */
/*  Framebuffer globals                                                   */
/* ═══════════════════════════════════════════════════════════════════════ */
static Framebuffer g_fb;
static u32 *g_buf;
static u32  g_stride; /* bytes per row */

static inline void px(int x, int y, u32 c)
{
    if ((unsigned)x < FB_W && (unsigned)y < FB_H)
        g_buf[y * (g_stride >> 2) + x] = c;
}

/* ═══════════════════════════════════════════════════════════════════════ */
/*  Drawing primitives                                                    */
/* ═══════════════════════════════════════════════════════════════════════ */
static void fill_screen(u32 c)
{
    u32 pw = g_stride >> 2;
    for (u32 y = 0; y < FB_H; y++)
        for (u32 x = 0; x < FB_W; x++)
            g_buf[y * pw + x] = c;
}

static void fill_rect(int x0, int y0, int w, int h, u32 c)
{
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++)
            px(x, y, c);
}

/* 3D raised button rectangle */
static void draw_btn(int x, int y, int w, int h, u32 face, u32 hi, u32 lo)
{
    fill_rect(x, y, w, h, face);
    fill_rect(x, y, w, 2, hi);          /* top highlight */
    fill_rect(x, y, 2, h, hi);          /* left highlight */
    fill_rect(x, y + h - 2, w, 2, lo);  /* bottom shadow */
    fill_rect(x + w - 2, y, 2, h, lo);  /* right shadow */
}

/* Selection outline (pad cursor) */
static void draw_outline(int x, int y, int w, int h, u32 c, int t)
{
    fill_rect(x - t, y - t, w + 2*t, t, c);       /* top */
    fill_rect(x - t, y + h, w + 2*t, t, c);       /* bottom */
    fill_rect(x - t, y, t, h, c);                  /* left */
    fill_rect(x + w, y, t, h, c);                  /* right */
}

/* Draw one bitmap character at scale S (each font pixel = S×S screen px) */
static void draw_char(int cx, int cy, char ch, u32 color, int s)
{
    if (ch < 32 || ch > 126) return;
    const uint8_t *glyph = FONT[ch - 32];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (1 << col)) {
                int bx = cx + col * s;
                int by = cy + row * s;
                for (int dy = 0; dy < s; dy++)
                    for (int dx = 0; dx < s; dx++)
                        px(bx + dx, by + dy, color);
            }
        }
    }
}

static void draw_text(int x, int y, const char *t, u32 c, int s)
{
    for (int i = 0; t[i]; i++)
        draw_char(x + i * 8 * s, y, t[i], c, s);
}

static int text_w(const char *t, int s) { return (int)strlen(t) * 8 * s; }

static void draw_text_c(int y, const char *t, u32 c, int s)
{
    draw_text((FB_W - text_w(t, s)) / 2, y, t, c, s);
}

/* ═══════════════════════════════════════════════════════════════════════ */
/*  Keyboard layout                                                       */
/* ═══════════════════════════════════════════════════════════════════════ */

/* Key definition */
typedef struct {
    int  x, y, w, h;    /* pixel rect */
    char val;            /* character or KEY_BS / KEY_OK */
    char label[8];       /* display text */
    u32  face, hi, lo;   /* colors */
} Key;

#define MAX_KEYS 60
static Key g_keys[MAX_KEYS];
static int g_nkeys;
static int g_sel;      /* selected key index (pad cursor) */

/* Grid params for char rows (rows 0-3) */
#define KBD_MARGIN  60
#define KBD_GAP     8
#define KBD_VGAP    8
#define KBD_COLS    10
#define KBD_TOP     160
#define KBD_KEY_H   88

static int KBD_KEY_W; /* computed at init */

static const char ROWS[4][11] = {
    "1234567890",
    "qwertyuiop",
    "asdfghjkl.",
    "zxcvbnm:-/",
};

static void kbd_init(void)
{
    g_nkeys = 0;
    KBD_KEY_W = (FB_W - 2 * KBD_MARGIN - (KBD_COLS - 1) * KBD_GAP) / KBD_COLS;

    /* Rows 0-3: regular character keys */
    for (int r = 0; r < 4; r++) {
        int rowY = KBD_TOP + r * (KBD_KEY_H + KBD_VGAP);
        for (int c = 0; c < KBD_COLS; c++) {
            Key *k = &g_keys[g_nkeys++];
            k->x   = KBD_MARGIN + c * (KBD_KEY_W + KBD_GAP);
            k->y   = rowY;
            k->w   = KBD_KEY_W;
            k->h   = KBD_KEY_H;
            k->val = ROWS[r][c];
            k->label[0] = ROWS[r][c]; k->label[1] = '\0';
            k->face = C_KEY; k->hi = C_KEY_HI; k->lo = C_KEY_LO;
        }
    }

    /* Row 4: special keys — @  _  [SPACE]  [DEL]  [DONE] */
    int r4y = KBD_TOP + 4 * (KBD_KEY_H + KBD_VGAP);
    int total_w = FB_W - 2 * KBD_MARGIN;
    /* Widths: @ and _ are 1 unit, SPC=4, DEL=2, DONE=2; total=10 units */
    int unit = (total_w - 4 * KBD_GAP) / 10; /* ≈ same as KBD_KEY_W */
    int cx = KBD_MARGIN;

    /* @ */
    { Key *k=&g_keys[g_nkeys++]; k->x=cx; k->y=r4y; k->w=unit; k->h=KBD_KEY_H;
      k->val='@'; strcpy(k->label,"@"); k->face=C_KEY; k->hi=C_KEY_HI; k->lo=C_KEY_LO; cx+=unit+KBD_GAP; }
    /* _ */
    { Key *k=&g_keys[g_nkeys++]; k->x=cx; k->y=r4y; k->w=unit; k->h=KBD_KEY_H;
      k->val='_'; strcpy(k->label,"_"); k->face=C_KEY; k->hi=C_KEY_HI; k->lo=C_KEY_LO; cx+=unit+KBD_GAP; }
    /* SPACE (4 units) */
    { int sw=unit*4+KBD_GAP*3; Key *k=&g_keys[g_nkeys++]; k->x=cx; k->y=r4y; k->w=sw; k->h=KBD_KEY_H;
      k->val=' '; strcpy(k->label,"SPACE"); k->face=C_SPC; k->hi=C_KEY_HI; k->lo=C_KEY_LO; cx+=sw+KBD_GAP; }
    /* DEL (1.5 units) */
    { int dw=unit*2-KBD_GAP/2; Key *k=&g_keys[g_nkeys++]; k->x=cx; k->y=r4y; k->w=dw; k->h=KBD_KEY_H;
      k->val=KEY_BS; strcpy(k->label,"DEL"); k->face=C_DEL; k->hi=C_DEL_HI; k->lo=C_DEL_LO; cx+=dw+KBD_GAP; }
    /* DONE (remaining) */
    { int ow=KBD_MARGIN+total_w-cx; Key *k=&g_keys[g_nkeys++]; k->x=cx; k->y=r4y; k->w=ow; k->h=KBD_KEY_H;
      k->val=KEY_OK; strcpy(k->label,"DONE"); k->face=C_DONE; k->hi=C_DONE_HI; k->lo=C_DONE_LO; }

    g_sel = KBD_COLS; /* start on 'q' row */
}

/* ═══════════════════════════════════════════════════════════════════════ */
/*  Keyboard draw                                                         */
/* ═══════════════════════════════════════════════════════════════════════ */
static void kbd_draw(const char *buf, int len, int tap_idx)
{
    fill_screen(C_BG);

    /* ── Title bar ──────────────────────────────────────────────── */
    fill_rect(0, 0, FB_W, 48, C_TITLE_BG);
    draw_text_c(12, "LAN Play Setup", C_TXT, 3);

    /* ── Text input field ───────────────────────────────────────── */
    int fy = 64, fh = 70;
    fill_rect(KBD_MARGIN, fy, FB_W - 2*KBD_MARGIN, fh, C_FIELD_BG);
    /* border */
    fill_rect(KBD_MARGIN, fy, FB_W-2*KBD_MARGIN, 2, C_FIELD_BD);
    fill_rect(KBD_MARGIN, fy+fh-2, FB_W-2*KBD_MARGIN, 2, C_FIELD_BD);
    fill_rect(KBD_MARGIN, fy, 2, fh, C_FIELD_BD);
    fill_rect(FB_W-KBD_MARGIN-2, fy, 2, fh, C_FIELD_BD);

    /* Text (show last ~40 chars if too long) */
    int max_chars = (FB_W - 2*KBD_MARGIN - 30) / (8*3);
    int start = len > max_chars ? len - max_chars : 0;
    char display[256] = {0};
    if (len > 0) memcpy(display, buf + start, len - start);
    display[len - start] = '\0';
    draw_text(KBD_MARGIN + 14, fy + 22, display, C_TXT, 3);
    /* Cursor blink */
    int cx_pos = KBD_MARGIN + 14 + text_w(display, 3);
    fill_rect(cx_pos, fy + 16, 3, 38, C_CURSOR);

    /* Char count */
    char cnt[16]; snprintf(cnt, sizeof(cnt), "%d", len);
    draw_text(FB_W - KBD_MARGIN - text_w(cnt, 2) - 10, fy + 28, cnt, C_TXT_DIM, 2);

    /* ── Keys ───────────────────────────────────────────────────── */
    for (int i = 0; i < g_nkeys; i++) {
        Key *k = &g_keys[i];
        u32 face = (i == tap_idx) ? C_KEY_TAP : k->face;
        u32 hi   = (i == tap_idx) ? C_TXT     : k->hi;
        u32 lo   = (i == tap_idx) ? C_TXT_DIM : k->lo;

        draw_btn(k->x, k->y, k->w, k->h, face, hi, lo);

        /* Selection outline */
        if (i == g_sel)
            draw_outline(k->x, k->y, k->w, k->h, C_KEY_SEL, 3);

        /* Label — centered in key */
        u32 tc = (i == tap_idx) ? RGBA8_MAXALPHA(0,0,0) : C_TXT;
        int scale = (strlen(k->label) == 1) ? 4 : 3;
        int lw = text_w(k->label, scale);
        int lh = 8 * scale;
        draw_text(k->x + (k->w - lw) / 2, k->y + (k->h - lh) / 2,
                  k->label, tc, scale);
    }

    /* ── Footer hints ───────────────────────────────────────────── */
    draw_text_c(FB_H - 30, "Touch keys  |  D-Pad + A  |  B: del  |  +: confirm  |  -: cancel", C_TXT_DIM, 2);
}

/* ═══════════════════════════════════════════════════════════════════════ */
/*  Touch / pad helpers                                                   */
/* ═══════════════════════════════════════════════════════════════════════ */
static PadState g_pad;

/* Returns key index at pixel (px,py), or -1 */
static int key_at(int px_x, int px_y)
{
    for (int i = 0; i < g_nkeys; i++) {
        Key *k = &g_keys[i];
        if (px_x >= k->x && px_x < k->x + k->w &&
            px_y >= k->y && px_y < k->y + k->h) return i;
    }
    return -1;
}

/* Pad navigation: find nearest key in a direction */
static int find_key_dir(int from, int dx, int dy)
{
    Key *cur = &g_keys[from];
    int cx = cur->x + cur->w/2, cy = cur->y + cur->h/2;
    int best = -1;
    int best_dist = 999999;

    for (int i = 0; i < g_nkeys; i++) {
        if (i == from) continue;
        Key *k = &g_keys[i];
        int kx = k->x + k->w/2, ky = k->y + k->h/2;
        int ddx = kx - cx, ddy = ky - cy;

        /* Check direction */
        if (dx > 0 && ddx <= 0) continue;
        if (dx < 0 && ddx >= 0) continue;
        if (dy > 0 && ddy <= 0) continue;
        if (dy < 0 && ddy >= 0) continue;

        int dist = ddx*ddx + ddy*ddy;
        if (dist < best_dist) { best_dist = dist; best = i; }
    }
    return (best >= 0) ? best : from;
}

/* ═══════════════════════════════════════════════════════════════════════ */
/*  INI helpers                                                           */
/* ═══════════════════════════════════════════════════════════════════════ */
static void trim(char *s)
{
    char *p = s;
    while (*p==' '||*p=='\t') p++;
    if (p != s) memmove(s, p, strlen(p)+1);
    size_t l = strlen(s);
    while (l>0&&(s[l-1]==' '||s[l-1]=='\t'||s[l-1]=='\r'||s[l-1]=='\n')) s[--l]='\0';
}

static int read_relay(char *buf, size_t sz)
{
    buf[0]='\0';
    FILE *f=fopen(CONFIG_PATH,"r"); if(!f)return-1;
    char line[512];
    while(fgets(line,sizeof(line),f)){
        trim(line);
        if(!line[0]||line[0]==';'||line[0]=='#'||line[0]=='[')continue;
        char *eq=strchr(line,'='); if(!eq)continue;
        *eq='\0'; char *k=line,*v=eq+1; trim(k); trim(v);
        if(strcmp(k,"relay_addr")==0){
            size_t n=strlen(v); if(n>=sz)n=sz-1;
            memcpy(buf,v,n); buf[n]='\0'; fclose(f); return 0;
        }
    }
    fclose(f); return-1;
}

static int save_relay(const char *addr)
{
    mkdir(CONFIG_DIR,0777);
    char lines[MAX_CFG_LINES][MAX_LINE]; int n=0; bool found=false;
    FILE *r=fopen(CONFIG_PATH,"r");
    if(r){ while(n<MAX_CFG_LINES-1&&fgets(lines[n],MAX_LINE,r))n++; fclose(r); }
    for(int i=0;i<n;i++){
        char t[MAX_LINE]; memcpy(t,lines[i],MAX_LINE-1); t[MAX_LINE-1]='\0'; trim(t);
        if(strncmp(t,"relay_addr",10)==0&&strchr(t,'=')){
            snprintf(lines[i],MAX_LINE,"relay_addr = %s\n",addr); found=true; break;
        }
    }
    if(!found){
        if(!n){ snprintf(lines[n++],MAX_LINE,"; switch-lan-play\n"); snprintf(lines[n++],MAX_LINE,"[server]\n"); }
        if(n<MAX_CFG_LINES-1) snprintf(lines[n++],MAX_LINE,"relay_addr = %s\n",addr);
    }
    FILE *w=fopen(CONFIG_PATH,"w"); if(!w)return-1;
    for(int i=0;i<n;i++)fputs(lines[i],w);
    fclose(w); return 0;
}

/* ═══════════════════════════════════════════════════════════════════════ */
/*  Keyboard screen (main interaction loop)                               */
/* ═══════════════════════════════════════════════════════════════════════ */
static bool keyboard_screen(const char *prefill, char *out, size_t outsz)
{
    char buf[STR_MAX]={0}; int len=0;
    if(prefill&&prefill[0]){
        size_t n=strlen(prefill); if(n>=outsz)n=outsz-1;
        memcpy(buf,prefill,n); buf[n]='\0'; len=(int)n;
    }

    kbd_init();

    s32 prev_tc=0;
    int tap_flash=-1;
    int flash_frames=0;

    u64 stick_last=0;
    const u64 STICK_D=120000000ULL;

    while(appletMainLoop()){

        /* ── Draw ── */
        g_buf=(u32*)framebufferBegin(&g_fb,&g_stride);
        kbd_draw(buf, len, tap_flash);
        framebufferEnd(&g_fb);

        /* Flash timer */
        if(tap_flash>=0 && ++flash_frames>4){ tap_flash=-1; flash_frames=0; }

        /* ── Input ── */
        padUpdate(&g_pad);
        u64 down=padGetButtonsDown(&g_pad);

        /* D-pad navigation */
        if(down&HidNpadButton_Right) g_sel=find_key_dir(g_sel, 1, 0);
        if(down&HidNpadButton_Left)  g_sel=find_key_dir(g_sel,-1, 0);
        if(down&HidNpadButton_Down)  g_sel=find_key_dir(g_sel, 0, 1);
        if(down&HidNpadButton_Up)    g_sel=find_key_dir(g_sel, 0,-1);

        /* Stick */
        { HidAnalogStickState ls=padGetStickPos(&g_pad,0);
          u64 now=armTicksToNs(armGetSystemTick());
          if(now-stick_last>STICK_D){
              bool m=false;
              if(ls.x> 20000){g_sel=find_key_dir(g_sel, 1, 0);m=true;}
              if(ls.x<-20000){g_sel=find_key_dir(g_sel,-1, 0);m=true;}
              if(ls.y> 20000){g_sel=find_key_dir(g_sel, 0,-1);m=true;}
              if(ls.y<-20000){g_sel=find_key_dir(g_sel, 0, 1);m=true;}
              if(m)stick_last=now;
          }
        }

        /* A = activate selected key */
        if(down&HidNpadButton_A){
            char v=g_keys[g_sel].val;
            tap_flash=g_sel; flash_frames=0;
            if(v==KEY_OK){ if(len>0){memcpy(out,buf,len);out[len]='\0';return true;} }
            else if(v==KEY_BS){ if(len>0)buf[--len]='\0'; }
            else{ if(len<(int)(outsz-1)){buf[len++]=v;buf[len]='\0';} }
        }

        /* Touch */
        { HidTouchScreenState ts={0};
          if(hidGetTouchScreenStates(&ts,1)){
              bool nt=(ts.count>0&&prev_tc==0);
              prev_tc=ts.count;
              if(nt){
                  int ki=key_at(ts.touches[0].x, ts.touches[0].y);
                  if(ki>=0){
                      g_sel=ki;
                      tap_flash=ki; flash_frames=0;
                      char v=g_keys[ki].val;
                      if(v==KEY_OK){ if(len>0){memcpy(out,buf,len);out[len]='\0';return true;} }
                      else if(v==KEY_BS){ if(len>0)buf[--len]='\0'; }
                      else{ if(len<(int)(outsz-1)){buf[len++]=v;buf[len]='\0';} }
                  }
              }
          }
        }

        /* B = backspace */
        if(down&HidNpadButton_B){ if(len>0)buf[--len]='\0'; }
        /* + = confirm */
        if(down&HidNpadButton_Plus){ if(len>0){memcpy(out,buf,len);out[len]='\0';return true;} }
        /* - = cancel */
        if(down&HidNpadButton_Minus) return false;

        svcSleepThread(16000000LL);
    }
    return false;
}

/* ═══════════════════════════════════════════════════════════════════════ */
/*  Sysmodule Status                                                      */
/* ═══════════════════════════════════════════════════════════════════════ */
typedef struct {
    bool active;
    char relay[128];
    char error_msg[128];
    uint64_t up_b, dn_b;
} SysStatus;

static void read_status(SysStatus *s)
{
    memset(s, 0, sizeof(*s));
    FILE *f = fopen("sdmc:/tmp/lanplay.status", "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "active=1", 8) == 0) s->active = true;
        if (strncmp(line, "error=", 6) == 0) {
            strncpy(s->error_msg, line + 6, sizeof(s->error_msg) - 1);
            char *n = strchr(s->error_msg, '\n'); if (n) *n = '\0';
        }
        if (strncmp(line, "relay=", 6) == 0) {
            strncpy(s->relay, line + 6, sizeof(s->relay) - 1);
            char *n = strchr(s->relay, '\n'); if (n) *n = '\0';
        }
        if (strncmp(line, "up_bytes=", 9) == 0) s->up_b = strtoull(line + 9, NULL, 10);
        if (strncmp(line, "dn_bytes=", 9) == 0) s->dn_b = strtoull(line + 9, NULL, 10);
    }
    fclose(f);
}

static void trigger_reload(void)
{
    mkdir("sdmc:/tmp", 0777);
    FILE *f = fopen("sdmc:/tmp/lanplay.reload", "w");
    if (f) { fprintf(f, "1"); fclose(f); }
}

/* ═══════════════════════════════════════════════════════════════════════ */
/*  Main menu screen                                                      */
/* ═══════════════════════════════════════════════════════════════════════ */
static bool menu_screen(const char *current)
{
    SysStatus status;
    u64 last_update = 0;

    while(appletMainLoop()){
        /* Update status every second */
        u64 now = armTicksToNs(armGetSystemTick());
        if (now - last_update > 1000000000ULL) {
            read_status(&status);
            last_update = now;
        }

        g_buf=(u32*)framebufferBegin(&g_fb,&g_stride);
        fill_screen(C_BG);

        /* Title bar */
        fill_rect(0, 0, FB_W, 48, C_TITLE_BG);
        draw_text_c(12, "LAN Play Setup", C_TXT, 3);

        /* Config Status */
        draw_text(100, 80, "Configured Relay:", C_TXT_DIM, 2);
        draw_text(100, 110, (current && current[0]) ? current : "None", 
                  (current && current[0]) ? C_GREEN_T : C_RED_T, 3);

        /* Sysmodule Status */
        fill_rect(FB_W - 450, 80, 350, 150, C_FIELD_BG);
        draw_outline(FB_W - 450, 80, 350, 150, C_FIELD_BD, 2);
        draw_text(FB_W - 440, 90, "Background Service:", C_TXT_DIM, 2);
        if (status.active) {
            draw_text(FB_W - 440, 120, "Connected to:", C_TXT_DIM, 2);
            draw_text(FB_W - 440, 150, status.relay, C_GREEN_T, 2);
            char up[64], dn[64];
            snprintf(up, 64, "Sent: %llu KB", (unsigned long long)(status.up_b / 1024));
            snprintf(dn, 64, "Recv: %llu KB", (unsigned long long)(status.dn_b / 1024));
            draw_text(FB_W - 440, 185, up, C_TXT, 2);
            draw_text(FB_W - 440, 210, dn, C_TXT, 2);
        } else {
            draw_text(FB_W - 440, 130, "NOT RUNNING", C_RED_T, 3);
            if (status.error_msg[0]) {
                draw_text(FB_W - 440, 180, status.error_msg, C_RED_T, 2);
            } else {
                draw_text(FB_W - 440, 180, "Reboot console to start", C_TXT_DIM, 2);
            }
        }

        /* Buttons */
        int bw=350, bh=80, gap=50;
        int bx=(FB_W-bw*2-gap)/2; /* Only 2 buttons now: Configure and Exit */

        /* A: Configure */
        draw_btn(bx, 340, bw, bh, C_DONE, C_DONE_HI, C_DONE_LO);
        draw_text(bx+(bw-text_w("A: Configure",3))/2, 340+(bh-24)/2, "A: Configure", C_TXT, 3);

        /* B: Exit */
        draw_btn(bx+bw+gap, 340, bw, bh, C_DEL, C_DEL_HI, C_DEL_LO);
        draw_text(bx+bw+gap+(bw-text_w("B: Exit",3))/2, 340+(bh-24)/2, "B: Exit", C_TXT, 3);

        /* Hint */
        draw_text_c(450, "The sysmodule background service restarts automatically when you configure it.", C_TXT_DIM, 2);
        draw_text_c(550, "Selection: D-Pad   Confirm: A/B/X buttons", C_TXT_DIM, 2);

        framebufferEnd(&g_fb);

        padUpdate(&g_pad);
        u64 dn=padGetButtonsDown(&g_pad);
        if(dn&HidNpadButton_A) return true;
        if(dn&HidNpadButton_B) return false;
        if(dn&HidNpadButton_Plus) return false;

        /* Touch */
        { HidTouchScreenState ts={0};
          if(hidGetTouchScreenStates(&ts,1)&&ts.count>0){
              int tx=ts.touches[0].x, ty=ts.touches[0].y;
              if(ty>=340&&ty<420){
                  if(tx>=bx&&tx<bx+bw) return true;
                  if(tx>=bx+bw+gap&&tx<bx+bw*2+gap) return false;
              }
          }
        }

        svcSleepThread(16000000LL);
    }
    return false;
}

/* ═══════════════════════════════════════════════════════════════════════ */
/*  Result screen                                                         */
/* ═══════════════════════════════════════════════════════════════════════ */
static void result_screen(const char *addr, bool ok)
{
    while(appletMainLoop()){
        g_buf=(u32*)framebufferBegin(&g_fb,&g_stride);
        fill_screen(C_BG);
        fill_rect(0, 0, FB_W, 48, C_TITLE_BG);
        draw_text_c(12, "LAN Play Setup", C_TXT, 3);

        if(ok){
            draw_text_c(140, "Saved successfully!", C_GREEN_T, 4);
            draw_text_c(210, addr, C_TXT, 3);
            draw_text_c(300, "Next steps:", C_TXT_DIM, 3);
            draw_text_c(340, "1. Press HOME to return", C_TXT_DIM, 2);
            draw_text_c(370, "2. Restart the Switch (POWER > Restart)", C_TXT_DIM, 2);
            draw_text_c(400, "3. The sysmodule connects automatically", C_TXT_DIM, 2);
        } else {
            draw_text_c(200, "Failed to write config!", C_RED_T, 4);
            draw_text_c(280, "Check that the SD card is writable.", C_TXT_DIM, 3);
        }

        draw_text_c(550, "Press any button to exit", C_TXT_DIM, 3);
        framebufferEnd(&g_fb);

        padUpdate(&g_pad);
        if(padGetButtonsDown(&g_pad)) break;

        HidTouchScreenState ts={0};
        if(hidGetTouchScreenStates(&ts,1)&&ts.count>0) break;

        svcSleepThread(16000000LL);
    }
}

/* ═══════════════════════════════════════════════════════════════════════ */
/*  Entry point                                                           */
/* ═══════════════════════════════════════════════════════════════════════ */
int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    /* Mount SD card for config file I/O */
    fsdevMountSdmc();

    /* Create linear double-buffered framebuffer (NO consoleInit!) */
    NWindow *win = nwindowGetDefault();
    framebufferCreate(&g_fb, win, FB_W, FB_H, PIXEL_FORMAT_RGBA_8888, 2);
    framebufferMakeLinear(&g_fb);

    /* Input */
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
    hidInitializeTouchScreen();

    while (appletMainLoop()) {
        /* Read current config */
        char current[STR_MAX]={0};
        read_relay(current, sizeof(current));

        /* Main menu */
        if (!menu_screen(current)) break;

        /* Keyboard */
        char relay[STR_MAX]={0};
        if (keyboard_screen(current, relay, sizeof(relay))) {
            bool ok = (save_relay(relay) == 0);
            if (ok) {
                trigger_reload();
                /* Go directly back to menu to see updated status */
            } else {
                result_screen(relay, ok);
            }
        }
    }

    framebufferClose(&g_fb);
    fsdevUnmountAll();
    return 0;
}
