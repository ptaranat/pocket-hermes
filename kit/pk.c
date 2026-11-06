#include "pk.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define GLYPH_SLOTS 4096

typedef struct {
    uint32_t key;
    int width, height, x_offset, y_offset, advance;
    uint8_t *bitmap;
} glyph;

typedef struct {
    stbtt_fontinfo info;
    uint8_t *data;
    glyph slots[GLYPH_SLOTS];
} font_face;

uint32_t pk_pixels[PK_WIDTH * PK_HEIGHT];
pk_theme pk_style;

const char *const pk_canvas_names[PK_CANVAS_COUNT] = {"Midnight", "Black", "Gray"};
const char *const pk_accent_names[PK_ACCENT_COUNT] = {
    "Indigo", "Blue", "Violet", "Teal", "Green", "Yellow", "Orange", "Red", "Pink"};
const pk_color pk_canvas_colors[PK_CANVAS_COUNT] = {0x000022, 0x000000, 0x1c1c1e};
const pk_color pk_accent_colors[PK_ACCENT_COUNT] = {
    0x517af6, 0x3a82f7, 0x7d81ef, 0x67c8ce, 0x82d68a, 0xeccf62, 0xfca04c, 0xed7470, 0xef87b1};

static const char *const font_files[PK_FONT_COUNT] = {
    "fonts/InterTight-400.ttf", "fonts/InterTight-500.ttf", "fonts/InterTight-600.ttf",
    "fonts/InterTight-700.ttf", "fonts/IBMPlexMono-Medium.ttf"};

static font_face faces[PK_FONT_COUNT];
static int clip_top = 0, clip_bottom = PK_HEIGHT;

static pk_color mix(pk_color top, pk_color under, float amount) {
    pk_color out = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        float a = (float)((top >> shift) & 255), b = (float)((under >> shift) & 255);
        out |= (pk_color)(a * amount + b * (1 - amount) + 0.5f) << shift;
    }
    return out;
}

static inline void plot(int x, int y, pk_color color, uint32_t coverage) {
    if (x < 0 || y < clip_top || x >= PK_WIDTH || y >= clip_bottom || coverage == 0) return;
    uint32_t *pixel = &pk_pixels[y * PK_WIDTH + x];
    if (coverage >= 255) {
        *pixel = color | 0xff000000;
        return;
    }
    uint32_t inverse = 255 - coverage, under = *pixel;
    uint32_t rb = ((color & 0xff00ff) * coverage + (under & 0xff00ff) * inverse) >> 8;
    uint32_t g = ((color & 0x00ff00) * coverage + (under & 0x00ff00) * inverse) >> 8;
    *pixel = 0xff000000 | (rb & 0xff00ff) | (g & 0x00ff00);
}

static inline float clamp01(float value) {
    return value < 0 ? 0 : value > 1 ? 1 : value;
}

static float round_rect_distance(float px, float py, float x, float y, float w, float h, float radius) {
    float half_w = w / 2, half_h = h / 2;
    float qx = fabsf(px - (x + half_w)) - (half_w - radius);
    float qy = fabsf(py - (y + half_h)) - (half_h - radius);
    float outside = hypotf(fmaxf(qx, 0), fmaxf(qy, 0));
    return outside + fminf(fmaxf(qx, qy), 0) - radius;
}

static uint8_t *read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    rewind(file);
    uint8_t *data = malloc((size_t)size);
    if (data && fread(data, 1, (size_t)size, file) != (size_t)size) {
        free(data);
        data = NULL;
    }
    fclose(file);
    return data;
}

bool pk_init(const char *assets_dir) {
    char path[512];
    for (int i = 0; i < PK_FONT_COUNT; i++) {
        snprintf(path, sizeof path, "%s/%s", assets_dir, font_files[i]);
        faces[i].data = read_file(path);
        if (!faces[i].data || !stbtt_InitFont(&faces[i].info, faces[i].data, 0)) {
            fprintf(stderr, "could not load font %s\n", path);
            return false;
        }
    }
    pk_set_theme(PK_CANVAS_BLACK, PK_ACCENT_ORANGE);
    return true;
}

