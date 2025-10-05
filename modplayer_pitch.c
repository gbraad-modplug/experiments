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
    double pitch_factor;   // NEW: pitch multiplier
    int num_channels;
    int *mute_states;
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
    if (!isatty(STDIN_FILENO)) {
        fprintf(stderr, "stdin is not a TTY; key input may not work.\n");
        return -1;
    }
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
static void handle_sigint(int sig) {
    (void)sig;
    running = 0;
}

// -------- SDL audio callback --------
static void audio_callback(void *userdata, Uint8 *stream, int len) {
    AudioData *data = (AudioData *)userdata;
    int16_t *buffer = (int16_t *)stream;
    int frames = len / (2 * sizeof(int16_t)); // stereo, 16-bit

    int count = openmpt_module_read_interleaved_stereo(
        data->mod,
        data->samplerate * data->pitch_factor, // apply pitch factor
        frames,
        buffer
    );

    if (count == 0) {
        SDL_memset(stream, 0, len);
    }
}

// -------- utilities --------
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

    // Load module file
    size_t bytes_size = 0;
    void *bytes = load_file(argv[1], &bytes_size);
    if (!bytes) return 1;

    int error = 0;
    openmpt_module_ext *modext = openmpt_module_ext_create_from_memory(
        bytes, bytes_size,
        NULL, NULL, NULL, &error,
        NULL, NULL, NULL);
    free(bytes);
    if (!modext) {
        fprintf(stderr, "Error loading module (code %d)\n", error);
        return 1;
    }
    openmpt_module *mod = openmpt_module_ext_get_module(modext);

    AudioData ad;
    ad.modext = modext;
    ad.mod = mod;
    ad.samplerate = 48000.0;
    ad.pitch_factor = 1.0; // normal speed
    ad.num_channels = openmpt_module_get_num_channels(mod);
    ad.mute_states = calloc((size_t)ad.num_channels, sizeof(int));
    ad.interactive_ok = 0;

    if (openmpt_module_ext_get_interface(
            modext,
            LIBOPENMPT_EXT_C_INTERFACE_INTERACTIVE,
            &ad.interactive,
            sizeof(ad.interactive)) != 0) {
        ad.interactive_ok = 1;
        fprintf(stderr, "Interactive extension loaded successfully.\n");
    } else {
        fprintf(stderr, "Interactive extension not available.\n");
    }

    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_AudioSpec spec;
    SDL_zero(spec);
    spec.freq = (int)ad.samplerate;
    spec.format = AUDIO_S16SYS;
    spec.channels = 2;
    spec.samples = 1024;
    spec.callback = audio_callback;
    spec.userdata = &ad;

    if (SDL_OpenAudio(&spec, NULL) < 0) {
        fprintf(stderr, "SDL_OpenAudio failed: %s\n", SDL_GetError());
        return 1;
    }

    signal(SIGINT, handle_sigint);
    SDL_PauseAudio(0);
    tty_make_raw_nonblocking();

    printf("Playing…\n");
    printf("Keys: 1–9 toggle channels, m=mute all, u=unmute all, +/- adjust pitch, q/ESC quit.\n");

    while (running) {
        int k = read_key_nonblocking();
        if (k != -1) {
            if (k == 27 || k == 'q' || k == 'Q') {
                printf("DEBUG: quit\n");
                running = 0;
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
                printf("DEBUG: mute all\n");
                for (int ch = 0; ch < ad.num_channels; ++ch) {
                    ad.mute_states[ch] = 1;
                    ad.interactive.set_channel_volume(ad.modext, ch, 0.0);
                }
            } else if (ad.interactive_ok && (k == 'u' || k == 'U')) {
                printf("DEBUG: unmute all\n");
                for (int ch = 0; ch < ad.num_channels; ++ch) {
                    ad.mute_states[ch] = 0;
                    ad.interactive.set_channel_volume(ad.modext, ch, 1.0);
                }
            } else if (k == '+' || k == '=') {
                ad.pitch_factor *= 1.05;
                printf("Pitch factor: %.2f\n", ad.pitch_factor);
            } else if (k == '-') {
                ad.pitch_factor /= 1.05;
                printf("Pitch factor: %.2f\n", ad.pitch_factor);
            } else {
                printf("DEBUG: key=%d ignored\n", k);
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
