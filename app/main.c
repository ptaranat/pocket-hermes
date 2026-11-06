#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "hub.h"
#include "pk.h"
#include "voice.h"

#define TAB_COUNT 4
#define CONFIG_VALUE_MAX 256
#define REPLY_TOP 110
#define REPLY_BOTTOM 620
#define REPLY_LINE 80
#define REPLY_SIZE 62
#define VISIBLE_ROWS 4
#define ROW_PITCH 140
#define LIST_TOP 116
#define READER_TOP 104
#define READER_BOTTOM 664
#define READER_LINE 44
#define READER_SIZE 34
#define READER_SCROLL 180
#define INBOX_REFRESH_MS 60000
#define HOLD_DELAY_MS 280
#define HOLD_REPEAT_MS 110
#define HOLD_SCROLL_PER_MS 0.9f
#define BUBBLE_MAX_TEXT 680
#define BUBBLE_PAD_X 26
#define BUBBLE_PAD_Y 16
#define MESSAGE_GAP 30
#define APPROVAL_SECONDS 60

typedef enum { TAB_TALK, TAB_CHATS, TAB_INBOX, TAB_AGENT } tab_id;
typedef enum { VIEW_TABS, VIEW_LISTENING, VIEW_WORKING, VIEW_REPLY, VIEW_ERROR, VIEW_APPROVAL, VIEW_LOOK, VIEW_CHAT, VIEW_NOTE } view_id;
typedef enum { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_L, BTN_R, BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_MENU, BTN_COUNT } button_id;
typedef enum { ANIM_GRID, ANIM_ORB, ANIM_MIST, ANIM_COUNT } working_animation;

static const char *const tab_names[TAB_COUNT] = {"Talk", "Chats", "Inbox", "Agent"};
static const char *const animation_names[ANIM_COUNT] = {"Grid", "Orb", "Mist"};

static const hub_item demo_chats[] = {
    {"a", "Weekend trip planning", "Telegram", "7:30 PM", false},
    {"b", "Home server backups", "Desktop", "5:12 PM", false},
    {"c", "Reading list ideas", "Telegram", "Yesterday", false},
    {"d", "Fixing the docs build", "CLI", "Monday", false},
};
static const hub_item demo_inbox[] = {
    {"a", "Morning summary finished", "Routine", "8:00 AM", true},
    {"b", "Nightly backup failed", "Routine", "Yesterday", false},
};

static struct {
    view_id view;
    tab_id tab;
    int selection[TAB_COUNT];
    int look_row;
    pk_canvas canvas;
    pk_accent accent;
    working_animation animation;
    bool speak;
    view_id listen_origin;
    bool demo, running, dirty;
    hub_list chats, inbox, agent;
    hub_chat chat;
    hub_note note;
    float reader_scroll;
    int reader_height;
    int message_heights[HUB_MAX_MESSAGES];
    int held_button;
    Uint32 held_since, held_repeat, held_tick;
    bool reader_at_end;
    bool inbox_requested;
    Uint32 inbox_refreshed;
    Uint32 view_started;
    Uint32 reply_started;
    float reply_scroll;
    int reply_lines;
    turn_state turn;
    pk_image orb_idle, orb_listening, orb_small;
    char data_dir[CONFIG_VALUE_MAX];
    int joystick_buttons[BTN_COUNT];
} app = {.held_button = -1, .canvas = PK_CANVAS_BLACK, .accent = PK_ACCENT_ORANGE, .animation = ANIM_ORB, .running = true, .dirty = true};

static void go_to(view_id view) {
    app.view = view;
    app.view_started = SDL_GetTicks();
    app.dirty = true;
}

static const char *clock_text(void) {
    static char text[16];
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    int hour = local.tm_hour % 12 ? local.tm_hour % 12 : 12;
    snprintf(text, sizeof text, "%d:%02d", app.demo ? 7 : hour, app.demo ? 42 : local.tm_min);
    return text;
}

static int battery_percent(void) {
    if (app.demo) return 82;
    FILE *file = fopen("/sys/class/power_supply/axp2202-battery/capacity", "r");
    int percent = 100;
    if (file) {
        if (fscanf(file, "%d", &percent) != 1) percent = 100;
        fclose(file);
    }
    return percent;
}

static const char *elapsed_text(void) {
    static char text[16];
    Uint32 seconds = app.demo ? 4 : (SDL_GetTicks() - app.view_started) / 1000;
    snprintf(text, sizeof text, "%u:%02u", seconds / 60, seconds % 60);
    return text;
}

static void look_path(char *path, size_t size) {
    snprintf(path, size, "%s/look", app.data_dir);
}

static void save_look(void) {
    char path[CONFIG_VALUE_MAX + 8];
    look_path(path, sizeof path);
    FILE *file = fopen(path, "w");
    if (!file) return;
    fprintf(file, "%d %d %d %d\n", app.canvas, app.accent, app.animation, app.speak ? 1 : 0);
    fclose(file);
}

static void load_look(void) {
    char path[CONFIG_VALUE_MAX + 8];
    look_path(path, sizeof path);
    FILE *file = fopen(path, "r");
    int canvas, accent, animation, speak = 0;
    if (file) {
        if (fscanf(file, "%d %d %d %d", &canvas, &accent, &animation, &speak) >= 3 && canvas >= 0 && canvas < PK_CANVAS_COUNT &&
            accent >= 0 && accent < PK_ACCENT_COUNT && animation >= 0 && animation < ANIM_COUNT) {
            app.canvas = canvas;
            app.accent = accent;
            app.animation = animation;
            app.speak = speak != 0;
        }
        fclose(file);
    }
    pk_set_theme(app.canvas, app.accent);
    voice_set_speak(app.speak);
}