void pk_set_theme(pk_canvas canvas, pk_accent accent) {
    pk_color base = pk_canvas_colors[canvas], white = 0xffffff;
    pk_style = (pk_theme){
        .canvas = base,
        .card = mix(white, base, 0.09f),
        .line = mix(white, base, 0.17f),
        .accent = pk_accent_colors[accent],
        .focus_fill = mix(pk_accent_colors[accent], base, 0.16f),
        .text = white,
        .muted = 0x929298,
        .faint = 0x5c5c62,
    };
}

bool pk_image_load(pk_image *image, const char *path) {
    int channels;
    uint8_t *rgba = stbi_load(path, &image->width, &image->height, &channels, 4);
    if (!rgba) return false;
    size_t count = (size_t)image->width * (size_t)image->height;
    image->alpha = malloc(count);
    for (size_t i = 0; i < count; i++) image->alpha[i] = rgba[i * 4 + 3];
    stbi_image_free(rgba);
    return true;
}

bool pk_save_png(const char *path) {
    static uint8_t rgb[PK_WIDTH * PK_HEIGHT * 3];
    for (int i = 0; i < PK_WIDTH * PK_HEIGHT; i++) {
        rgb[i * 3] = (uint8_t)(pk_pixels[i] >> 16);
        rgb[i * 3 + 1] = (uint8_t)(pk_pixels[i] >> 8);
        rgb[i * 3 + 2] = (uint8_t)pk_pixels[i];
    }
    return stbi_write_png(path, PK_WIDTH, PK_HEIGHT, 3, rgb, PK_WIDTH * 3) != 0;
}

void pk_clip(int top, int bottom) {
    clip_top = top;
    clip_bottom = bottom;
}

void pk_clear(void) {
    uint32_t value = pk_style.canvas | 0xff000000;
    for (int i = 0; i < PK_WIDTH * PK_HEIGHT; i++) pk_pixels[i] = value;
}

void pk_round_rect(float x, float y, float w, float h, float radius, pk_color color) {
    int top = (int)floorf(y), bottom = (int)ceilf(y + h), left = (int)floorf(x), right = (int)ceilf(x + w);
    for (int py = top; py < bottom; py++) {
        for (int px = left; px < right; px++) {
            float distance = round_rect_distance((float)px + 0.5f, (float)py + 0.5f, x, y, w, h, radius);
            plot(px, py, color, (uint32_t)(clamp01(0.5f - distance) * 255));
        }
    }
}

void pk_round_outline(float x, float y, float w, float h, float radius, float thickness, pk_color color) {
    int top = (int)floorf(y), bottom = (int)ceilf(y + h), left = (int)floorf(x), right = (int)ceilf(x + w);
    for (int py = top; py < bottom; py++) {
        for (int px = left; px < right; px++) {
            float distance = round_rect_distance((float)px + 0.5f, (float)py + 0.5f, x, y, w, h, radius);
            float coverage = clamp01(0.5f - distance) * clamp01(0.5f + distance + thickness);
            plot(px, py, color, (uint32_t)(coverage * 255));
        }
    }
}

void pk_circle(float cx, float cy, float radius, pk_color color) {
    pk_round_rect(cx - radius, cy - radius, radius * 2, radius * 2, radius, color);
}

void pk_ring(float cx, float cy, float radius, float thickness, pk_color color) {
    pk_round_outline(cx - radius, cy - radius, radius * 2, radius * 2, radius, thickness, color);
}

