#ifndef PK_H
#define PK_H

#include <stdbool.h>
#include <stdint.h>

#define PK_WIDTH 1024
#define PK_HEIGHT 768
#define PK_MARGIN 48
#define PK_RIGHT_EDGE (PK_WIDTH - PK_MARGIN)
#define PK_PROMPT_Y 684

typedef uint32_t pk_color;

typedef enum { PK_REGULAR, PK_MEDIUM, PK_SEMIBOLD, PK_BOLD, PK_MONO, PK_FONT_COUNT } pk_font;
typedef enum { PK_LEFT, PK_CENTER, PK_RIGHT } pk_align;

typedef enum {
    PK_CANVAS_MIDNIGHT, PK_CANVAS_BLACK, PK_CANVAS_GRAY, PK_CANVAS_COUNT
} pk_canvas;

typedef enum {
    PK_ACCENT_INDIGO, PK_ACCENT_BLUE, PK_ACCENT_VIOLET, PK_ACCENT_TEAL, PK_ACCENT_GREEN,
    PK_ACCENT_YELLOW, PK_ACCENT_ORANGE, PK_ACCENT_RED, PK_ACCENT_PINK, PK_ACCENT_COUNT
} pk_accent;

typedef struct {
    pk_color canvas, card, line, accent, focus_fill, text, muted, faint;
} pk_theme;

typedef struct {
    int width, height;
    uint8_t *alpha;
} pk_image;

typedef struct {
    char letter;
    const char *meaning;
} pk_prompt;

extern uint32_t pk_pixels[PK_WIDTH * PK_HEIGHT];
extern pk_theme pk_style;

extern const char *const pk_canvas_names[PK_CANVAS_COUNT];
extern const char *const pk_accent_names[PK_ACCENT_COUNT];
extern const pk_color pk_canvas_colors[PK_CANVAS_COUNT];
extern const pk_color pk_accent_colors[PK_ACCENT_COUNT];

bool pk_init(const char *assets_dir);
void pk_set_theme(pk_canvas canvas, pk_accent accent);
bool pk_image_load(pk_image *image, const char *path);
bool pk_save_png(const char *path);

void pk_clear(void);
void pk_clip(int top, int bottom);
void pk_round_rect(float x, float y, float w, float h, float radius, pk_color color);
void pk_round_outline(float x, float y, float w, float h, float radius, float thickness, pk_color color);
void pk_circle(float cx, float cy, float radius, pk_color color);
void pk_ring(float cx, float cy, float radius, float thickness, pk_color color);
void pk_line(float x0, float y0, float x1, float y1, float width, pk_color color);
void pk_image_draw(const pk_image *image, int x, int y);

float pk_text_width(pk_font font, int size, const char *text);
void pk_text(pk_font font, int size, float x, float middle_y, pk_align align, pk_color color, const char *text);
int pk_text_wrapped(pk_font font, int size, float x, float top_y, float max_width, int line_height,
                    pk_color color, const char *text);

int pk_text_lines(pk_font font, int size, float max_width, const char *text);

void pk_battery(float x, float y, int percent);
void pk_tab_bar(const char *const *tabs, int count, int active, int badge_tab, int badge, const char *clock, int battery);
void pk_header(const char *title, const char *right, int battery);
void pk_prompts(const pk_prompt *prompts, int count);
void pk_card(float y, float height, bool focused);
void pk_row(float y, const char *title, const char *subtitle, const char *meta, bool focused);
void pk_tile(float x, float y, const char *title, const char *value, const char *note, bool focused);
void pk_rays(float cx, float cy, float inner, float reach, float energy, float time);

#endif