static void draw_tabs(void) {
    pk_tab_bar(tab_names, TAB_COUNT, app.tab, TAB_INBOX, app.inbox.unread, clock_text(), battery_percent());
}

static void draw_empty(const char *message) {
    pk_text(PK_REGULAR, 44, PK_WIDTH / 2.0f, 360, PK_CENTER, pk_style.muted, message);
}

static void draw_talk(void) {
    pk_image_draw(&app.orb_idle, 512 - 180, 335 - 180);
    pk_text(PK_REGULAR, 44, 512, 580, PK_CENTER, pk_style.muted, "Hold A to talk");
    pk_prompts((pk_prompt[]){{'Y', "New chat"}}, 1);
}

static int first_visible(int selection, int count) {
    int first = selection - VISIBLE_ROWS + 1;
    if (first < 0) first = 0;
    if (first > count - VISIBLE_ROWS && count > VISIBLE_ROWS) first = count - VISIBLE_ROWS;
    return first;
}

static void draw_list(const hub_list *list, int selection, bool show_unread) {
    int first = first_visible(selection, list->count);
    for (int i = first; i < list->count && i < first + VISIBLE_ROWS; i++) {
        float y = LIST_TOP + (float)(i - first) * ROW_PITCH;
        const hub_item *item = &list->items[i];
        pk_row(y, item->title, item->subtitle, item->meta, i == selection);
        if (show_unread && item->unread) pk_circle(940, y + 42, 8, pk_style.accent);
    }
}

static bool draw_list_state(hub_status status, int count, const char *failed, const char *empty) {
    if (count > 0) return true;
    if (status == HUB_FAILED) draw_empty(failed);
    else if (status == HUB_READY) draw_empty(empty);
    else draw_empty("Loading");
    return false;
}

static void draw_chats(void) {
    if (draw_list_state(app.chats.status, app.chats.count, "Hermes is offline right now", "No chats yet")) {
        draw_list(&app.chats, app.selection[TAB_CHATS], false);
        pk_prompts((pk_prompt[]){{'A', "Open"}, {'Y', "New chat"}}, 2);
    }
}

static void draw_demo_approval_card(void) {
    pk_card(116, 224, true);
    pk_text(PK_SEMIBOLD, 32, 84, 160, PK_LEFT, pk_style.muted, "Approval needed");
    pk_text(PK_SEMIBOLD, 32, 940, 160, PK_RIGHT, pk_style.text, "0:48");
    pk_text(PK_MONO, 44, 84, 222, PK_LEFT, pk_style.text, "git push origin main");
    pk_text(PK_REGULAR, 34, 84, 288, PK_LEFT, pk_style.muted, "Fixing the docs build");
    for (int i = 0; i < 2; i++) {
        pk_row(356 + (float)i * ROW_PITCH, demo_inbox[i].title, demo_inbox[i].subtitle, demo_inbox[i].meta, false);
    }
    pk_prompts((pk_prompt[]){{'A', "Review"}}, 1);
}

static void draw_inbox(void) {
    if (app.demo) {
        draw_demo_approval_card();
        return;
    }
    if (draw_list_state(app.inbox.status, app.inbox.count, "Hermes is offline right now", "Nothing waiting")) {
        draw_list(&app.inbox, app.selection[TAB_INBOX], true);
        pk_prompts((pk_prompt[]){{'A', "Open"}}, 1);
    }
}

static float reader_limit(void) {
    return fmaxf(0, (float)app.reader_height - (READER_BOTTOM - READER_TOP));
}

static int message_height(const hub_message *message) {
    float max_width = message->from_user ? BUBBLE_MAX_TEXT : 920;
    int lines = pk_text_lines(PK_REGULAR, READER_SIZE, max_width, message->text);
    return lines * READER_LINE + (message->from_user ? BUBBLE_PAD_Y * 2 : 0);
}

static void measure_chat(void) {
    app.reader_height = 0;
    for (int i = 0; i < app.chat.count; i++) {
        app.message_heights[i] = message_height(&app.chat.messages[i]);
        app.reader_height += app.message_heights[i] + MESSAGE_GAP;
    }
}

static void draw_message(const hub_message *message, float y, int height) {
    if (!message->from_user) {
        pk_text_wrapped(PK_REGULAR, READER_SIZE, PK_MARGIN, y, 920, READER_LINE, pk_style.text, message->text);
        return;
    }
    float text_width = fminf(BUBBLE_MAX_TEXT, pk_text_width(PK_REGULAR, READER_SIZE, message->text));
    float bubble_width = text_width + BUBBLE_PAD_X * 2;
    float x = PK_RIGHT_EDGE - bubble_width;
    pk_round_rect(x, y, bubble_width, (float)height, 22, pk_style.card);
    pk_text_wrapped(PK_REGULAR, READER_SIZE, x + BUBBLE_PAD_X, y + BUBBLE_PAD_Y, BUBBLE_MAX_TEXT, READER_LINE, pk_style.text, message->text);
}

static void draw_scrollbar(void) {
    float view = READER_BOTTOM - READER_TOP, limit = reader_limit();
    if (limit <= 0) return;
    float thumb = fmaxf(48, view * view / (float)app.reader_height);
    float y = READER_TOP + (view - thumb) * app.reader_scroll / limit;
    pk_round_rect(1000, y, 5, thumb, 2.5f, pk_style.faint);
}

static const hub_item *open_chat(void) {
    return &app.chats.items[app.selection[TAB_CHATS]];
}

static bool chat_continues_here(const hub_item *item) {
    return strncmp(item->id, "pocket-", 7) == 0;
}

