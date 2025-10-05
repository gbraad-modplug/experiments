#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <SDL.h>
#include <libopenmpt/libopenmpt.h>
#include <libopenmpt/libopenmpt_ext.h>

typedef enum {
    PLAYBACK_NONE,
    PLAYBACK_QUEUE_ORDER,
    PLAYBACK_LOOP_TILL_ROW
} PlaybackCommandType;

typedef struct {
    PlaybackCommandType type;
    int order;
    int row;
} PlaybackCommand;

#define MAX_COMMANDS 8

typedef struct {
    openmpt_module_ext *modext;
    openmpt_module *mod;
    openmpt_module_ext_interface_interactive interactive;
    int interactive_ok;
    double samplerate;
    double pitch_factor;
    int num_channels;
    int *mute_states;

    int pattern_mode;   // 0 = song, 1 = pattern loop
    int loop_pattern;   // pattern index to loop
    int loop_order;     // order index to loop
    int paused;         // 0 = playing, 1 = paused

    // Command queue
    PlaybackCommand command_queue[MAX_COMMANDS];
    int command_queue_head;
    int command_queue_tail;

    // For queued jumps (song mode)
    int queued_order;
    int queued_row;
    int has_queued_jump;

    // For loop-till-row
    int loop_till_row;
    int is_looping_till;

    // For pattern mode pending jump
    int pending_pattern_mode_order; // -1 = none
} AudioData;

static volatile int running = 1;

// Function prototypes
static void reapply_mutes(AudioData *ad);
static void enqueue_command(AudioData *ad, PlaybackCommandType type, int order, int row);
static void process_commands(AudioData *ad);

// -------- terminal (stdin) helpers --------
static struct termios orig_termios;
static void tty_restore(void) {
    if (orig_termios.c_cflag) {
        tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
    }
}
static int tty_make_raw_nonblocking(void) {
    if (!isatty(STDIN_FILENO)) return -1;
    if (tcgetattr(STDIN_FILENO, &orig_termios) < 0) return -1;
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags != -1) fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    atexit(tty_restore);
    return 0;
}
static int read_key_nonblocking(void) {
    unsigned char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n == 1) return (int)c;
    return -1;
}

// -------- signal handling --------
static void handle_sigint(int sig) { (void)sig; running = 0; }

// -------- SDL audio callback --------
static void audio_callback(void *userdata, Uint8 *stream, int len) {
    AudioData *data = (AudioData *)userdata;
    int16_t *buffer = (int16_t *)stream;
    int frames = len / (2 * sizeof(int16_t));

    // Process playback commands before rendering audio
    process_commands(data);

    // Handle queued jumps at row boundary (for song mode)
    if (data->has_queued_jump) {
        openmpt_module_set_position_order_row(data->mod, data->queued_order, data->queued_row);
        if (data->interactive_ok)
            reapply_mutes(data);
        data->has_queued_jump = 0;
    }

    if (data->paused) {
        SDL_memset(stream, 0, len);
        return;
    }

    int count = openmpt_module_read_interleaved_stereo(
        data->mod,
        data->samplerate * data->pitch_factor,
        frames,
        buffer
    );
    if (count == 0) {
        SDL_memset(stream, 0, len);
    }
}

// -------- utilities --------
static void reapply_mutes(AudioData *ad) {
    if (!ad->interactive_ok) return;
    for (int ch = 0; ch < ad->num_channels; ++ch) {
        ad->interactive.set_channel_volume(ad->modext, ch,
            ad->mute_states[ch] ? 0.0 : 1.0);
    }
}

static void *load_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror("fopen"); return NULL; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = malloc((size_t)size);
    fread(buf, 1, (size_t)size, f);
    fclose(f);
    *out_size = (size_t)size;
    return buf;
}

static void enqueue_command(AudioData *ad, PlaybackCommandType type, int order, int row) {
    int next_tail = (ad->command_queue_tail + 1) % MAX_COMMANDS;
    if (next_tail != ad->command_queue_head) { // not full
        ad->command_queue[ad->command_queue_tail].type = type;
        ad->command_queue[ad->command_queue_tail].order = order;
        ad->command_queue[ad->command_queue_tail].row = row;
        ad->command_queue_tail = next_tail;
    }
}

