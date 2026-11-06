#ifndef VOICE_H
#define VOICE_H

#include <stdbool.h>

#define VOICE_TEXT_MAX 2048
#define VOICE_STEP_TEXT 96
#define VOICE_MAX_STEPS 4

typedef enum { TURN_IDLE, TURN_WAITING, TURN_DONE, TURN_FAILED } turn_phase;

typedef struct {
    bool done;
    char label[VOICE_STEP_TEXT];
    char detail[VOICE_STEP_TEXT];
} turn_step;

typedef struct {
    turn_phase phase;
    int step_count;
    turn_step steps[VOICE_MAX_STEPS];
    char heard[VOICE_TEXT_MAX];
    char reply[VOICE_TEXT_MAX];
    char approval[VOICE_STEP_TEXT * 2];
    char error[128];
    float reply_seconds;
} turn_state;

typedef struct {
    const char *relay_url;
    const char *relay_token;
    const char *record_command;
    const char *play_command;
    const char *mic_on_command;
    const char *mic_off_command;
    const char *work_dir;
} voice_config;

void voice_configure(const voice_config *config);
bool voice_record_start(void);
void voice_record_cancel(void);
void voice_record_send(void);
float voice_level(void);
turn_state voice_turn(void);
void voice_turn_clear(void);
void voice_play_reply(void);
void voice_play_stop(void);
bool voice_playing(void);
void voice_new_chat(void);
void voice_set_session(const char *session_id);
void voice_set_speak(bool speak);
void voice_speak_reply(void);
void voice_approve(const char *choice);

#endif