static void draw_chat(void) {
    const hub_item *item = open_chat();
    pk_header(item->title, clock_text(), battery_percent());
    if (app.chat.status == HUB_LOADING) draw_empty("Loading");
    if (app.chat.status == HUB_FAILED) draw_empty("Couldn't open this chat");
    if (app.reader_at_end && app.chat.status == HUB_READY) {
        app.reader_scroll = reader_limit();
        app.reader_at_end = false;
    }
    float y = READER_TOP - app.reader_scroll;
    pk_clip(READER_TOP - 6, READER_BOTTOM);
    for (int i = 0; i < app.chat.count; i++) {
        int height = app.message_heights[i];
        if (y + (float)height > READER_TOP - 6 && y < READER_BOTTOM) draw_message(&app.chat.messages[i], y, height);
        y += (float)(height + MESSAGE_GAP);
    }
    pk_clip(0, PK_HEIGHT);
    draw_scrollbar();
    if (chat_continues_here(item)) {
        pk_prompts((pk_prompt[]){{'A', "Hold A to talk"}, {'B', "Back"}}, 2);
    } else {
        char hint[64];
        snprintf(hint, sizeof hint, "Continue this on %s", item->subtitle);
        pk_text(PK_MEDIUM, 28, PK_MARGIN, PK_PROMPT_Y + 24, PK_LEFT, pk_style.muted, hint);
        pk_prompts((pk_prompt[]){{'B', "Back"}}, 1);
    }
}

static void draw_note(void) {
    pk_header("Inbox", clock_text(), battery_percent());
    if (app.note.status == HUB_LOADING) draw_empty("Loading");
    if (app.note.status == HUB_FAILED) draw_empty("Couldn't open this");
    if (app.note.status == HUB_READY) {
        app.reader_height = 64 + pk_text_lines(PK_REGULAR, READER_SIZE, 920, app.note.body) * READER_LINE;
        float y = READER_TOP - app.reader_scroll;
        pk_clip(READER_TOP - 6, READER_BOTTOM);
        pk_text(PK_SEMIBOLD, 42, PK_MARGIN, y + 24, PK_LEFT, pk_style.text, app.note.title);
        pk_text_wrapped(PK_REGULAR, READER_SIZE, PK_MARGIN, y + 64, 920, READER_LINE, pk_style.text, app.note.body);
        pk_clip(0, PK_HEIGHT);
        draw_scrollbar();
    }
    pk_prompts((pk_prompt[]){{'B', "Back"}}, 1);
}

static void draw_agent(void) {
    static const hub_item demo_tiles[] = {
        {"Model", "GLM 5.3", "Flash", "", false},
        {"Gateway", "Online", "6 of 6 platforms", "", false},
        {"Routines", "2 active", "All healthy", "", false},
        {"Achievements", "28 of 60", "46 turns today", "", false},
    };
    const hub_item *tiles = app.demo ? demo_tiles : app.agent.items;
    bool ready = app.demo || app.agent.count >= 4;
    if (ready) {
        for (int i = 0; i < 4; i++) {
            float x = i % 2 ? 522 : 48, y = i < 2 ? 116 : 400;
            pk_tile(x, y, tiles[i].id, tiles[i].title, tiles[i].subtitle, false);
        }
    } else {
        draw_list_state(app.agent.status, 0, "Hermes is offline right now", "Loading");
    }
    pk_prompts((pk_prompt[]){{'Y', "Look"}}, 1);
}

static void draw_listening(float time) {
    pk_header("Listening", elapsed_text(), battery_percent());
    float energy = app.demo ? 0.95f : 0.12f + voice_level() * 0.88f;
    pk_rays(512, 370, 150, 150, energy, time);
    pk_image_draw(&app.orb_listening, 512 - 150, 370 - 150);
    pk_prompts((pk_prompt[]){{'A', "Release to send"}, {'B', "Cancel"}}, 2);
}

static void draw_working_animation(float cx, float cy, float time) {
    if (app.animation == ANIM_ORB) {
        float pulse = 0.5f + 0.5f * sinf(time * 3);
        pk_ring(cx, cy, 50 + pulse * 5, 2, pk_style.line);
        pk_image_draw(&app.orb_small, (int)cx - 46, (int)cy - 46);
        return;
    }
    if (app.animation == ANIM_GRID) {
        for (int row = 0; row < 4; row++) {
            for (int col = 0; col < 4; col++) {
                bool lit = ((row + col + (int)(time * 6)) % 3) != 0;
                pk_round_rect(cx - 42 + (float)col * 22, cy - 42 + (float)row * 22, 16, 16, 3, lit ? pk_style.accent : pk_style.line);
            }
        }
        return;
    }
    for (int i = 0; i < 160; i++) {
        float angle = (float)i * 2.399963f + time * 0.5f, radius = 46 * sqrtf((float)i / 160.0f);
        pk_circle(cx + cosf(angle) * radius, cy + sinf(angle) * radius * 0.92f, 1.6f, pk_style.accent);
    }
}

static void draw_step(float y, bool done, const char *label, const char *detail) {
    if (done) {
        pk_circle(68, y + 20, 16, pk_style.text);
        pk_line(60, y + 21, 66, y + 27, 3.5f, pk_style.canvas);
        pk_line(66, y + 27, 77, y + 13, 3.5f, pk_style.canvas);
        pk_text(PK_REGULAR, 40, 108, y + 20, PK_LEFT, pk_style.muted, label);
        return;
    }
    pk_ring(68, y + 20, 16, 4, pk_style.accent);
    pk_circle(68, y + 20, 7, pk_style.accent);
    pk_text(PK_SEMIBOLD, 50, 108, y + 20, PK_LEFT, pk_style.text, label);
    if (detail && detail[0]) {
        char short_detail[44];
        snprintf(short_detail, sizeof short_detail, "%s", detail);
        pk_text(PK_MONO, 30, 108, y + 62, PK_LEFT, pk_style.muted, short_detail);
    }
}

