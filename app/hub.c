#include "hub.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "relay.h"

typedef void (*hub_job)(const char *argument);

typedef struct {
    hub_job run;
    char argument[96];
} hub_task;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static hub_list chats, inbox, agent;
static hub_chat chat;
static hub_note note;

static char *take_line(char **cursor) {
    char *line = *cursor;
    if (!line || !*line) return NULL;
    char *end = strchr(line, '\n');
    if (end) {
        *end = 0;
        *cursor = end + 1;
    } else {
        *cursor = NULL;
    }
    return line;
}

static char *take_field(char **cursor) {
    char *field = *cursor;
    if (!field) return "";
    char *end = strchr(field, '\t');
    if (end) {
        *end = 0;
        *cursor = end + 1;
    } else {
        *cursor = NULL;
    }
    return field;
}

static void *run_task(void *argument) {
    hub_task task = *(hub_task *)argument;
    free(argument);
    task.run(task.argument);
    return NULL;
}

static void start(hub_job job, const char *argument) {
    hub_task *task = malloc(sizeof *task);
    if (!task) return;
    task->run = job;
    snprintf(task->argument, sizeof task->argument, "%s", argument ? argument : "");
    pthread_t worker;
    if (pthread_create(&worker, NULL, run_task, task) == 0) {
        pthread_detach(worker);
    } else {
        free(task);
    }
}

static void parse_item(char *line, hub_item *item) {
    snprintf(item->id, sizeof item->id, "%s", take_field(&line));
    snprintf(item->title, sizeof item->title, "%s", take_field(&line));
    snprintf(item->subtitle, sizeof item->subtitle, "%s", take_field(&line));
    snprintf(item->meta, sizeof item->meta, "%s", take_field(&line));
    item->unread = strcmp(take_field(&line), "1") == 0;
}

static void load_list(const char *path, hub_list *target, bool has_summary) {
    relay_reply reply = {0};
    static hub_list parsed;
    if (!relay_request("GET", path, NULL, 0, &reply) || reply.status != 200) {
        free(reply.body);
        pthread_mutex_lock(&lock);
        target->status = HUB_FAILED;
        pthread_mutex_unlock(&lock);
        return;
    }
    pthread_mutex_lock(&lock);
    memset(&parsed, 0, sizeof parsed);
    char *cursor = reply.body, *line;
    if (has_summary && (line = take_line(&cursor))) sscanf(line, "unread %d", &parsed.unread);
    while (parsed.count < HUB_MAX_ITEMS && (line = take_line(&cursor))) parse_item(line, &parsed.items[parsed.count++]);
    parsed.status = HUB_READY;
    *target = parsed;
    pthread_mutex_unlock(&lock);
    free(reply.body);
}

static void load_chats(const char *unused) {
    (void)unused;
    load_list("/v1/chats", &chats, false);
}

static void load_inbox(const char *unused) {
    (void)unused;
    load_list("/v1/inbox", &inbox, true);
}

static void load_agent(const char *unused) {
    (void)unused;
    load_list("/v1/agent", &agent, false);
}

static void load_chat(const char *session_id) {
    char path[128];
    snprintf(path, sizeof path, "/v1/chats/%s", session_id);
    relay_reply reply = {0};
    bool ok = relay_request("GET", path, NULL, 0, &reply) && reply.status == 200;
    pthread_mutex_lock(&lock);
    chat.status = ok ? HUB_READY : HUB_FAILED;
    chat.count = 0;
    char *cursor = reply.body, *line;
    while (ok && chat.count < HUB_MAX_MESSAGES && (line = take_line(&cursor))) {
        hub_message *message = &chat.messages[chat.count++];
        message->from_user = strcmp(take_field(&line), "user") == 0;
        snprintf(message->text, sizeof message->text, "%s", take_field(&line));
    }
    pthread_mutex_unlock(&lock);
    free(reply.body);
}

static void load_note(const char *item_id) {
    char path[64];
    snprintf(path, sizeof path, "/v1/inbox/%s", item_id);
    relay_reply reply = {0};
    bool ok = relay_request("GET", path, NULL, 0, &reply) && reply.status == 200;
    pthread_mutex_lock(&lock);
    note.status = ok ? HUB_READY : HUB_FAILED;
    if (ok) {
        char *cursor = reply.body, *title = take_line(&cursor), *body = take_line(&cursor);
        snprintf(note.title, sizeof note.title, "%s", title ? title : "");
        snprintf(note.body, sizeof note.body, "%s", body ? body : "");
    }
    pthread_mutex_unlock(&lock);
    free(reply.body);
    if (ok) load_inbox("");
}

static void mark_loading(hub_status *status) {
    pthread_mutex_lock(&lock);
    if (*status != HUB_READY) *status = HUB_LOADING;
    pthread_mutex_unlock(&lock);
}

void hub_refresh_chats(void) {
    mark_loading(&chats.status);
    start(load_chats, NULL);
}

void hub_refresh_inbox(void) {
    mark_loading(&inbox.status);
    start(load_inbox, NULL);
}

void hub_refresh_agent(void) {
    mark_loading(&agent.status);
    start(load_agent, NULL);
}

void hub_open_chat(const char *session_id) {
    pthread_mutex_lock(&lock);
    chat.status = HUB_LOADING;
    chat.count = 0;
    pthread_mutex_unlock(&lock);
    start(load_chat, session_id);
}

void hub_open_note(const char *item_id) {
    pthread_mutex_lock(&lock);
    note.status = HUB_LOADING;
    pthread_mutex_unlock(&lock);
    start(load_note, item_id);
}

void hub_chats(hub_list *out) {
    pthread_mutex_lock(&lock);
    *out = chats;
    pthread_mutex_unlock(&lock);
}

void hub_inbox(hub_list *out) {
    pthread_mutex_lock(&lock);
    *out = inbox;
    pthread_mutex_unlock(&lock);
}

void hub_agent(hub_list *out) {
    pthread_mutex_lock(&lock);
    *out = agent;
    pthread_mutex_unlock(&lock);
}

void hub_chat_view(hub_chat *out) {
    pthread_mutex_lock(&lock);
    *out = chat;
    pthread_mutex_unlock(&lock);
}

void hub_note_view(hub_note *out) {
    pthread_mutex_lock(&lock);
    *out = note;
    pthread_mutex_unlock(&lock);
}
