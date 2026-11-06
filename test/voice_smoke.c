#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "voice.h"

static void read_setting(const char *path, const char *name, char *value, size_t size) {
    FILE *file = fopen(path, "r");
    char line[512];
    size_t length = strlen(name);
    while (file && fgets(line, sizeof line, file)) {
        if (strncmp(line, name, length) == 0 && line[length] == '=') {
            line[strcspn(line, "\r\n")] = 0;
            snprintf(value, size, "%s", line + length + 1);
        }
    }
    if (file) fclose(file);
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: voice_smoke <config> <wav> <seconds>\n");
        return 2;
    }
    static char url[256], token[256], record[640];
    read_setting(argv[1], "RELAY_URL", url, sizeof url);
    read_setting(argv[1], "RELAY_TOKEN", token, sizeof token);
    snprintf(record, sizeof record, "exec ffmpeg -loglevel error -re -i '%s' -f s16le -ar 48000 -ac 2 -", argv[2]);
    voice_configure(&(voice_config){.relay_url = url, .relay_token = token, .record_command = record,
                                    .play_command = "true", .mic_on_command = "", .mic_off_command = "",
                                    .work_dir = "build/data"});
    if (!voice_record_start()) return 1;
    float peak = 0;
    for (int i = 0; i < atoi(argv[3]) * 10; i++) {
        usleep(100000);
        if (voice_level() > peak) peak = voice_level();
    }
    voice_record_send();
    printf("peak level %.2f\n", peak);
    turn_state turn;
    int shown = 0;
    do {
        usleep(200000);
        turn = voice_turn();
        for (; shown < turn.step_count; shown++) printf("step: %s (%s)\n", turn.steps[shown].label, turn.steps[shown].detail);
    } while (turn.phase == TURN_WAITING);
    if (turn.phase == TURN_FAILED) {
        printf("failed: %s\n", turn.error);
        return 1;
    }
    printf("heard: %s\nreply: %s\naudio: %.1f s\n", turn.heard, turn.reply, turn.reply_seconds);
    return 0;
}