static void draw_streaming_reply(void) {
    pk_header("Hermes", clock_text(), battery_percent());
    int lines = pk_text_lines(PK_REGULAR, REPLY_SIZE, 920, app.turn.reply);
    float overflow = fmaxf(0, (float)(lines * REPLY_LINE - (REPLY_BOTTOM - REPLY_TOP)));
    pk_clip(REPLY_TOP - 8, REPLY_BOTTOM);
    pk_text_wrapped(PK_REGULAR, REPLY_SIZE, PK_MARGIN, REPLY_TOP - overflow, 920, REPLY_LINE, pk_style.text, app.turn.reply);
    pk_clip(0, PK_HEIGHT);
    pk_prompts((pk_prompt[]){{'B', "Stop"}}, 1);
}

static void draw_working(float time) {
    if (!app.demo && app.turn.reply[0]) {
        draw_streaming_reply();
        return;
    }
    pk_header("Working", elapsed_text(), battery_percent());
    if (app.demo) {
        pk_text(PK_REGULAR, 44, PK_MARGIN, 140, PK_LEFT, pk_style.text, "\xe2\x80\x9c" "Did the docs fix get pushed?" "\xe2\x80\x9d");
        pk_line(PK_MARGIN, 192, PK_RIGHT_EDGE, 192, 2, pk_style.line);
        draw_step(222, true, "Searched files", NULL);
        draw_step(296, true, "Read docs/setup.md", NULL);
        draw_step(376, false, "Running a command", "git log --oneline -3");
        draw_working_animation(860, 560, time);
        pk_prompts((pk_prompt[]){{'B', "Stop"}}, 1);
        return;
    }
    char quoted[VOICE_TEXT_MAX + 8];
    if (app.turn.heard[0]) {
        snprintf(quoted, sizeof quoted, "\xe2\x80\x9c%s\xe2\x80\x9d", app.turn.heard);
        pk_clip(0, 186);
        pk_text_wrapped(PK_REGULAR, 40, PK_MARGIN, 104, 920, 50, pk_style.text, quoted);
        pk_clip(0, PK_HEIGHT);
        pk_line(PK_MARGIN, 192, PK_RIGHT_EDGE, 192, 2, pk_style.line);
    }
    float y = 222;
    for (int i = 0; i < app.turn.step_count; i++) {
        const turn_step *step = &app.turn.steps[i];
        draw_step(y, step->done, step->label, step->detail);
        y += step->done ? 74 : 104;
    }
    bool busy = app.turn.step_count > 0 && !app.turn.steps[app.turn.step_count - 1].done;
    if (!busy) {
        const char *status = !app.turn.heard[0] ? "Transcribing" : "Thinking";
        draw_step(y, false, status, NULL);
    }
    draw_working_animation(860, 560, time);
    pk_prompts((pk_prompt[]){{'B', "Stop"}}, 1);
}

static void draw_reply(void) {
    pk_header("Hermes", clock_text(), battery_percent());
    const char *text = app.demo ? "It's committed but not pushed. Want me to push it to main?" : app.turn.reply;
    pk_clip(REPLY_TOP - 8, REPLY_BOTTOM);
    app.reply_lines = pk_text_wrapped(PK_REGULAR, REPLY_SIZE, PK_MARGIN, REPLY_TOP - app.reply_scroll, 920, REPLY_LINE, pk_style.text, text);
    pk_clip(0, PK_HEIGHT);
    bool playing = app.demo || voice_playing();
    if (playing) {
        float seconds = app.turn.reply_seconds > 0 ? app.turn.reply_seconds : 1;
        float progress = app.demo ? 0.41f : fminf(1, (float)(SDL_GetTicks() - app.reply_started) / 1000.0f / seconds);
        pk_line(PK_MARGIN, 640, PK_RIGHT_EDGE, 640, 4, pk_style.line);
        pk_line(PK_MARGIN, 640, PK_MARGIN + (PK_RIGHT_EDGE - PK_MARGIN) * progress, 640, 4, pk_style.text);
    }
    pk_prompts((pk_prompt[]){{'A', "Reply"}, {'X', app.turn.reply_seconds > 0 || app.demo ? "Replay" : "Read aloud"}, {'B', "Back"}}, 3);
}

static void draw_error(void) {
    pk_header("Hermes", clock_text(), battery_percent());
    pk_text_wrapped(PK_REGULAR, REPLY_SIZE, PK_MARGIN, REPLY_TOP + 60, 920, REPLY_LINE, pk_style.text, app.turn.error);
    pk_prompts((pk_prompt[]){{'A', "Try again"}, {'B', "Back"}}, 2);
}

static void draw_approval(void) {
    char countdown[16] = "0:48";
    const char *command = app.demo ? "git push origin main" : app.turn.approval;
    if (!app.demo) {
        int left = APPROVAL_SECONDS - (int)((SDL_GetTicks() - app.view_started) / 1000);
        snprintf(countdown, sizeof countdown, "0:%02d", left > 0 ? left : 0);
    }
    pk_header("Approval", countdown, battery_percent());
    pk_text(PK_SEMIBOLD, 72, PK_MARGIN, 156, PK_LEFT, pk_style.text, "Run this command?");
    pk_round_rect(PK_MARGIN, 232, PK_RIGHT_EDGE - PK_MARGIN, 190, 20, pk_style.card);
    pk_clip(240, 414);
    pk_text_wrapped(PK_MONO, 36, 84, 250, 860, 48, pk_style.text, command);
    pk_clip(0, PK_HEIGHT);
    if (app.demo) {
        pk_text(PK_REGULAR, 40, PK_MARGIN, 478, PK_LEFT, pk_style.muted, "Pushes 1 commit to the docs repo.");
        pk_text(PK_REGULAR, 34, PK_MARGIN, 528, PK_LEFT, pk_style.faint, "Asked in Fixing the docs build");
    }
    pk_prompts((pk_prompt[]){{'A', "Run"}, {'X', "Allow this session"}, {'B', "Reject"}}, 3);
}

