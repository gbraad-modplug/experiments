#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <SDL.h>
#include <libopenmpt/libopenmpt.h>
#include <libopenmpt/libopenmpt_ext.h>

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

    int do_pattern_loop; // flag for deferred pattern loop
} AudioData;

static volatile int running = 1;

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

    // Defer pattern loop jump until inside audio callback
    if (data->do_pattern_loop) {
        openmpt_module_set_position_order_row(data->mod, data->loop_order, 0);
        if (data->interactive_ok)
            reapply_mutes(data);
        data->do_pattern_loop = 0;
        // Optionally: printf("Looped back to Order %d (Pattern %d)\n", data->loop_order, data->loop_pattern);
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

    AudioData ad;
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
    ad.do_pattern_loop = 0;

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
    spec.samples = 512;
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
    printf("  r retrigger current pattern\n");
    printf("  1–9 toggle channels, m=mute all, u=unmute all\n");
    printf("  +/- adjust pitch, p=toggle pattern/song mode\n");
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
                int cur_pat   = openmpt_module_get_current_pattern(ad.mod);
                openmpt_module_set_position_order_row(ad.mod, cur_order, 0);
                reapply_mutes(&ad);
                printf("Retriggered Order %d (Pattern %d)\n", cur_order, cur_pat);
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
            } else if (k == 'p' || k == 'P') {
                ad.pattern_mode = !ad.pattern_mode;
                if (ad.pattern_mode) {
                    ad.loop_order   = openmpt_module_get_current_order(ad.mod);
                    ad.loop_pattern = openmpt_module_get_current_pattern(ad.mod);
                    printf("Pattern mode ON (looping pattern %d at order %d)\n",
                           ad.loop_pattern, ad.loop_order);
                    prev_row = -1; // reset previous row tracker
                } else {
                    printf("Song mode ON\n");
                }
            }
        }

        // Pattern loop enforcement at row boundary, with wrap detection
        if (ad.pattern_mode) {
            int cur_order = openmpt_module_get_current_order(ad.mod);
            int cur_row   = openmpt_module_get_current_row(ad.mod);
            int rows      = openmpt_module_get_pattern_num_rows(ad.mod, ad.loop_pattern);

            if (cur_order == ad.loop_order) {
                if (prev_row == rows - 1 && cur_row == 0) {
                    ad.do_pattern_loop = 1;
                    // printf("Looped back to Order %d (Pattern %d)\n", ad.loop_order, ad.loop_pattern);
                }
                prev_row = cur_row;
            } else {
                ad.do_pattern_loop = 1;
                // printf("Corrected back to Order %d (Pattern %d)\n", ad.loop_order, ad.loop_pattern);
                prev_row = -1;
            }
        }

        SDL_Delay(10);
    }

    SDL_CloseAudio();
    free(ad.mute_states);
    openmpt_module_ext_destroy(modext);
    SDL_Quit();
    return 0;
}
