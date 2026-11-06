#include "relay.h"

#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define RELAY_TIMEOUT_SECONDS 150

static const char *relay_url = "";
static const char *relay_token = "";

void relay_configure(const char *url, const char *token) {
    relay_url = url;
    relay_token = token;
}

static bool parse_url(char *host, size_t host_size, char *port, size_t port_size) {
    const char *start = strstr(relay_url, "://");
    start = start ? start + 3 : relay_url;
    const char *colon = strchr(start, ':');
    const char *slash = strchr(start, '/');
    size_t host_length = colon ? (size_t)(colon - start) : slash ? (size_t)(slash - start) : strlen(start);
    if (host_length == 0 || host_length >= host_size) return false;
    memcpy(host, start, host_length);
    host[host_length] = 0;
    snprintf(port, port_size, "%s", colon ? colon + 1 : "80");
    port[strcspn(port, "/")] = 0;
    return true;
}

static int connect_relay(void) {
    char host[128], port[8];
    if (!parse_url(host, sizeof host, port, sizeof port)) return -1;
    struct addrinfo hints = {.ai_socktype = SOCK_STREAM}, *found = NULL;
    if (getaddrinfo(host, port, &hints, &found) != 0) return -1;
    int fd = socket(found->ai_family, found->ai_socktype, found->ai_protocol);
    struct timeval timeout = {.tv_sec = RELAY_TIMEOUT_SECONDS};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    if (fd >= 0 && connect(fd, found->ai_addr, found->ai_addrlen) != 0) {
        close(fd);
        fd = -1;
    }
    freeaddrinfo(found);
    return fd;
}

static bool send_all(int fd, const void *data, size_t length) {
    const uint8_t *cursor = data;
    while (length > 0) {
        ssize_t sent = send(fd, cursor, length, MSG_NOSIGNAL);
        if (sent <= 0) return false;
        cursor += sent;
        length -= (size_t)sent;
    }
    return true;
}

bool relay_request(const char *method, const char *path, const void *body, size_t body_length, relay_reply *reply) {
    int fd = connect_relay();
    if (fd < 0) return false;
    char head[640];
    int head_length = snprintf(head, sizeof head,
                               "%s %s HTTP/1.0\r\nHost: relay\r\nAuthorization: Bearer %s\r\n"
                               "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                               method, path, relay_token, body_length);
    bool sent = send_all(fd, head, (size_t)head_length) && (body_length == 0 || send_all(fd, body, body_length));
    size_t capacity = 1 << 16, used = 0;
    char *buffer = malloc(capacity + 1);
    ssize_t count = 0;
    while (sent && buffer && (count = recv(fd, buffer + used, capacity - used, 0)) > 0) {
        used += (size_t)count;
        if (used == capacity) {
            capacity *= 2;
            char *grown = realloc(buffer, capacity + 1);
            if (!grown) break;
            buffer = grown;
        }
    }
    close(fd);
    if (!sent || !buffer || used == 0) {
        free(buffer);
        return false;
    }
    buffer[used] = 0;
    char *body_start = strstr(buffer, "\r\n\r\n");
    if (!body_start || sscanf(buffer, "HTTP/%*s %d", &reply->status) != 1) {
        free(buffer);
        return false;
    }
    body_start += 4;
    reply->length = used - (size_t)(body_start - buffer);
    memmove(buffer, body_start, reply->length + 1);
    reply->body = buffer;
    return true;
}