static void draw_look_row(int row, const char *title, const char *value) {
    float y = LIST_TOP + (float)row * ROW_PITCH;
    pk_card(y, 128, app.look_row == row);
    pk_text(PK_MEDIUM, 42, 84, y + 42, PK_LEFT, pk_style.text, title);
    pk_text(PK_MEDIUM, 30, 84, y + 92, PK_LEFT, pk_style.muted, value);
}

static void draw_look(float time) {
    pk_header("Look & Feel", clock_text(), battery_percent());
    draw_look_row(0, "Canvas", pk_canvas_names[app.canvas]);
    for (int i = 0; i < PK_CANVAS_COUNT; i++) {
        float cx = 800 + (float)i * 60, cy = LIST_TOP + 64;
        pk_circle(cx, cy, 20, pk_canvas_colors[i]);
        pk_ring(cx, cy, 20, 2, (int)app.canvas == i ? pk_style.text : pk_style.faint);
    }
    draw_look_row(1, "Accent color", pk_accent_names[app.accent]);
    for (int i = 0; i < PK_ACCENT_COUNT; i++) {
        float cx = 600 + (float)i * 42, cy = LIST_TOP + ROW_PITCH + 64;
        if ((int)app.accent == i) pk_ring(cx, cy, 19, 3, pk_style.text);
        pk_circle(cx, cy, 13, pk_accent_colors[i]);
    }
    draw_look_row(2, "Loading indicator", animation_names[app.animation]);
    draw_working_animation(880, LIST_TOP + ROW_PITCH * 2 + 64, time);
    draw_look_row(3, "Read replies aloud", app.speak ? "On" : "Off");
    pk_text(PK_MEDIUM, 28, PK_MARGIN, PK_PROMPT_Y + 24, PK_LEFT, pk_style.muted, "Left and right to change");
    pk_prompts((pk_prompt[]){{'B', "Back"}}, 1);
}

static void draw(float time) {
    pk_clear();
    switch (app.view) {
    case VIEW_TABS:
        draw_tabs();
        if (app.tab == TAB_TALK) draw_talk();
        if (app.tab == TAB_CHATS) draw_chats();
        if (app.tab == TAB_INBOX) draw_inbox();
        if (app.tab == TAB_AGENT) draw_agent();
        break;
    case VIEW_LISTENING: draw_listening(time); break;
    case VIEW_WORKING: draw_working(time); break;
    case VIEW_REPLY: draw_reply(); break;
    case VIEW_ERROR: draw_error(); break;
    case VIEW_APPROVAL: draw_approval(); break;
    case VIEW_LOOK: draw_look(time); break;
    case VIEW_CHAT: draw_chat(); break;
    case VIEW_NOTE: draw_note(); break;
    }
}

static void start_listening(void) {
    view_id origin = app.view == VIEW_CHAT ? VIEW_CHAT : VIEW_TABS;
    if (voice_record_start()) {
        app.listen_origin = origin;
        go_to(VIEW_LISTENING);
    }
}

static void leave_turn(void) {
    if (app.listen_origin == VIEW_CHAT) {
        hub_open_chat(app.chats.items[app.selection[TAB_CHATS]].id);
        app.reader_at_end = true;
    } else {
        voice_set_session("");
    }
    go_to(app.listen_origin);
}

static void change_look(int direction) {
    if (app.look_row == 0) app.canvas = (app.canvas + PK_CANVAS_COUNT + direction) % PK_CANVAS_COUNT;
    if (app.look_row == 1) app.accent = (app.accent + PK_ACCENT_COUNT + direction) % PK_ACCENT_COUNT;
    if (app.look_row == 2) app.animation = (app.animation + ANIM_COUNT + direction) % ANIM_COUNT;
    if (app.look_row == 3) app.speak = !app.speak;
    pk_set_theme(app.canvas, app.accent);
    voice_set_speak(app.speak);
    save_look();
}

static void scroll_reply(int direction) {
    float limit = fmaxf(0, (float)(app.reply_lines * REPLY_LINE - (REPLY_BOTTOM - REPLY_TOP)));
    app.reply_scroll = fminf(limit, fmaxf(0, app.reply_scroll + (float)(direction * REPLY_LINE)));
}

static void scroll_reader(int direction) {
    app.reader_scroll = fminf(reader_limit(), fmaxf(0, app.reader_scroll + (float)(direction * READER_SCROLL)));
}

static int list_count(tab_id tab) {
    if (tab == TAB_CHATS) return app.chats.count;
    if (tab == TAB_INBOX) return app.inbox.count;
    return 0;
}

static void refresh_tab(void) {
    if (app.demo) return;
    if (app.tab == TAB_CHATS) hub_refresh_chats();
    if (app.tab == TAB_INBOX) hub_refresh_inbox();
    if (app.tab == TAB_AGENT) hub_refresh_agent();
}

static void switch_tab(int direction) {
    app.tab = (app.tab + TAB_COUNT + direction) % TAB_COUNT;
    refresh_tab();
}

static void activate_tab(void) {
    if (app.tab == TAB_TALK) start_listening();
    if (app.tab == TAB_INBOX && app.demo) go_to(VIEW_APPROVAL);
    if (app.tab == TAB_CHATS && app.chats.count > 0) {
        const hub_item *item = open_chat();
        voice_set_session(chat_continues_here(item) ? item->id : "");
        hub_open_chat(item->id);
        app.reader_scroll = 0;
        app.reader_at_end = true;
        go_to(VIEW_CHAT);
    }
    if (app.tab == TAB_INBOX && !app.demo && app.inbox.count > 0) {
        hub_open_note(app.inbox.items[app.selection[TAB_INBOX]].id);
        app.reader_scroll = 0;
        go_to(VIEW_NOTE);
    }
}