void pk_line(float x0, float y0, float x1, float y1, float width, pk_color color) {
    float dx = x1 - x0, dy = y1 - y0, length_squared = dx * dx + dy * dy, half = width / 2;
    bool steep = fabsf(dy) > fabsf(dx);
    float start = steep ? fminf(y0, y1) : fminf(x0, x1), end = steep ? fmaxf(y0, y1) : fmaxf(x0, x1);
    float slope = steep ? (dy != 0 ? dx / dy : 0) : (dx != 0 ? dy / dx : 0);
    float band = half * sqrtf(1 + slope * slope) + 1.5f;
    for (int major = (int)floorf(start - half - 1); major <= (int)ceilf(end + half + 1); major++) {
        float center = steep ? x0 + ((float)major + 0.5f - y0) * slope : y0 + ((float)major + 0.5f - x0) * slope;
        for (int minor = (int)floorf(center - band); minor <= (int)ceilf(center + band); minor++) {
            int px = steep ? minor : major, py = steep ? major : minor;
            float fx = (float)px + 0.5f - x0, fy = (float)py + 0.5f - y0;
            float along = length_squared > 0 ? clamp01((fx * dx + fy * dy) / length_squared) : 0;
            float distance = hypotf(fx - along * dx, fy - along * dy);
            plot(px, py, color, (uint32_t)(clamp01(half + 0.5f - distance) * 255));
        }
    }
}

void pk_image_draw(const pk_image *image, int x, int y) {
    for (int row = 0; row < image->height; row++) {
        const uint8_t *alpha = image->alpha + (size_t)row * (size_t)image->width;
        for (int col = 0; col < image->width; col++) plot(x + col, y + row, pk_style.text, alpha[col]);
    }
}

static glyph *glyph_for(pk_font font, int size, int codepoint) {
    font_face *face = &faces[font];
    uint32_t key = ((uint32_t)size << 21) | (uint32_t)codepoint;
    uint32_t slot = (key * 2654435761u) % GLYPH_SLOTS;
    while (face->slots[slot].key && face->slots[slot].key != key) slot = (slot + 1) % GLYPH_SLOTS;
    glyph *cached = &face->slots[slot];
    if (cached->key == key) return cached;
    float scale = stbtt_ScaleForPixelHeight(&face->info, (float)size);
    int advance, bearing;
    stbtt_GetCodepointHMetrics(&face->info, codepoint, &advance, &bearing);
    cached->key = key;
    cached->advance = (int)lroundf((float)advance * scale);
    cached->bitmap = stbtt_GetCodepointBitmap(&face->info, 0, scale, codepoint, &cached->width, &cached->height,
                                              &cached->x_offset, &cached->y_offset);
    return cached;
}

static int next_codepoint(const char **cursor) {
    const uint8_t *s = (const uint8_t *)*cursor;
    int codepoint = *s, extra = 0;
    if (*s >= 0xf0) { codepoint = *s & 0x07; extra = 3; }
    else if (*s >= 0xe0) { codepoint = *s & 0x0f; extra = 2; }
    else if (*s >= 0xc0) { codepoint = *s & 0x1f; extra = 1; }
    s++;
    while (extra-- > 0 && (*s & 0xc0) == 0x80) codepoint = (codepoint << 6) | (*s++ & 0x3f);
    *cursor = (const char *)s;
    return codepoint;
}

float pk_text_width(pk_font font, int size, const char *text) {
    float width = 0, scale = stbtt_ScaleForPixelHeight(&faces[font].info, (float)size);
    int previous = 0;
    while (*text) {
        int codepoint = next_codepoint(&text);
        if (previous) width += (float)stbtt_GetCodepointKernAdvance(&faces[font].info, previous, codepoint) * scale;
        width += (float)glyph_for(font, size, codepoint)->advance;
        previous = codepoint;
    }
    return width;
}

void pk_text(pk_font font, int size, float x, float middle_y, pk_align align, pk_color color, const char *text) {
    float scale = stbtt_ScaleForPixelHeight(&faces[font].info, (float)size);
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&faces[font].info, &ascent, &descent, &gap);
    float baseline = middle_y + (float)(ascent + descent) * scale / 2;
    if (align == PK_RIGHT) x -= pk_text_width(font, size, text);
    if (align == PK_CENTER) x -= pk_text_width(font, size, text) / 2;
    int previous = 0;
    while (*text) {
        int codepoint = next_codepoint(&text);
        if (previous) x += (float)stbtt_GetCodepointKernAdvance(&faces[font].info, previous, codepoint) * scale;
        glyph *g = glyph_for(font, size, codepoint);
        int origin_x = (int)lroundf(x) + g->x_offset, origin_y = (int)lroundf(baseline) + g->y_offset;
        for (int row = 0; row < g->height; row++) {
            for (int col = 0; col < g->width; col++) {
                plot(origin_x + col, origin_y + row, color, g->bitmap[row * g->width + col]);
            }
        }
        x += (float)g->advance;
        previous = codepoint;
    }
}

