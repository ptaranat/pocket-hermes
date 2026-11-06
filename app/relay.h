#ifndef RELAY_H
#define RELAY_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    int status;
    char *body;
    size_t length;
} relay_reply;

void relay_configure(const char *url, const char *token);
bool relay_request(const char *method, const char *path, const void *body, size_t body_length, relay_reply *reply);

#endif