static void button_down(button_id button) {
    app.dirty = true;
    if ((button == BTN_UP || button == BTN_DOWN) && app.held_button != (int)button) {
        app.held_button = button;
        app.held_since = SDL_GetTicks();
        app.held_repeat = app.held_since + HOLD_DELAY_MS;
        app.held_tick = app.held_since;
    }
    if (button == BTN_MENU) {
        app.running = false;
        return;
    }
    switch (app.view) {
    case VIEW_TABS:
        if (button == BTN_L) switch_tab(-1);
        if (button == BTN_R) switch_tab(1);
        if (button == BTN_UP && app.selection[app.tab] > 0) app.selection[app.tab]--;
        if (button == BTN_DOWN && app.selection[app.tab] < list_count(app.tab) - 1) app.selection[app.tab]++;
        if (button == BTN_A) activate_tab();
        if (button == BTN_Y && app.tab != TAB_AGENT) {
            voice_new_chat();
            app.tab = TAB_TALK;
        }
        if (button == BTN_Y && app.tab == TAB_AGENT) go_to(VIEW_LOOK);
        break;
    case VIEW_CHAT:
        if (button == BTN_A && chat_continues_here(open_chat())) start_listening();
        if (button == BTN_B) {
            voice_set_session("");
            go_to(VIEW_TABS);
        }
        if (button == BTN_UP) scroll_reader(-1);
        if (button == BTN_DOWN) scroll_reader(1);
        break;
    case VIEW_NOTE:
        if (button == BTN_B) go_to(VIEW_TABS);
        if (button == BTN_UP) scroll_reader(-1);
        if (button == BTN_DOWN) scroll_reader(1);
        break;
    case VIEW_LISTENING:
        if (button == BTN_B) {
            voice_record_cancel();
            leave_turn();
        }
        break;
    case VIEW_REPLY:
        if (button == BTN_A) start_listening();
        if (button == BTN_X) {
            voice_speak_reply();
            app.reply_started = SDL_GetTicks();
        }
        if (button == BTN_B) {
            voice_play_stop();
            voice_turn_clear();
            leave_turn();
        }
        if (button == BTN_UP) scroll_reply(-1);
        if (button == BTN_DOWN) scroll_reply(1);
        break;
    case VIEW_WORKING:
        if (button == BTN_B) {
            voice_turn_clear();
            leave_turn();
        }
        break;
    case VIEW_ERROR:
        if (button == BTN_A) start_listening();
        if (button == BTN_B) leave_turn();
        break;
    case VIEW_APPROVAL:
        if (button == BTN_A || button == BTN_B || button == BTN_X) {
            if (app.demo) {
                go_to(VIEW_TABS);
            } else {
                voice_approve(button == BTN_A ? "once" : button == BTN_X ? "session" : "deny");
                go_to(VIEW_WORKING);
            }
        }
        break;
    case VIEW_LOOK:
        if (button == BTN_UP && app.look_row > 0) app.look_row--;
        if (button == BTN_DOWN && app.look_row < 3) app.look_row++;
        if (button == BTN_LEFT) change_look(-1);
        if (button == BTN_RIGHT) change_look(1);
        if (button == BTN_B) go_to(VIEW_TABS);
        break;
    }
}

static void button_up(button_id button) {
    if ((int)button == app.held_button) app.held_button = -1;
    if (button == BTN_A && app.view == VIEW_LISTENING) {
        voice_record_send();
        go_to(VIEW_WORKING);
    }
}

static int button_for_key(SDL_Keycode key) {
    switch (key) {
    case SDLK_z: return BTN_A;
    case SDLK_x: return BTN_B;
    case SDLK_a: return BTN_X;
    case SDLK_s: return BTN_Y;
    case SDLK_q: return BTN_L;
    case SDLK_w: return BTN_R;
    case SDLK_UP: return BTN_UP;
    case SDLK_DOWN: return BTN_DOWN;
    case SDLK_LEFT: return BTN_LEFT;
    case SDLK_RIGHT: return BTN_RIGHT;
    case SDLK_ESCAPE: return BTN_MENU;
    default: return -1;
    }
}

static int button_for_joystick(int index) {
    for (int i = 0; i < BTN_COUNT; i++) {
        if (app.joystick_buttons[i] == index) return i;
    }
    return -1;
}

static void hat_changed(Uint8 value) {
    static Uint8 previous;
    Uint8 pressed = value & (Uint8)~previous, released = previous & (Uint8)~value;
    previous = value;
    if (released & SDL_HAT_UP) button_up(BTN_UP);
    if (released & SDL_HAT_DOWN) button_up(BTN_DOWN);
    if (pressed & SDL_HAT_UP) button_down(BTN_UP);
    if (pressed & SDL_HAT_DOWN) button_down(BTN_DOWN);
    if (pressed & SDL_HAT_LEFT) button_down(BTN_LEFT);
    if (pressed & SDL_HAT_RIGHT) button_down(BTN_RIGHT);
}

static void handle_event(const SDL_Event *event, bool log_input) {
    int button = -1;
    switch (event->type) {
    case SDL_QUIT:
        app.running = false;
        break;
    case SDL_KEYDOWN:
        if (log_input) printf("key %d\n", event->key.keysym.sym);
        if (!event->key.repeat && (button = button_for_key(event->key.keysym.sym)) >= 0) button_down(button);
        break;
    case SDL_KEYUP:
        if ((button = button_for_key(event->key.keysym.sym)) >= 0) button_up(button);
        break;
    case SDL_JOYBUTTONDOWN:
        if (log_input) printf("joystick button %d\n", event->jbutton.button);
        if ((button = button_for_joystick(event->jbutton.button)) >= 0) button_down(button);
        break;
    case SDL_JOYBUTTONUP:
        if ((button = button_for_joystick(event->jbutton.button)) >= 0) button_up(button);
        break;
    case SDL_JOYHATMOTION:
        if (log_input) printf("joystick hat %d\n", event->jhat.value);
        hat_changed(event->jhat.value);
        break;
    case SDL_JOYAXISMOTION:
        if (log_input && abs(event->jaxis.value) > 16000) printf("joystick axis %d value %d\n", event->jaxis.axis, event->jaxis.value);
        break;
    default:
        break;
    }
}

