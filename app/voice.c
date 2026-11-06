#include "voice.h"

#include "relay.h"

#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define SAMPLE_RATE 48000
#define CHANNELS 2
#define BYTES_PER_SECOND (SAMPLE_RATE * CHANNELS * 2)
#define MAX_RECORD_SECONDS 60
#define WAV_HEADER_BYTES 44
#define POLL_MICROSECONDS 300000

static voice_config config;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static turn_state turn;
static unsigned turn_generation;
static char chat_session[96];
static char turn_path[128];
static volatile bool speak_replies;

static pid_t recorder_pid, player_pid;
static int recorder_fd = -1;
static pthread_t reader_thread;
static uint8_t *recording;
static size_t recorded_bytes;
static volatile float current_level;
static volatile bool recording_active;

void voice_configure(const voice_config *new_config) {
    config = *new_config;
    relay_configure(config.relay_url, config.relay_token);
}

void voice_set_session(const char *session_id) {
    pthread_mutex_lock(&lock);
    snprintf(chat_session, sizeof chat_session, "%s", session_id ? session_id : "");
    pthread_mutex_unlock(&lock);
}

void voice_set_speak(bool speak) {
    speak_replies = speak;
}

static pid_t spawn_shell(const char *command, int *stdout_fd) {
    int pipe_ends[2] = {-1, -1};
    if (stdout_fd && pipe(pipe_ends) != 0) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        if (stdout_fd) {
            dup2(pipe_ends[1], STDOUT_FILENO);
            close(pipe_ends[0]);
            close(pipe_ends[1]);
        }
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }
    if (stdout_fd) {
        close(pipe_ends[1]);
        *stdout_fd = pipe_ends[0];
    }
    return pid;
}

static void stop_process(pid_t *pid) {
    if (*pid <= 0) return;
    kill(-*pid, SIGTERM);
    waitpid(*pid, NULL, 0);
    *pid = 0;
}

static void run_shell(const char *command) {
    if (!command || !command[0]) return;
    pid_t pid = spawn_shell(command, NULL);
    if (pid > 0) waitpid(pid, NULL, 0);
}

static void *read_microphone(void *unused) {
    (void)unused;
    const size_t capacity = (size_t)BYTES_PER_SECOND * MAX_RECORD_SECONDS;
    uint8_t chunk[4096];
    ssize_t count;
    while ((count = read(recorder_fd, chunk, sizeof chunk)) > 0) {
        size_t usable = (size_t)count;
        if (recorded_bytes + usable > capacity) usable = capacity - recorded_bytes;
        memcpy(recording + WAV_HEADER_BYTES + recorded_bytes, chunk, usable);
        recorded_bytes += usable;
        const int16_t *samples = (const int16_t *)chunk;
        size_t frames = (size_t)count / 4;
        double energy = 0;
        for (size_t i = 0; i < frames; i++) energy += (double)samples[i * 2] * samples[i * 2];
        float rms = frames ? (float)sqrt(energy / (double)frames) / 32768.0f : 0;
        float target = fminf(1.0f, sqrtf(rms) * 2.4f);
        current_level = current_level * 0.6f + target * 0.4f;
    }
    return NULL;
}

bool voice_record_start(void) {
    if (recording_active) return false;
    recording = malloc(WAV_HEADER_BYTES + (size_t)BYTES_PER_SECOND * MAX_RECORD_SECONDS);
    if (!recording) return false;
    recorded_bytes = 0;
    current_level = 0;
    voice_play_stop();
    run_shell(config.mic_on_command);
    recorder_pid = spawn_shell(config.record_command, &recorder_fd);
    if (recorder_pid <= 0) {
        free(recording);
        recording = NULL;
        return false;
    }
    recording_active = true;
    pthread_create(&reader_thread, NULL, read_microphone, NULL);
    return true;
}

static void finish_recording(void) {
    stop_process(&recorder_pid);
    pthread_join(reader_thread, NULL);
    close(recorder_fd);
    recorder_fd = -1;
    recording_active = false;
    current_level = 0;
    run_shell(config.mic_off_command);
}

void voice_record_cancel(void) {
    if (!recording_active) return;
    finish_recording();
    free(recording);
    recording = NULL;
}

float voice_level(void) {
    return current_level;
}

static void write_u32(uint8_t *at, uint32_t value) {
    for (int i = 0; i < 4; i++) at[i] = (uint8_t)(value >> (i * 8));
}

static void write_u16(uint8_t *at, uint16_t value) {
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> 8);
}

static void fill_wav_header(uint8_t *header, uint32_t data_bytes) {
    memcpy(header, "RIFF", 4);
    write_u32(header + 4, data_bytes + 36);
    memcpy(header + 8, "WAVEfmt ", 8);
    write_u32(header + 16, 16);
    write_u16(header + 20, 1);
    write_u16(header + 22, CHANNELS);
    write_u32(header + 24, SAMPLE_RATE);
    write_u32(header + 28, BYTES_PER_SECOND);
    write_u16(header + 32, CHANNELS * 2);
    write_u16(header + 34, 16);
    memcpy(header + 36, "data", 4);
    write_u32(header + 40, data_bytes);
}

