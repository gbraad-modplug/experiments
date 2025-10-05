// modplayer_ext_stdin.c
// Headless SDL2 + libopenmpt player that reads keys from stdin (no window needed).
// Keys: 1–9 toggle channels, m=mute all, u=unmute all, q/ESC=quit.
// Prints debug messages on every key press.

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
    openmpt_module_ext *modext;  // interactive API handle
    openmpt_module *mod;         // decoding handle
    openmpt_module_ext_interface_interactive interactive;
    int interactive_ok;
    double samplerate;
    int num_channels;
    int *mute_states;            // 0=unmuted, 1=muted
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
    if (tcgetattr(STDIN_FILENO, &orig_termios) < 0) {
        perror("tcgetattr");
        return -1;
    }
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;   // nonblocking
    raw.c_cc[VTIME] = 0;  // no timeout
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) < 0) {
        perror("tcsetattr");
        return -1;
    }
    // Also set nonblocking on fd
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags != -1) {
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    }
    atexit(tty_restore);
    return 0;
}

static int read_key_nonblocking(void) {
    unsigned char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n == 1) return (int)c;
    return -1; // no key
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
    int frames = len / (2 * sizeof(int16_t)); // stereo, 16-bit samples

    int count = openmpt_module_read_interleaved_stereo(
        data->mod, data->samplerate, frames, buffer);

    if (count == 0) {
        SDL_memset(stream, 0, len);
    }
}

// -------- utilities --------
static void *load_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror("fopen"); return NULL; }
    if (fseek(f, 0, SEEK_END) < 0) { perror("fseek"); fclose(f); return NULL; }
    long size = ftell(f);
    if (size <= 0) { fprintf(stderr, "Empty/unreadable file\n"); fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) < 0) { perror("fseek"); fclose(f); return NULL; }
    void *buf = malloc((size_t)size);
    if (!buf) { fprintf(stderr, "malloc failed\n"); fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (rd != (size_t)size) { fprintf(stderr, "Failed to read file\n"); free(buf); return NULL; }
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

    // Create libopenmpt module (interactive ext)
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
    if (!mod) {
        fprintf(stderr, "Failed to get base module\n");
        openmpt_module_ext_destroy(modext);
        return 1;
    }

    // Prepare audio data
    AudioData ad;
    ad.modext = modext;
    ad.mod = mod;
    ad.samplerate = 48000.0;
    ad.num_channels = openmpt_module_get_num_channels(mod);
    ad.mute_states = (int *)calloc((size_t)ad.num_channels, sizeof(int));
    ad.interactive_ok = 0;
    if (!ad.mute_states) {
        fprintf(stderr, "calloc failed\n");
        openmpt_module_ext_destroy(modext);
        return 1;
    }

    // Query interactive interface
    if (openmpt_module_ext_get_interface(
            modext,
            LIBOPENMPT_EXT_C_INTERFACE_INTERACTIVE,
            &ad.interactive,
            sizeof(ad.interactive)) != 0) {
        ad.interactive_ok = 1;
        fprintf(stderr, "Interactive extension loaded successfully.\n");
    } else {
        fprintf(stderr, "Interactive extension not available in this libopenmpt build.\n");
    }

    // Init SDL audio only (no window required for stdin keys)
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
    spec.samples = 1024;
    spec.callback = audio_callback;
    spec.userdata = &ad;

    if (SDL_OpenAudio(&spec, NULL) < 0) {
        fprintf(stderr, "SDL_OpenAudio failed: %s\n", SDL_GetError());
        free(ad.mute_states);
        openmpt_module_ext_destroy(modext);
        SDL_Quit();
        return 1;
    }

    // Set up signals and start audio
    signal(SIGINT, handle_sigint);
    SDL_PauseAudio(0);

    // Make stdin raw + nonblocking so we can read keys in a terminal
    tty_make_raw_nonblocking();

    printf("Playing…\n");
    if (ad.interactive_ok) {
        printf("Keys: 1–9 toggle channel mutes, m=mute all, u=unmute all, q/ESC=quit.\n");
    } else {
        printf("Interactive extension not available; mute controls disabled. Press q/ESC to quit.\n");
    }

    // Main loop: poll stdin and apply actions
    while (running) {
        int k = read_key_nonblocking();
        if (k != -1) {
            // Normalize ESC and printable
            if (k == 27 /* ESC */ || k == 'q' || k == 'Q') {
                printf("DEBUG: quit\n");
                running = 0;
            } else if (ad.interactive_ok && k >= '1' && k <= '9') {
                int ch = k - '1'; // 0-based
                if (ch < ad.num_channels) {
                    ad.mute_states[ch] = !ad.mute_states[ch];
                    ad.interactive.set_channel_volume(ad.modext, ch,
                        ad.mute_states[ch] ? 0.0 : 1.0);
                    const char *name = openmpt_module_get_channel_name(ad.mod, ch);
                    printf("DEBUG: Channel %d (%s) %s\n",
                           ch + 1, (name && *name) ? name : "unnamed",
                           ad.mute_states[ch] ? "muted" : "unmuted");
                } else {
                    printf("DEBUG: Channel %d out of range (num_channels=%d)\n", ch + 1, ad.num_channels);
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
            } else {
                printf("DEBUG: key=%d ('%c') ignored\n", k, (k >= 32 && k < 127) ? k : '?');
            }
        }

        SDL_Delay(10); // small sleep to avoid busy loop
    }

    // Cleanup
    SDL_CloseAudio();
    tty_restore();
    free(ad.mute_states);
    openmpt_module_ext_destroy(modext);
    SDL_Quit();
    return 0;
}