static void update_turn(void) {
    if (app.view == VIEW_REPLY) app.turn = voice_turn();
    if (app.view == VIEW_APPROVAL && !app.demo) {
        app.turn = voice_turn();
        if (!app.turn.approval[0]) go_to(VIEW_WORKING);
    }
    if (app.view != VIEW_WORKING) return;
    app.turn = voice_turn();
    app.dirty = true;
    if (app.turn.approval[0]) {
        go_to(VIEW_APPROVAL);
    } else if (app.turn.phase == TURN_DONE) {
        app.reply_scroll = 0;
        app.reply_started = SDL_GetTicks();
        if (app.speak) voice_play_reply();
        go_to(VIEW_REPLY);
    } else if (app.turn.phase == TURN_FAILED) {
        go_to(VIEW_ERROR);
    }
}

static void update_hub(Uint32 now) {
    if (app.demo) return;
    hub_status chats = app.chats.status, inbox = app.inbox.status, chat = app.chat.status, note = app.note.status;
    hub_status agent = app.agent.status;
    int unread = app.inbox.unread;
    hub_chats(&app.chats);
    hub_inbox(&app.inbox);
    hub_agent(&app.agent);
    if (app.view == VIEW_CHAT) {
        hub_chat_view(&app.chat);
        if (chat != HUB_READY && app.chat.status == HUB_READY) measure_chat();
    }
    if (app.view == VIEW_NOTE) hub_note_view(&app.note);
    if (chats != app.chats.status || inbox != app.inbox.status || chat != app.chat.status || note != app.note.status ||
        agent != app.agent.status || unread != app.inbox.unread) {
        app.dirty = true;
    }
    if (app.selection[TAB_CHATS] >= app.chats.count) app.selection[TAB_CHATS] = app.chats.count > 0 ? app.chats.count - 1 : 0;
    if (app.selection[TAB_INBOX] >= app.inbox.count) app.selection[TAB_INBOX] = app.inbox.count > 0 ? app.inbox.count - 1 : 0;
    if (!app.inbox_requested || now - app.inbox_refreshed > INBOX_REFRESH_MS) {
        app.inbox_requested = true;
        app.inbox_refreshed = now;
        hub_refresh_inbox();
    }
}

static void update_held(Uint32 now) {
    if (app.held_button < 0 || now - app.held_since < HOLD_DELAY_MS) return;
    int direction = app.held_button == BTN_UP ? -1 : 1;
    if (app.view == VIEW_CHAT || app.view == VIEW_NOTE) {
        float step = (float)(now - app.held_tick) * HOLD_SCROLL_PER_MS * (float)direction;
        app.reader_scroll = fminf(reader_limit(), fmaxf(0, app.reader_scroll + step));
        app.held_tick = now;
        app.dirty = true;
    } else if (now >= app.held_repeat) {
        app.held_repeat = now + HOLD_REPEAT_MS;
        int held = app.held_button;
        button_down(held);
    }
}

static bool animating(void) {
    if (app.held_button >= 0) return true;
    if (app.view == VIEW_LISTENING || app.view == VIEW_WORKING || app.view == VIEW_LOOK) return true;
    return app.view == VIEW_REPLY && voice_playing();
}

static void load_config(char values[][CONFIG_VALUE_MAX], const char *const *names, int count, const char *path) {
    FILE *file = fopen(path, "r");
    char line[CONFIG_VALUE_MAX * 2];
    while (file && fgets(line, sizeof line, file)) {
        line[strcspn(line, "\r\n")] = 0;
        char *equals = strchr(line, '=');
        if (!equals) continue;
        *equals = 0;
        for (int i = 0; i < count; i++) {
            if (strcmp(line, names[i]) == 0) snprintf(values[i], CONFIG_VALUE_MAX, "%s", equals + 1);
        }
    }
    if (file) fclose(file);
}

static bool load_assets(const char *assets_dir) {
    char path[CONFIG_VALUE_MAX + 32];
    if (!pk_init(assets_dir)) return false;
    snprintf(path, sizeof path, "%s/orb-idle.png", assets_dir);
    bool ok = pk_image_load(&app.orb_idle, path);
    snprintf(path, sizeof path, "%s/orb-listening.png", assets_dir);
    ok = ok && pk_image_load(&app.orb_listening, path);
    snprintf(path, sizeof path, "%s/orb-small.png", assets_dir);
    return ok && pk_image_load(&app.orb_small, path);
}