static void fail_turn(unsigned generation, const char *message) {
    pthread_mutex_lock(&lock);
    if (generation == turn_generation) {
        turn.phase = TURN_FAILED;
        snprintf(turn.error, sizeof turn.error, "%s", message);
    }
    pthread_mutex_unlock(&lock);
}

static char *next_line(char **cursor) {
    char *line = *cursor;
    if (!line) return NULL;
    char *end = strchr(line, '\n');
    if (end) {
        *end = 0;
        *cursor = end + 1;
    } else {
        *cursor = NULL;
    }
    return line;
}

static float wav_seconds(size_t bytes) {
    return bytes > WAV_HEADER_BYTES ? (float)(bytes - WAV_HEADER_BYTES) / (48000.0f * 2.0f) : 0;
}

static void reply_path(char *path, size_t size) {
    snprintf(path, size, "%s/reply.wav", config.work_dir);
}

typedef struct {
    uint8_t *wav;
    size_t bytes;
    unsigned generation;
} turn_job;

static bool turn_is_current(unsigned generation) {
    pthread_mutex_lock(&lock);
    bool current = generation == turn_generation;
    pthread_mutex_unlock(&lock);
    return current;
}

static void parse_steps(char *cursor, turn_state *state) {
    char *line;
    state->step_count = 0;
    while (state->step_count < VOICE_MAX_STEPS && cursor && *cursor && (line = next_line(&cursor))) {
        char *label = strchr(line, '\t');
        if (!label) continue;
        *label++ = 0;
        char *detail = strchr(label, '\t');
        if (detail) *detail++ = 0;
        turn_step *step = &state->steps[state->step_count++];
        step->done = strcmp(line, "done") == 0;
        snprintf(step->label, sizeof step->label, "%s", label);
        snprintf(step->detail, sizeof step->detail, "%s", detail ? detail : "");
    }
}

static void stop_remote_turn(const char *status_path) {
    char path[160];
    relay_reply reply = {0};
    snprintf(path, sizeof path, "%s/stop", status_path);
    if (relay_request("POST", path, NULL, 0, &reply)) free(reply.body);
}

static float download_reply(const char *audio) {
    relay_reply clip = {0};
    char path[256];
    float seconds = 0;
    if (audio[0] && relay_request("GET", audio, NULL, 0, &clip)) {
        reply_path(path, sizeof path);
        FILE *file = clip.status == 200 ? fopen(path, "wb") : NULL;
        if (file) {
            fwrite(clip.body, 1, clip.length, file);
            fclose(file);
            seconds = wav_seconds(clip.length);
        }
        free(clip.body);
    }
    return seconds;
}

static bool poll_turn(unsigned generation, const char *status_path) {
    relay_reply status = {0};
    if (!relay_request("GET", status_path, NULL, 0, &status)) {
        fail_turn(generation, "Lost the connection. Try again.");
        return false;
    }
    char *cursor = status.body;
    char *state = next_line(&cursor), *heard = next_line(&cursor), *text = next_line(&cursor), *audio = next_line(&cursor);
    if (status.status != 200 || !state || !heard || !text || !audio) {
        free(status.body);
        fail_turn(generation, "Lost the connection. Try again.");
        return false;
    }
    bool failed = strcmp(state, "failed") == 0, done = strcmp(state, "done") == 0;
    bool asking = strcmp(state, "approval") == 0;
    if (failed) {
        fail_turn(generation, strcmp(text, "heard nothing") == 0 ? "Didn't catch that." : text);
        free(status.body);
        return false;
    }
    float seconds = done ? download_reply(audio) : 0;
    turn_state update = {0};
    parse_steps(cursor, &update);
    pthread_mutex_lock(&lock);
    if (generation == turn_generation) {
        snprintf(turn.heard, sizeof turn.heard, "%s", heard);
        snprintf(asking ? turn.approval : turn.reply, asking ? sizeof turn.approval : sizeof turn.reply, "%s", text);
        if (!asking) turn.approval[0] = 0;
        turn.step_count = update.step_count;
        memcpy(turn.steps, update.steps, sizeof turn.steps);
        turn.reply_seconds = seconds;
        if (done) turn.phase = TURN_DONE;
    }
    pthread_mutex_unlock(&lock);
    free(status.body);
    return !done;
}

