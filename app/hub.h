#ifndef HUB_H
#define HUB_H

#include <stdbool.h>

#define HUB_MAX_ITEMS 24
#define HUB_MAX_MESSAGES 30
#define HUB_MESSAGE_TEXT 1024
#define HUB_NOTE_TEXT 3200

typedef enum { HUB_EMPTY, HUB_LOADING, HUB_READY, HUB_FAILED } hub_status;

typedef struct {
    char id[96];
    char title[96];
    char subtitle[40];
    char meta[24];
    bool unread;
} hub_item;

typedef struct {
    hub_status status;
    int count;
    int unread;
    hub_item items[HUB_MAX_ITEMS];
} hub_list;

typedef struct {
    bool from_user;
    char text[HUB_MESSAGE_TEXT];
} hub_message;

typedef struct {
    hub_status status;
    int count;
    hub_message messages[HUB_MAX_MESSAGES];
} hub_chat;

typedef struct {
    hub_status status;
    char title[96];
    char body[HUB_NOTE_TEXT];
} hub_note;

void hub_refresh_chats(void);
void hub_refresh_inbox(void);
void hub_refresh_agent(void);
void hub_open_chat(const char *session_id);
void hub_open_note(const char *item_id);
void hub_chats(hub_list *out);
void hub_inbox(hub_list *out);
void hub_agent(hub_list *out);
void hub_chat_view(hub_chat *out);
void hub_note_view(hub_note *out);

#endif