static int save_shot(const char *screen, const char *path) {
    static const struct { const char *name; view_id view; tab_id tab; } shots[] = {
        {"talk", VIEW_TABS, TAB_TALK}, {"chats", VIEW_TABS, TAB_CHATS}, {"inbox", VIEW_TABS, TAB_INBOX},
        {"agent", VIEW_TABS, TAB_AGENT}, {"listening", VIEW_LISTENING, TAB_TALK}, {"working", VIEW_WORKING, TAB_TALK},
        {"reply", VIEW_REPLY, TAB_TALK}, {"approval", VIEW_APPROVAL, TAB_INBOX}, {"look", VIEW_LOOK, TAB_AGENT},
        {"chat", VIEW_CHAT, TAB_CHATS},
    };
    app.demo = true;
    memcpy(app.chats.items, demo_chats, sizeof demo_chats);
    app.chats.count = 4;
    app.chats.status = HUB_READY;
    app.inbox.unread = 1;
    static const char *const demo_messages[] = {
        "Did the docs fix get pushed?",
        "It's committed but not pushed. The last commit is the docs fix, and main is one ahead of origin. Want me to push it?",
        "Yeah go ahead, then tell me when it's live.",
        "Pushed. The deploy is running now and should finish in about two minutes. I'll keep an eye on it from here.",
    };
    app.chat.count = 4;
    app.chat.status = HUB_READY;
    for (int i = 0; i < 4; i++) {
        app.chat.messages[i].from_user = i % 2 == 0;
        snprintf(app.chat.messages[i].text, HUB_MESSAGE_TEXT, "%s", demo_messages[i]);
    }
    measure_chat();
    for (size_t i = 0; i < sizeof shots / sizeof shots[0]; i++) {
        if (strcmp(shots[i].name, screen) == 0) {
            app.view = shots[i].view;
            app.tab = shots[i].tab;
            draw(1.3f);
            return pk_save_png(path) ? 0 : 1;
        }
    }
    fprintf(stderr, "unknown screen %s\n", screen);
    return 1;
}

static int run(bool log_input) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *window = SDL_CreateWindow("pocket-hermes", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, PK_WIDTH, PK_HEIGHT, 0);
    SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, -1, 0) : NULL;
    SDL_Texture *texture = renderer ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, PK_WIDTH, PK_HEIGHT) : NULL;
    if (!texture) {
        fprintf(stderr, "SDL setup: %s\n", SDL_GetError());
        return 1;
    }
    for (int i = 0; i < SDL_NumJoysticks(); i++) SDL_JoystickOpen(i);
    if (log_input) {
        SDL_RendererInfo info;
        SDL_GetRendererInfo(renderer, &info);
        printf("video %s, renderer %s, joysticks %d\n", SDL_GetCurrentVideoDriver(), info.name, SDL_NumJoysticks());
        for (int i = 0; i < SDL_NumJoysticks(); i++) printf("joystick %d: %s\n", i, SDL_JoystickNameForIndex(i));
    }
    Uint32 last_second = 0, frame_ms = 0;
    int frames = 0;
    while (app.running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) handle_event(&event, log_input);
        update_turn();
        Uint32 now = SDL_GetTicks();
        update_hub(now);
        update_held(now);
        if (now / 1000 != last_second) {
            last_second = now / 1000;
            app.dirty = true;
        }
        if (app.dirty || animating()) {
            draw((float)now / 1000.0f);
            SDL_UpdateTexture(texture, NULL, pk_pixels, PK_WIDTH * 4);
            SDL_RenderCopy(renderer, texture, NULL, NULL);
            SDL_RenderPresent(renderer);
            app.dirty = false;
            frame_ms += SDL_GetTicks() - now;
            if (log_input && ++frames == 120) {
                printf("view %d: %.1f ms per frame\n", app.view, (double)frame_ms / frames);
                frames = 0;
                frame_ms = 0;
            }
        }
        SDL_Delay(animating() ? 16 : 33);
    }
    voice_record_cancel();
    voice_play_stop();
    SDL_Quit();
    return 0;
}

int main(int argc, char **argv) {
    static const char *const names[] = {
        "RELAY_URL", "RELAY_TOKEN", "RECORD_COMMAND", "PLAY_COMMAND", "MIC_ON_COMMAND", "MIC_OFF_COMMAND",
        "ASSETS_DIR", "DATA_DIR", "BUTTONS",
    };
    enum { RELAY_URL, RELAY_TOKEN, RECORD, PLAY, MIC_ON, MIC_OFF, ASSETS, DATA, BUTTONS, VALUE_COUNT };
    static char values[VALUE_COUNT][CONFIG_VALUE_MAX] = {
        [RECORD] = "exec arecord -q -D hw:audiocodec,0 -f S16_LE -r 48000 -c 2 -t raw",
        [PLAY] = "exec aplay -q",
        [MIC_ON] = "amixer -c 0 cset numid=5 20 >/dev/null",
        [MIC_OFF] = "amixer -c 0 cset numid=5 0 >/dev/null",
        [ASSETS] = "assets",
        [DATA] = "/mnt/UDISK/pocket-hermes",
        [BUTTONS] = "1 0 3 2 4 5 -1 -1 -1 -1 8",
    };
    const char *config_path = getenv("POCKET_HERMES_CONFIG");
    load_config(values, names, VALUE_COUNT, config_path ? config_path : "/mnt/UDISK/pocket-hermes/config");
    snprintf(app.data_dir, sizeof app.data_dir, "%s", values[DATA]);
    mkdir(app.data_dir, 0755);
    if (!load_assets(values[ASSETS])) return 1;
    load_look();

    const char *cursor = values[BUTTONS];
    for (int i = 0; i < BTN_COUNT; i++) {
        char *end;
        app.joystick_buttons[i] = (int)strtol(cursor, &end, 10);
        cursor = end;
    }
    voice_configure(&(voice_config){
        .relay_url = values[RELAY_URL], .relay_token = values[RELAY_TOKEN], .record_command = values[RECORD],
        .play_command = values[PLAY], .mic_on_command = values[MIC_ON], .mic_off_command = values[MIC_OFF],
        .work_dir = app.data_dir,
    });

    if (argc == 4 && strcmp(argv[1], "--shot") == 0) return save_shot(argv[2], argv[3]);
    bool log_input = argc == 2 && strcmp(argv[1], "--log-input") == 0;
    if (log_input) setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc == 2 && strcmp(argv[1], "--demo") == 0) {
        app.demo = true;
        memcpy(app.chats.items, demo_chats, sizeof demo_chats);
        app.chats.count = 4;
        app.chats.status = HUB_READY;
        app.inbox.unread = 1;
    }
    return run(log_input);
}