static int layout_wrapped(pk_font font, int size, float x, float top_y, float max_width, int line_height,
                          pk_color color, const char *text, bool visible) {
    char line[512] = "", trial[512];
    int lines = 0;
    const char *cursor = text;
    while (*cursor) {
        const char *end = strchr(cursor, ' ');
        size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
        if (length > 400) length = 400;
        snprintf(trial, sizeof trial, "%s%s%.*s", line, line[0] ? " " : "", (int)length, cursor);
        if (line[0] && pk_text_width(font, size, trial) > max_width) {
            if (visible) pk_text(font, size, x, top_y + (float)(lines * line_height) + (float)line_height / 2, PK_LEFT, color, line);
            lines++;
            snprintf(line, sizeof line, "%.*s", (int)length, cursor);
        } else {
            memcpy(line, trial, sizeof line);
        }
        cursor += length;
        while (*cursor == ' ') cursor++;
    }
    if (line[0]) {
        if (visible) pk_text(font, size, x, top_y + (float)(lines * line_height) + (float)line_height / 2, PK_LEFT, color, line);
        lines++;
    }
    return lines;
}

int pk_text_wrapped(pk_font font, int size, float x, float top_y, float max_width, int line_height,
                    pk_color color, const char *text) {
    return layout_wrapped(font, size, x, top_y, max_width, line_height, color, text, true);
}

int pk_text_lines(pk_font font, int size, float max_width, const char *text) {
    return layout_wrapped(font, size, 0, 0, max_width, 0, 0, text, false);
}

void pk_battery(float x, float y, int percent) {
    pk_round_outline(x, y, 56, 28, 7, 2, pk_style.text);
    pk_round_rect(x + 5, y + 5, 46.0f * (float)percent / 100.0f, 18, 3, pk_style.text);
    pk_round_rect(x + 59, y + 9, 4, 10, 2, pk_style.text);
}

static void shoulder(float x, float y, const char *letter) {
    pk_round_outline(x, y - 21, 42, 42, 10, 2, pk_style.faint);
    pk_text(PK_SEMIBOLD, 26, x + 21, y, PK_CENTER, pk_style.muted, letter);
}

void pk_tab_bar(const char *const *tabs, int count, int active, int badge_tab, int badge, const char *clock, int battery) {
    float y = 54, x = 112;
    shoulder(PK_MARGIN, y, "L");
    for (int i = 0; i < count; i++) {
        bool chosen = i == active;
        pk_font font = chosen ? PK_SEMIBOLD : PK_REGULAR;
        float width = pk_text_width(font, 40, tabs[i]);
        pk_text(font, 40, x, y, PK_LEFT, chosen ? pk_style.text : pk_style.muted, tabs[i]);
        if (chosen) pk_line(x, 88, x + width, 88, 4, pk_style.text);
        x += width;
        if (i == badge_tab && badge > 0) {
            char count_text[8];
            snprintf(count_text, sizeof count_text, "%d", badge);
            pk_circle(x + 22, y - 10, 14, pk_style.accent);
            pk_text(PK_BOLD, 22, x + 22, y - 10, PK_CENTER, 0x000000, count_text);
            x += 28;
        }
        x += 40;
    }
    shoulder(x - 6, y, "R");
    pk_text(PK_MEDIUM, 30, 884, y, PK_RIGHT, pk_style.text, clock);
    pk_battery(900, y - 14, battery);
}