static void process_commands(AudioData *ad) {
    while (ad->command_queue_head != ad->command_queue_tail) {
        PlaybackCommand *cmd = &ad->command_queue[ad->command_queue_head];
        switch (cmd->type) {
            case PLAYBACK_QUEUE_ORDER:
                if (ad->pattern_mode) {
                    // Always overwrite: last N/n or P/p wins, used on next wrap
                    ad->pending_pattern_mode_order = cmd->order;
                } else {
                    ad->queued_order = cmd->order;
                    ad->queued_row = cmd->row;
                    ad->has_queued_jump = 1;
                }
                break;
            case PLAYBACK_LOOP_TILL_ROW:
                ad->loop_order = cmd->order;
                ad->loop_pattern = openmpt_module_get_order_pattern(ad->mod, cmd->order);
                ad->loop_till_row = cmd->row;
                ad->is_looping_till = 1;
                openmpt_module_set_position_order_row(ad->mod, cmd->order, 0);
                if (ad->interactive_ok) reapply_mutes(ad);
                break;
            default: break;
        }
        ad->command_queue_head = (ad->command_queue_head + 1) % MAX_COMMANDS;
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s file.mod\n", argv[0]);
        return 1;
    }
    printf("Now playing: %s\n", argv[1]);

    // Load module
    size_t bytes_size = 0;
    void *bytes = load_file(argv[1], &bytes_size);
    if (!bytes) return 1;
    int error = 0;
    openmpt_module_ext *modext = openmpt_module_ext_create_from_memory(
        bytes, bytes_size, NULL, NULL, NULL, &error, NULL, NULL, NULL);
    free(bytes);
    if (!modext) { fprintf(stderr, "Error loading module (%d)\n", error); return 1; }
    openmpt_module *mod = openmpt_module_ext_get_module(modext);
    if (!mod) { fprintf(stderr, "Failed to get base module\n"); openmpt_module_ext_destroy(modext); return 1; }

    // Print order list overview
    int num_orders = openmpt_module_get_num_orders(mod);
    printf("Song order list (%d entries):\n", num_orders);
    for (int ord = 0; ord < num_orders; ++ord) {
        int pat = openmpt_module_get_order_pattern(mod, ord);
        printf("  Order %2d -> Pattern %2d\n", ord, pat);
    }
    printf("--------------------------------------\n");

    AudioData ad = {0};
    ad.modext = modext;
    ad.mod = mod;
    ad.samplerate = 48000.0;
    ad.pitch_factor = 1.0;
    ad.num_channels = openmpt_module_get_num_channels(mod);
    ad.mute_states = (int *)calloc((size_t)ad.num_channels, sizeof(int));
    ad.interactive_ok = 0;
    ad.pattern_mode = 0;
    ad.loop_pattern = 0;
    ad.loop_order = 0;
    ad.paused = 1;
    ad.command_queue_head = ad.command_queue_tail = 0;
    ad.has_queued_jump = 0;
    ad.loop_till_row = 0;
    ad.is_looping_till = 0;
    ad.pending_pattern_mode_order = -1;

    if (!ad.mute_states) {
        fprintf(stderr, "calloc failed\n");
        openmpt_module_ext_destroy(modext);
        return 1;
    }

    if (openmpt_module_ext_get_interface(
            modext, LIBOPENMPT_EXT_C_INTERFACE_INTERACTIVE,
            &ad.interactive, sizeof(ad.interactive)) != 0) {
        ad.interactive_ok = 1;
        fprintf(stderr, "Interactive extension loaded.\n");
    }

    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        free(ad.mute_states);
        openmpt_module_ext_destroy(modext);
        return 1;
    }
    SDL_AudioSpec spec;
    SDL_zero(spec);
    spec.freq = (int)ad.samplerate;
    spec.format = AUDIO_S16SYS;
    spec.channels = 2;
    spec.samples = 256; // tight command timing
    spec.callback = audio_callback;
    spec.userdata = &ad;
    if (SDL_OpenAudio(&spec, NULL) < 0) {
        fprintf(stderr, "SDL_OpenAudio failed: %s\n", SDL_GetError());
        free(ad.mute_states);
        openmpt_module_ext_destroy(modext);
        SDL_Quit();
        return 1;
    }

    signal(SIGINT, handle_sigint);
    SDL_PauseAudio(0);
    tty_make_raw_nonblocking();

    printf("Controls:\n");
    printf("  SPACE start/stop playback\n");
    printf("  r immediately retrigger current pattern (row 0)\n");
    printf("  N/n queue next order (pattern) for after current pattern in pattern mode, or next jump in song mode\n");
    printf("  P/p queue previous order (pattern) for after current pattern in pattern mode, or previous jump in song mode\n");
    printf("  j loop current pattern from row 0 till the row you pressed j\n");
    printf("  S or s toggle song/pattern mode\n");
    printf("  1–9 toggle channels, m=mute all, u=unmute all\n");
    printf("  +/- adjust pitch\n");
    printf("  q/ESC quit\n");

    printf("\nPlayback paused (press SPACE to start)\n");

    static int prev_row = -1;
    while (running) {
        int k = read_key_nonblocking();
        if (k != -1) {
            if (k == 27 || k == 'q' || k == 'Q') {
                running = 0;
            } else if (k == ' ') {
                ad.paused = !ad.paused;
                printf("Playback %s\n", ad.paused ? "paused" : "resumed");
            } else if (k == 'r' || k == 'R') {
                int cur_order = openmpt_module_get_current_order(ad.mod);
                openmpt_module_set_position_order_row(ad.mod, cur_order, 0);
                if (ad.interactive_ok) reapply_mutes(&ad);
                printf("Immediate retrigger: Order %d, Row 0\n", cur_order);
            } else if (k == 'N' || k == 'n') {
                int cur_order = openmpt_module_get_current_order(ad.mod);
                int next_order = cur_order + 1;
                if (next_order < num_orders) {
                    enqueue_command(&ad, PLAYBACK_QUEUE_ORDER, next_order, 0);
                    printf("Next order queued: Order %d\n", next_order);
                }
            } else if (k == 'P' || k == 'p') {
                int cur_order = openmpt_module_get_current_order(ad.mod);
                int prev_order = cur_order > 0 ? cur_order - 1 : 0;
                enqueue_command(&ad, PLAYBACK_QUEUE_ORDER, prev_order, 0);
                printf("Previous order queued: Order %d\n", prev_order);
            } else if (k == 'j' || k == 'J') {
                int cur_order = openmpt_module_get_current_order(ad.mod);
                int cur_row = openmpt_module_get_current_row(ad.mod);
                enqueue_command(&ad, PLAYBACK_LOOP_TILL_ROW, cur_order, cur_row);
                printf("Loop till row queued: Order %d, Row %d\n", cur_order, cur_row);
            } else if (k == 'S' || k == 's') {
                ad.pattern_mode = !ad.pattern_mode;
                if (ad.pattern_mode) {
                    ad.loop_order   = openmpt_module_get_current_order(ad.mod);
                    ad.loop_pattern = openmpt_module_get_current_pattern(ad.mod);
                    printf("Pattern mode ON (looping pattern %d at order %d)\n",
                           ad.loop_pattern, ad.loop_order);
                    ad.pending_pattern_mode_order = -1;
                } else {
                    printf("Song mode ON\n");
                }
            } else if (ad.interactive_ok && k >= '1' && k <= '9') {
                int ch = k - '1';
                if (ch < ad.num_channels) {
                    ad.mute_states[ch] = !ad.mute_states[ch];
                    ad.interactive.set_channel_volume(ad.modext, ch,
                        ad.mute_states[ch] ? 0.0 : 1.0);
                    printf("Channel %d %s\n", ch + 1,
                        ad.mute_states[ch] ? "muted" : "unmuted");
                }
            } else if (ad.interactive_ok && (k == 'm' || k == 'M')) {
                for (int ch = 0; ch < ad.num_channels; ++ch) {
                    ad.mute_states[ch] = 1;
                    ad.interactive.set_channel_volume(ad.modext, ch, 0.0);
                }
                printf("All channels muted\n");
            } else if (ad.interactive_ok && (k == 'u' || k == 'U')) {
                for (int ch = 0; ch < ad.num_channels; ++ch) {
                    ad.mute_states[ch] = 0;
                    ad.interactive.set_channel_volume(ad.modext, ch, 1.0);
                }
                printf("All channels unmuted\n");
            } else if (k == '+' || k == '=') {
                ad.pitch_factor *= 1.05;
                printf("Pitch factor: %.2f\n", ad.pitch_factor);
            } else if (k == '-') {
                ad.pitch_factor /= 1.05;
                printf("Pitch factor: %.2f\n", ad.pitch_factor);
            }
        }

        // --- Pattern mode: always loop pattern at current order, and switch if pending queued ---
        if (ad.pattern_mode && !ad.is_looping_till) {
            int cur_order = openmpt_module_get_current_order(ad.mod);
            int cur_row = openmpt_module_get_current_row(ad.mod);
            int rows = openmpt_module_get_pattern_num_rows(ad.mod, ad.loop_pattern);

            // Detect wrap: always check pending order
            if (prev_row == rows - 1 && cur_row == 0) {
                if (ad.pending_pattern_mode_order != -1 && ad.pending_pattern_mode_order != ad.loop_order) {
                    ad.loop_order = ad.pending_pattern_mode_order;
                    ad.loop_pattern = openmpt_module_get_order_pattern(ad.mod, ad.loop_order);
                    openmpt_module_set_position_order_row(ad.mod, ad.loop_order, 0);
                    if (ad.interactive_ok) reapply_mutes(&ad);
                    printf("Pattern mode: jumping to and looping pattern %d at order %d\n",
                           ad.loop_pattern, ad.loop_order);
                    ad.pending_pattern_mode_order = -1; // clear after jump
                } else {
                    openmpt_module_set_position_order_row(ad.mod, ad.loop_order, 0);
                    if (ad.interactive_ok) reapply_mutes(&ad);
                    printf("Pattern mode: looping pattern %d at order %d\n", ad.loop_pattern, ad.loop_order);
                }
            }
            prev_row = cur_row;

            // Snap back if we left the loop order
            if (cur_order != ad.loop_order) {
                openmpt_module_set_position_order_row(ad.mod, ad.loop_order, 0);
                if (ad.interactive_ok) reapply_mutes(&ad);
                prev_row = -1;
            }
        }
        // --- Loop-till-row logic ---
        else if (ad.is_looping_till) {
            int cur_order = openmpt_module_get_current_order(ad.mod);
            int cur_row = openmpt_module_get_current_row(ad.mod);
            int rows = openmpt_module_get_pattern_num_rows(ad.mod, ad.loop_pattern);

            if (cur_order == ad.loop_order) {
                if (cur_row == ad.loop_till_row) {
                    ad.is_looping_till = 0; // stop looping, resume normal playback
                    printf("Loop-till-row finished at Order %d, Row %d\n", cur_order, cur_row);
                } else if (prev_row == rows - 1 && cur_row == 0) {
                    openmpt_module_set_position_order_row(ad.mod, ad.loop_order, 0);
                    if (ad.interactive_ok) reapply_mutes(&ad);
                    printf("Looping pattern at Order %d, back to Row 0\n", ad.loop_order);
                }
                prev_row = cur_row;
            } else {
                prev_row = -1;
            }
        } else if (ad.has_queued_jump) {
            openmpt_module_set_position_order_row(ad.mod, ad.queued_order, ad.queued_row);
            if (ad.interactive_ok) reapply_mutes(&ad);
            ad.has_queued_jump = 0;
            printf("Jumped to Order %d, Row %d\n", ad.queued_order, ad.queued_row);
        }

        SDL_Delay(10);
    }

    SDL_CloseAudio();
    free(ad.mute_states);
    openmpt_module_ext_destroy(modext);
    SDL_Quit();
    return 0;
}