static void *run_turn(void *argument) {
    turn_job job = *(turn_job *)argument;
    free(argument);
    fill_wav_header(job.wav, (uint32_t)job.bytes);
    relay_reply response = {0};
    char post_path[160];
    pthread_mutex_lock(&lock);
    snprintf(post_path, sizeof post_path, "/v1/turn?speak=%d%s%s", speak_replies ? 1 : 0, chat_session[0] ? "&session=" : "", chat_session);
    pthread_mutex_unlock(&lock);
    bool ok = relay_request("POST", post_path, job.wav, job.bytes + WAV_HEADER_BYTES, &response);
    free(job.wav);
    if (!ok) {
        fail_turn(job.generation, "Hermes is offline right now.");
        return NULL;
    }
    response.body[strcspn(response.body, "\n")] = 0;
    if (response.status != 200) {
        fail_turn(job.generation, response.status == 409 ? "Still working on the last one." : response.body);
        free(response.body);
        return NULL;
    }
    char status_path[128];
    snprintf(status_path, sizeof status_path, "/v1/turn/%s", response.body);
    free(response.body);
    pthread_mutex_lock(&lock);
    if (job.generation == turn_generation) snprintf(turn_path, sizeof turn_path, "%s", status_path);
    pthread_mutex_unlock(&lock);
    while (poll_turn(job.generation, status_path)) {
        if (!turn_is_current(job.generation)) {
            stop_remote_turn(status_path);
            return NULL;
        }
        usleep(POLL_MICROSECONDS);
    }
    return NULL;
}

void voice_record_send(void) {
    if (!recording_active) return;
    finish_recording();
    turn_job *job = malloc(sizeof *job);
    pthread_mutex_lock(&lock);
    memset(&turn, 0, sizeof turn);
    unsigned generation = ++turn_generation;
    turn.phase = TURN_WAITING;
    pthread_mutex_unlock(&lock);
    if (!job) {
        free(recording);
        recording = NULL;
        fail_turn(generation, "Couldn't start the request.");
        return;
    }
    *job = (turn_job){.wav = recording, .bytes = recorded_bytes, .generation = generation};
    recording = NULL;
    pthread_t worker;
    if (pthread_create(&worker, NULL, run_turn, job) != 0) {
        free(job->wav);
        free(job);
        fail_turn(generation, "Couldn't start the request.");
        return;
    }
    pthread_detach(worker);
}

turn_state voice_turn(void) {
    pthread_mutex_lock(&lock);
    turn_state copy = turn;
    pthread_mutex_unlock(&lock);
    return copy;
}

void voice_turn_clear(void) {
    pthread_mutex_lock(&lock);
    turn_generation++;
    memset(&turn, 0, sizeof turn);
    pthread_mutex_unlock(&lock);
}

void voice_play_reply(void) {
    voice_play_stop();
    char path[256], command[512];
    reply_path(path, sizeof path);
    snprintf(command, sizeof command, "%s '%s'", config.play_command, path);
    player_pid = spawn_shell(command, NULL);
}

static void *fetch_and_play(void *unused) {
    (void)unused;
    char path[160];
    pthread_mutex_lock(&lock);
    snprintf(path, sizeof path, "%s/speech", turn_path);
    pthread_mutex_unlock(&lock);
    relay_reply reply = {0};
    if (!relay_request("POST", path, NULL, 0, &reply)) return NULL;
    reply.body[strcspn(reply.body, "\n")] = 0;
    float seconds = reply.status == 200 ? download_reply(reply.body) : 0;
    free(reply.body);
    if (seconds <= 0) return NULL;
    pthread_mutex_lock(&lock);
    turn.reply_seconds = seconds;
    pthread_mutex_unlock(&lock);
    voice_play_reply();
    return NULL;
}

void voice_speak_reply(void) {
    pthread_mutex_lock(&lock);
    bool downloaded = turn.reply_seconds > 0;
    pthread_mutex_unlock(&lock);
    if (downloaded) {
        voice_play_reply();
        return;
    }
    pthread_t worker;
    if (pthread_create(&worker, NULL, fetch_and_play, NULL) == 0) pthread_detach(worker);
}

typedef struct {
    char path[160];
    const char *choice;
} approval_job;

static void *send_approval(void *argument) {
    approval_job job = *(approval_job *)argument;
    free(argument);
    relay_reply reply = {0};
    if (relay_request("POST", job.path, job.choice, strlen(job.choice), &reply)) free(reply.body);
    return NULL;
}

void voice_approve(const char *choice) {
    approval_job *job = malloc(sizeof *job);
    if (!job) return;
    job->choice = choice;
    pthread_mutex_lock(&lock);
    snprintf(job->path, sizeof job->path, "%s/approval", turn_path);
    turn.approval[0] = 0;
    pthread_mutex_unlock(&lock);
    pthread_t worker;
    if (pthread_create(&worker, NULL, send_approval, job) == 0) {
        pthread_detach(worker);
    } else {
        free(job);
    }
}

void voice_play_stop(void) {
    stop_process(&player_pid);
}

bool voice_playing(void) {
    if (player_pid <= 0) return false;
    if (waitpid(player_pid, NULL, WNOHANG) == 0) return true;
    player_pid = 0;
    return false;
}

static void *reset_chat(void *unused) {
    (void)unused;
    voice_set_session("");
    relay_reply reply = {0};
    if (relay_request("POST", "/v1/reset", NULL, 0, &reply)) free(reply.body);
    return NULL;
}

void voice_new_chat(void) {
    pthread_t worker;
    pthread_create(&worker, NULL, reset_chat, NULL);
    pthread_detach(worker);
}