void pk_header(const char *title, const char *right, int battery) {
    char fitted[128];
    snprintf(fitted, sizeof fitted, "%s", title);
    size_t length = strlen(fitted);
    while (length > 4 && pk_text_width(PK_MEDIUM, 34, fitted) > 700) {
        length--;
        while (length > 0 && (fitted[length] & 0xc0) == 0x80) length--;
        memcpy(fitted + length, "...", 4);
        if (pk_text_width(PK_MEDIUM, 34, fitted) <= 700) break;
    }
    pk_text(PK_MEDIUM, 34, PK_MARGIN, 54, PK_LEFT, pk_style.muted, fitted);
    pk_text(PK_MEDIUM, 30, 884, 54, PK_RIGHT, pk_style.text, right);
    pk_battery(900, 40, battery);
}

void pk_prompts(const pk_prompt *prompts, int count) {
    float x = PK_RIGHT_EDGE;
    for (int i = 0; i < count; i++) {
        char letter[2] = {prompts[i].letter, 0};
        x -= 62 + pk_text_width(PK_MEDIUM, 30, prompts[i].meaning);
        pk_ring(x + 24, PK_PROMPT_Y + 24, 24, 2, pk_style.text);
        pk_text(PK_SEMIBOLD, 28, x + 24, PK_PROMPT_Y + 24, PK_CENTER, pk_style.text, letter);
        pk_text(PK_MEDIUM, 30, x + 62, PK_PROMPT_Y + 24, PK_LEFT, pk_style.text, prompts[i].meaning);
        x -= 40;
    }
}

void pk_card(float y, float height, bool focused) {
    pk_round_rect(PK_MARGIN, y, PK_RIGHT_EDGE - PK_MARGIN, height, 22, focused ? pk_style.focus_fill : pk_style.card);
    if (focused) pk_round_outline(PK_MARGIN, y, PK_RIGHT_EDGE - PK_MARGIN, height, 22, 4, pk_style.accent);
}

void pk_row(float y, const char *title, const char *subtitle, const char *meta, bool focused) {
    pk_card(y, 128, focused);
    pk_text(PK_MEDIUM, 42, 84, y + 42, PK_LEFT, pk_style.text, title);
    pk_text(PK_MEDIUM, 30, 84, y + 92, PK_LEFT, pk_style.muted, subtitle);
    pk_text(PK_MEDIUM, 30, 940, y + 92, PK_RIGHT, focused ? pk_style.text : pk_style.muted, meta);
}

void pk_tile(float x, float y, const char *title, const char *value, const char *note, bool focused) {
    pk_round_rect(x, y, 454, 266, 24, focused ? pk_style.focus_fill : pk_style.card);
    if (focused) pk_round_outline(x, y, 454, 266, 24, 4, pk_style.accent);
    pk_text(PK_MEDIUM, 32, x + 30, y + 46, PK_LEFT, pk_style.muted, title);
    int size = 72;
    while (size > 36 && pk_text_width(PK_MEDIUM, size, value) > 394) size -= 4;
    pk_text(PK_MEDIUM, size, x + 30, y + 128, PK_LEFT, pk_style.text, value);
    pk_text(PK_MEDIUM, 32, x + 30, y + 214, PK_LEFT, pk_style.muted, note);
}

void pk_rays(float cx, float cy, float inner, float reach, float energy, float time) {
    const int count = 220;
    const float tau = 6.2831853f;
    for (int i = 0; i < count; i++) {
        float angle = (float)i / (float)count * tau, wave = 0;
        for (int k = 0; k < 5; k++) wave += sinf(angle * (float)(k * 3 + 2) + time * (0.7f + (float)k * 0.31f) + (float)k * 1.7f);
        wave = 0.5f + wave / 10.0f;
        float grain = 0.55f + 0.45f * (0.5f + 0.5f * sinf((float)i * 12.9898f));
        float length = reach * (0.16f + energy * powf(wave, 1.6f) * grain);
        float start = inner + 10, end = start + length, dx = cosf(angle), dy = sinf(angle);
        pk_line(cx + dx * start, cy + dy * start, cx + dx * end, cy + dy * end, 1.6f, pk_style.text);
    }
}
