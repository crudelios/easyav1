#include "easyav1.h"

#include "SDL.h"

#include <stdio.h>
#include <time.h>

#if SDL_VERSION_ATLEAST(2, 0, 18)
#define HAS_RENDER_GEOMETRY 1
#endif

#define SKIP_TIME_MS 3000

#define FONT_WIDTH 7
#define FONT_HEIGHT 7
#define FONT_COLOR 0xff, 0xff, 0xff, 0xff
#define FONT_PADDING 2
#define FONT_IMAGE_COLS 4
#define FONT_IMAGE_ROWS 3

#define TIME_BAR_SIDE_PADDING 20
#define TIME_BAR_BOTTOM_PADDING 20
#define TIME_BAR_HEIGHT 60
#define TIME_BAR_ANIMATION_MS 200
#define TIME_BAR_OPEN_WAIT_TIME_MS 3000

#define MAX_PLAY_BUTTON_SIZE 200
#define PLAY_PAUSE_ANIMATION_MS 400

typedef enum {
    TIME_BAR_CLOSED,
    TIME_BAR_OPENING,
    TIME_BAR_OPEN,
    TIME_BAR_CLOSING
} time_bar_state;

// Font data - based on font8x8_basic.h - public domain
// Please refer to https://github.com/dhepper/font8x8 for more information
static const char FONT[FONT_IMAGE_COLS * FONT_IMAGE_ROWS][FONT_HEIGHT] = {
    { 0x60, 0x30, 0x18, 0x0C, 0x06, 0x03, 0x01 },   // /
    { 0x3E, 0x63, 0x73, 0x7B, 0x6F, 0x67, 0x3E },   // 0
    { 0x0C, 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x3F },   // 1
    { 0x1E, 0x33, 0x30, 0x1C, 0x06, 0x33, 0x3F },   // 2
    { 0x1E, 0x33, 0x30, 0x1C, 0x30, 0x33, 0x1E },   // 3
    { 0x38, 0x3C, 0x36, 0x33, 0x7F, 0x30, 0x78 },   // 4
    { 0x3F, 0x03, 0x1F, 0x30, 0x30, 0x33, 0x1E },   // 5
    { 0x1C, 0x06, 0x03, 0x1F, 0x33, 0x33, 0x1E },   // 6
    { 0x3F, 0x33, 0x30, 0x18, 0x0C, 0x0C, 0x0C },   // 7
    { 0x1E, 0x33, 0x33, 0x1E, 0x33, 0x33, 0x1E },   // 8
    { 0x1E, 0x33, 0x33, 0x3E, 0x30, 0x18, 0x0E },   // 9
    { 0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C },   // :
};

#ifndef HAS_RENDER_GEOMETRY
static SDL_Point play_icon_points[MAX_PLAY_BUTTON_SIZE + MAX_PLAY_BUTTON_SIZE / 2 + 1];
#endif

static struct {
    int x;
    int y;
} font_positions[FONT_IMAGE_COLS * FONT_IMAGE_ROWS];

typedef enum {
    SEEK_NONE = 0,
    SEEK_BACKWARD = 1,
    SEEK_FORWARD = 2,
    SEEK_TO = 3
} seek_mode;

static struct {
    struct {
        SDL_Window *window;
        SDL_Renderer *renderer;
        struct {
            SDL_Texture *video;
            SDL_Texture *font;
        } textures;
        SDL_AudioDeviceID audio_device;
        struct {
            SDL_Thread *decoder;
            struct {
                SDL_mutex *seek;
                SDL_mutex *picture;
            } mutex;
        } thread;
    } SDL;
    struct {
        int x;
        int y;
        easyav1_timestamp last_move_inside;
        int pressed;
        int double_click;
    } mouse;
    struct {
        time_bar_state state;
        easyav1_timestamp state_start_time;
        unsigned int y_offset;
    } time_bar;
    struct {
        int paused;
        easyav1_timestamp last_change;
    } playback;
    struct {
        seek_mode mode;
        easyav1_timestamp timestamp;
    } seek;
    int quit;
    easyav1_timestamp hovered_timestamp;
    easyav1_t *easyav1;
    struct {
        int displaying_help;
        int loop;
        int fullscreen;
        int disable_audio;
        int disable_video;
        int use_fast_seek;
        int audio_track;
        int video_track;
        int audio_offset;
        int log_level;
        const char *filename;
    } options;
} data;

typedef enum {
    OPTION_TYPE_INT,
    OPTION_TYPE_BOOL
} option_type;

static const struct {
    const char *name;
    const char *abbr;
    option_type type;
    int *value_to_change;
    const char *description;
} option_list[] = {
    { "help", "h", OPTION_TYPE_BOOL, &data.options.displaying_help, "Display this help message and exit." },
    { "loop", "l", OPTION_TYPE_BOOL, &data.options.loop, "If set, video will loop back to the beginning when it finishes." },
    { "fullscreen", "f", OPTION_TYPE_BOOL, &data.options.fullscreen, "Start in fullscreen mode." },
    { "disable_audio", "da", OPTION_TYPE_BOOL, &data.options.disable_audio, "If set, video will not play." },
    { "disable_video", "dv", OPTION_TYPE_BOOL, &data.options.disable_video, "If set, audio will not play." },
    { "use_fast_seek", "fs", OPTION_TYPE_BOOL, &data.options.use_fast_seek, "Whether to use a faster, but less accurate, seeking." },
    { "audio_track", "at", OPTION_TYPE_INT, &data.options.audio_track, "The audio track to use. If the track doesn't exist, no audio will play." },
    { "video_track", "vt", OPTION_TYPE_INT, &data.options.video_track, "The video track to use. If the video doesn't exist, no video will play." },
    { "audio_offset", "ao", OPTION_TYPE_INT, &data.options.audio_offset, "Offset in millisseconds between audio and video." },
    { "log-level", "L", OPTION_TYPE_INT, &data.options.log_level, "The log level: 0 - default, 1 - errors, 2 - warnings, 3 - info" },
};

#define OPTION_COUNT (sizeof(option_list) / sizeof(option_list[0]))

static void audio_callback(const easyav1_audio_frame *frame, void *userdata)
{
    SDL_QueueAudio(data.SDL.audio_device, frame->pcm.interlaced, frame->bytes);
}

const char *parse_file_name(const char *argv_name)
{
    const char *file_name = SDL_strrchr(argv_name, '/');

    if (!file_name) {
        file_name = SDL_strrchr(argv_name, '\\');
    }

    if (!file_name) {
        file_name = argv_name;
    } else {
        file_name++;
    }

    return file_name;
}

int parse_options(int argc, char **argv)
{
    int count = 1;

    const char *file_name = parse_file_name(argv[0]);

    while (count < argc) {
        if (argv[count][0] == '-') {
            int found = 0;
            for (size_t i = 0; i < OPTION_COUNT; i++) {
                if ((option_list[i].name && argv[count][1] == '-' && strcmp(&argv[count][2], option_list[i].name) == 0) ||
                    (option_list[i].abbr && strcmp(&argv[count][1], option_list[i].abbr) == 0)) {
                    found = 1;
                    switch (option_list[i].type) {
                        case OPTION_TYPE_INT:
                            if (count == argc - 1) {
                                printf("Option %s requires an argument.\n", argv[count]);
                                return 0;
                            }
                            count++;
                            *option_list[i].value_to_change = atoi(argv[count]);
                            break;
                        case OPTION_TYPE_BOOL:
                            *option_list[i].value_to_change = 1;
                            break;
                    }
                    break;
                }
            }

            if (!found) {
                printf("Unknown argument: \"%s\".\nUse \"%s --help\" for more help.\n", argv[count], file_name);
                return 0;
            }

        } else {
            if (count == argc - 1) {
                data.options.filename = argv[count];
            } else {
                printf("Unknown argument: \"%s\".\nUse \"%s --help\" for more help.\n", argv[count], file_name);
                return 0;
            }
        }

        count++;
    }

    if (!data.options.displaying_help && !data.options.filename) {
        printf("Usage: \"%s [OPTIONS] <filename>\"\nUse \"%s --help\" for more help.\n", file_name, file_name);
        return 0;
    }

    return 1;
}

static void display_help(const char *argv_name)
{
    size_t largest_name = 20;
    size_t largest_abbr = 1;

    for (size_t option = 0; option < OPTION_COUNT; option++)
    {
        if (option_list[option].name && strlen(option_list[option].name) > largest_name) {
            largest_name = strlen(option_list[option].name);
        }

        if (option_list[option].abbr && strlen(option_list[option].abbr) > largest_abbr) {
            largest_abbr = strlen(option_list[option].abbr);
        }
    }

    printf("\neasyav1_player - A small AV1 video player.\n\n");

    const char *file_name = parse_file_name(argv_name);

    printf("Usage: \"%s [OPTIONS] <filename>\"\n\n", file_name);

    printf("Options:\n\n");

    for (size_t option = 0; option < OPTION_COUNT; option++)
    {
        const char *name_prefix = option_list[option].name ? "--" : "  ";
        const char *name = option_list[option].name ? option_list[option].name : "";
        const char *abbr_prefix = option_list[option].abbr ? "-" : " ";
        const char *abbr = option_list[option].abbr ? option_list[option].abbr : "";
        const char *type = option_list[option].type == OPTION_TYPE_INT ? "<number>" : "        ";

        printf("  %s%-*s %s%-*s  %s  %s\n", name_prefix, largest_name, name,
            abbr_prefix, largest_abbr, abbr, type, option_list[option].description);
    }

    printf("\n");
}

static int init_easyav1(const char *filename)
{
    easyav1_settings settings = easyav1_default_settings();
    settings.callbacks.audio = audio_callback;
    settings.audio_offset_time = -22050 / 2048 + data.options.audio_offset;
    settings.video_track = data.options.video_track;
    settings.audio_track = data.options.audio_track;
    settings.enable_audio = !data.options.disable_audio;
    settings.enable_video = !data.options.disable_video;
    settings.use_fast_seeking = data.options.use_fast_seek;
    if (data.options.log_level > 0) {
        if (data.options.log_level > 4) {
            data.options.log_level = 4;
        }
        settings.log_level = data.options.log_level - 1;
    }
    data.easyav1 = easyav1_init_from_filename(filename, &settings);
    if (!data.easyav1) {
        return 0;
    }

    return 1;
}

static void exit_decoder_thread(void)
{
    SDL_WaitThread(data.SDL.thread.decoder, NULL);
    SDL_DestroyMutex(data.SDL.thread.mutex.seek);
    SDL_DestroyMutex(data.SDL.thread.mutex.picture);

    data.SDL.thread.decoder = NULL;
    data.SDL.thread.mutex.picture = NULL;
    data.SDL.thread.mutex.seek = NULL;
}

static void quit_sdl(void)
{
    if (data.SDL.audio_device) {
        SDL_CloseAudioDevice(data.SDL.audio_device);
    }

    if (data.SDL.textures.font) {
        SDL_DestroyTexture(data.SDL.textures.font);
    }

    if (data.SDL.textures.video) {
        SDL_DestroyTexture(data.SDL.textures.video);
    }

    if (data.SDL.renderer) {
        SDL_DestroyRenderer(data.SDL.renderer);
    }

    if (data.SDL.window) {
        SDL_DestroyWindow(data.SDL.window);
    }

    SDL_Quit();
}

static int init_sdl(void)
{
    Uint32 flags = SDL_INIT_VIDEO;

    if (easyav1_has_audio_track(data.easyav1)) {
        flags |= SDL_INIT_AUDIO;
    }

    if (SDL_Init(flags) < 0) {
        printf("Failed to initialize SDL. Reason: %s\n", SDL_GetError());
        return 0;
    }

    SDL_DisplayMode display_mode;
    SDL_GetDesktopDisplayMode(0, &display_mode);

    unsigned int video_width = easyav1_get_video_width(data.easyav1);
    unsigned int video_height = easyav1_get_video_height(data.easyav1);

    if (video_width > display_mode.w - 10) {
        video_width = display_mode.w - 10;
    }
    if (video_height > display_mode.h - 100) {
        video_height = display_mode.h - 100;
    }

    flags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;

    if (data.options.fullscreen) {
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }

    data.SDL.window = SDL_CreateWindow("easyav1_player", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        video_width, video_height, flags);

    if (!data.SDL.window) {
        printf("Failed to create window. Reason: %s\n", SDL_GetError());
        quit_sdl();
        return 0;
    }

    data.SDL.renderer = SDL_CreateRenderer(data.SDL.window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    SDL_SetRenderDrawColor(data.SDL.renderer, 0, 0, 0, 255);
    SDL_RenderClear(data.SDL.renderer);
    SDL_RenderPresent(data.SDL.renderer);

    if (!data.SDL.renderer) {
        printf("Failed to create renderer. Reason: %s\n", SDL_GetError());
        quit_sdl();
        return 0;
    }

    unsigned int width = easyav1_get_video_width(data.easyav1);
    unsigned int height = easyav1_get_video_height(data.easyav1);

    if (easyav1_has_video_track(data.easyav1) && width > 0 && height > 0) {
        data.SDL.textures.video = SDL_CreateTexture(data.SDL.renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, width, height);
        if (!data.SDL.textures.video) {
            printf("Failed to create video texture. Reason: %s\n", SDL_GetError());
            quit_sdl();
            return 0;
        }

        // Keep the video texture black while it has no actual contents
        SDL_SetTextureColorMod(data.SDL.textures.video, 0, 0, 0);
        SDL_SetTextureScaleMode(data.SDL.textures.video, SDL_ScaleModeBest);
    }

    if (easyav1_has_audio_track(data.easyav1)) {
        SDL_AudioSpec audio_spec = {
            .freq = easyav1_get_audio_sample_rate(data.easyav1),
            .format = AUDIO_F32,
            .channels = easyav1_get_audio_channels(data.easyav1),
            .samples = 2048
        };

        data.SDL.audio_device = SDL_OpenAudioDevice(NULL, 0, &audio_spec, NULL, 0);

        if (data.SDL.audio_device == 0) {
            printf("Failed to open audio device. Reason: %s\n", SDL_GetError());
            quit_sdl();
            return 0;
        }

        SDL_PauseAudioDevice(data.SDL.audio_device, 0);
    }

    return 1;
}

static int init_fonts(void)
{
    SDL_Surface *font_surface = SDL_CreateRGBSurface(0,
        FONT_WIDTH * FONT_IMAGE_COLS,
        FONT_HEIGHT * FONT_IMAGE_ROWS,
        32, 0xff000000, 0x00ff0000, 0x0000ff00, 0x000000ff);

    if (!font_surface) {
        printf("Failed to create font surface. Reason: %s\n", SDL_GetError());
        return 0;
    }

    if (SDL_FillRect(font_surface, NULL, SDL_MapRGBA(font_surface->format, 0, 0, 0, 0))) {
        printf("Failed to fill font surface. Reason: %s\n", SDL_GetError());
        SDL_FreeSurface(font_surface);
        return 0;
    }

    for (int i = 0; i < FONT_IMAGE_COLS * FONT_IMAGE_ROWS; i++) {
        font_positions[i].x = i % FONT_IMAGE_COLS * FONT_WIDTH;
        font_positions[i].y = i / FONT_IMAGE_COLS * FONT_HEIGHT;

        SDL_Rect rect = { font_positions[i].x, font_positions[i].y, 1, 1 };

        for (int y = 0; y < FONT_HEIGHT; y++) {
            for (int x = 0; x < FONT_WIDTH; x++) {
                if (FONT[i][y] & (1 << x)) {
                    SDL_FillRect(font_surface, &rect, SDL_MapRGBA(font_surface->format, 0xff, 0xff, 0xff, 0xff));
                }
                rect.x += 1;
            }
            rect.x = font_positions[i].x;
            rect.y ++;
        }
    }

    data.SDL.textures.font = SDL_CreateTextureFromSurface(data.SDL.renderer, font_surface);

    if (!data.SDL.textures.font) {
        printf("Failed to create font texture. Reason: %s\n", SDL_GetError());
        SDL_FreeSurface(font_surface);
        return 0;
    }

    SDL_SetTextureBlendMode(data.SDL.textures.font, SDL_BLENDMODE_BLEND);

    SDL_FreeSurface(font_surface);

    return 1;
}

#ifndef HAS_RENDER_GEOMETRY
/*
 * Inneficient way to draw a triangle that looks like a play button:
 * Starting from the center of the triangle, draw the top line, then the bottom line, then the left line.
 * The left line is slanted 1px to the left to the top.
 * Repeat the loop until you fill the entire triangle.
 * The triangle is centered at x, y.
 *
 * To draw the triangle, you need to call SDL_RenderDrawLines with the points in the play_icon_points array.
 * You also need to call a final SDL_RenderDrawLine to draw the leftmost line (as the one in the loop is slanted).
 *
 * This is inneficient because, for a triangle of size N, you need to draw N + N/2 + 2 lines.
 *
 * It also doesn't look that good with partial transparency, as the lines overlap.
 */
static void prepare_play_icon(void)
{
    unsigned int width;
    unsigned int height;
    SDL_GetWindowSize(data.SDL.window, &width, &height);

    int x = (width - MAX_PLAY_BUTTON_SIZE) / 2;
    int y = (height - MAX_PLAY_BUTTON_SIZE) / 2;

#define SET_POINT(px, py) \
    play_icon_points[point].x = px; \
    play_icon_points[point].y = py; \
    point++;

    unsigned int point = 0;

    SET_POINT(x + MAX_PLAY_BUTTON_SIZE / 2, y + MAX_PLAY_BUTTON_SIZE / 2);

    for (int current = MAX_PLAY_BUTTON_SIZE / 2 - 1; current >= 0; current--) {
        SET_POINT(x + MAX_PLAY_BUTTON_SIZE - current, y + MAX_PLAY_BUTTON_SIZE / 2);
        SET_POINT(x + current + 1, y + MAX_PLAY_BUTTON_SIZE - current);
        SET_POINT(x + current, y + current);
    }
#undef SET_POINT
}
#endif

static void init_ui(void)
{
    data.time_bar.state = TIME_BAR_OPEN;
    data.time_bar.state_start_time = SDL_GetTicks64();

#ifndef HAS_RENDER_GEOMETRY
    prepare_play_icon();
#endif
}

static int easyav1_decode_thread(void *userdata)
{
    easyav1_timestamp last_timestamp = SDL_GetTicks64();
    easyav1_timestamp current_timestamp = last_timestamp;

    SDL_LockMutex(data.SDL.thread.mutex.picture);

    while (easyav1_decode_for(data.easyav1, current_timestamp - last_timestamp) != EASYAV1_STATUS_ERROR) {

        SDL_UnlockMutex(data.SDL.thread.mutex.picture);

        if (data.quit) {
            break;
        }

        last_timestamp = current_timestamp;
        current_timestamp = SDL_GetTicks64();

        // Pause the video
        if (data.mouse.pressed || data.playback.paused) {
            last_timestamp = current_timestamp;
        }

        // Handle seeking
        if (data.seek.mode != SEEK_NONE) {

            SDL_LockMutex(data.SDL.thread.mutex.seek);

            SDL_ClearQueuedAudio(data.SDL.audio_device);

            easyav1_status seek_status = EASYAV1_STATUS_OK;

            switch (data.seek.mode) {
                case SEEK_BACKWARD:
                    seek_status = easyav1_seek_backward(data.easyav1, SKIP_TIME_MS);
                    break;
                case SEEK_FORWARD:
                    seek_status = easyav1_seek_forward(data.easyav1, SKIP_TIME_MS);
                    break;
                case SEEK_TO:
                    seek_status = easyav1_seek_to_timestamp(data.easyav1, data.seek.timestamp);
                    break;
                default:
                    break;
            }

            data.seek.mode = SEEK_NONE;
            easyav1_timestamp seek_timestamp = data.seek.timestamp;
            data.seek.timestamp = 0;

            if (seek_status != EASYAV1_STATUS_OK) {
                printf("Failed to seek to timestamp %llu\n", seek_timestamp);
                data.quit = 1;

                SDL_UnlockMutex(data.SDL.thread.mutex.seek);

                break;
            }

            if (easyav1_has_video_track(data.easyav1)) {
                while (easyav1_has_video_frame(data.easyav1) == EASYAV1_FALSE &&
                    easyav1_is_finished(data.easyav1) == EASYAV1_FALSE) {
                    easyav1_decode_next(data.easyav1);
                }
            }

            SDL_UnlockMutex(data.SDL.thread.mutex.seek);
        }

        // Prevent busy waiting
        if (current_timestamp == last_timestamp) {
            SDL_Delay(1);
        }

        SDL_LockMutex(data.SDL.thread.mutex.picture);

    }

    SDL_UnlockMutex(data.SDL.thread.mutex.picture);

    return 0;
}

static int init_decoder_thread(void)
{
    data.SDL.thread.mutex.picture = SDL_CreateMutex();
    data.SDL.thread.mutex.seek = SDL_CreateMutex();
    data.SDL.thread.decoder = SDL_CreateThread(easyav1_decode_thread, "easyav1_decode_thread", NULL);

    if (!data.SDL.thread.mutex.picture || !data.SDL.thread.mutex.seek || !data.SDL.thread.decoder) {
        printf("Failed to create decoder thread. Reason: %s\n", SDL_GetError());
        data.quit = 1;
        exit_decoder_thread();
        return 0;
    }

    return 1;
}

static void get_timestamp_string(easyav1_timestamp timestamp, char *buffer, size_t size)
{
    if (timestamp > 3600000) {
        snprintf(buffer, size, "%llu:%02llu:%02llu", timestamp / 3600000,
            timestamp / 60000, (timestamp / 1000) % 60);
    } else {
        snprintf(buffer, size, "%llu:%02llu", timestamp / 60000, (timestamp / 1000) % 60);
    }
}

static unsigned int get_timestamp_width(easyav1_timestamp timestamp)
{
    char buffer[36];
    get_timestamp_string(timestamp, buffer, sizeof(buffer));
    return strlen(buffer) * (FONT_WIDTH + FONT_PADDING) - FONT_PADDING;
}

static void draw_timestamp(unsigned int x, unsigned int y, easyav1_timestamp timestamp)
{
    char buffer[36];
    get_timestamp_string(timestamp, buffer, sizeof(buffer));

    for (size_t i = 0; i < sizeof(buffer) - 1; i++) {
        if (buffer[i] == '\0') {
            break;
        }
        char c = buffer[i] - '/';

        if (c < 0 || c >= FONT_IMAGE_COLS * FONT_IMAGE_ROWS) {
            continue;
        }

        SDL_Rect src = {
            .x = font_positions[c].x,
            .y = font_positions[c].y,
            .w = FONT_WIDTH,
            .h = FONT_HEIGHT
        };

        SDL_Rect dst = {
            .x = x,
            .y = y,
            .w = FONT_WIDTH,
            .h = FONT_HEIGHT
        };

        SDL_RenderCopy(data.SDL.renderer, data.SDL.textures.font, &src, &dst);

        x += FONT_WIDTH + FONT_PADDING;
    }
}

static void request_seeking(seek_mode mode, easyav1_timestamp timestamp)
{
    // Pending seeking, do nothing
    if (data.seek.mode != SEEK_NONE) {
        return;
    }

    SDL_LockMutex(data.SDL.thread.mutex.seek);

    data.seek.mode = mode;
    data.seek.timestamp = timestamp;

    SDL_UnlockMutex(data.SDL.thread.mutex.seek);
}

static void handle_events(void)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT || (ev.type == SDL_KEYUP && ev.key.keysym.sym == SDLK_ESCAPE)) {
            data.quit = 1;
        }

        if (ev.type == SDL_KEYUP && ev.key.keysym.sym == SDLK_RIGHT) {
            request_seeking(SEEK_FORWARD, SKIP_TIME_MS);
        }

        if (ev.type == SDL_KEYUP && ev.key.keysym.sym == SDLK_LEFT) {
            request_seeking(SEEK_BACKWARD, SKIP_TIME_MS);
        }

        if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT && ev.button.clicks == 2) {
            data.mouse.double_click = 1;
        }
    }
}

static void toggle_fullscreen(void)
{
    Uint32 flags = SDL_GetWindowFlags(data.SDL.window);

    if (flags & SDL_WINDOW_FULLSCREEN) {
        SDL_SetWindowFullscreen(data.SDL.window, 0);
    } else {
        SDL_SetWindowFullscreen(data.SDL.window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    }
}

static void handle_input(void)
{
    handle_events();

    int width;
    int height;
    SDL_GetWindowSize(data.SDL.window, &width, &height);

    int mouse_x;
    int mouse_y;
    int mouse_moved = 0;
    int mouse_was_pressed = data.mouse.pressed;

    data.mouse.pressed = (SDL_GetMouseState(&mouse_x, &mouse_y) & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;

    if (data.mouse.double_click) {
        data.mouse.double_click = 0;
        toggle_fullscreen();
    }

    if (mouse_x != data.mouse.x || mouse_y != data.mouse.y) {
        if ((mouse_x >= 0 && mouse_y >= 0 && mouse_x < width && mouse_y < height) ||
            mouse_was_pressed) {
            data.mouse.last_move_inside = SDL_GetTicks64();
            data.mouse.x = mouse_x;
            data.mouse.y = mouse_y;
            mouse_moved = 1;
        } else if (!mouse_was_pressed) {
            data.mouse.pressed = 0;
        }
    }

    easyav1_timestamp current = easyav1_get_current_timestamp(data.easyav1);
    unsigned int x_offset = get_timestamp_width(current) + TIME_BAR_SIDE_PADDING * 2 + 2;
    int time_bar_width = width - x_offset - TIME_BAR_SIDE_PADDING - 2;

    easyav1_timestamp hovered_timestamp = 0;

    if (mouse_x > 0 && mouse_x >= x_offset) {
        hovered_timestamp = easyav1_get_duration(data.easyav1) * (mouse_x - x_offset) / (float) time_bar_width;
    }

    int mouse_is_hovering_timestamp = mouse_x > x_offset && mouse_x <= width - TIME_BAR_SIDE_PADDING - 2 &&
        mouse_y > height - TIME_BAR_HEIGHT && mouse_y < height - 1;

    if (data.mouse.pressed) {
        if (mouse_is_hovering_timestamp || (mouse_was_pressed && mouse_moved)) {
            request_seeking(SEEK_TO, hovered_timestamp);
        }

        if (!mouse_is_hovering_timestamp && !mouse_was_pressed && !easyav1_is_finished(data.easyav1)) {
            data.playback.paused = !data.playback.paused;
            data.playback.last_change = SDL_GetTicks64();
        }
    }

    data.hovered_timestamp = mouse_is_hovering_timestamp ? hovered_timestamp : 0;
}

static void update_time_bar_status(void)
{
    unsigned int max_height = TIME_BAR_HEIGHT;
    easyav1_timestamp timestamp = SDL_GetTicks64();

    switch (data.time_bar.state) {
        case TIME_BAR_CLOSED:

            if (data.time_bar.state_start_time < data.mouse.last_move_inside) {
                data.time_bar.state = TIME_BAR_OPENING;
                data.time_bar.state_start_time = timestamp;
            }

            data.time_bar.y_offset = max_height;
            break;

        case TIME_BAR_OPENING:

            if (timestamp - data.time_bar.state_start_time > TIME_BAR_ANIMATION_MS) {
                data.time_bar.state = TIME_BAR_OPEN;
                data.time_bar.state_start_time = timestamp;
                data.time_bar.y_offset = 0;
            } else {
                data.time_bar.y_offset = max_height - max_height * (timestamp - data.time_bar.state_start_time) /
                    TIME_BAR_ANIMATION_MS;
            }

            break;

        case TIME_BAR_OPEN:

            if (data.mouse.last_move_inside >= data.time_bar.state_start_time || data.hovered_timestamp) {
                data.time_bar.state_start_time = timestamp;
            }

            if (timestamp - data.time_bar.state_start_time > TIME_BAR_OPEN_WAIT_TIME_MS) {
                data.time_bar.state = TIME_BAR_CLOSING;
                data.time_bar.state_start_time = timestamp;
            }

            data.time_bar.y_offset = 0;
            break;

        case TIME_BAR_CLOSING:

            if (timestamp - data.time_bar.state_start_time > TIME_BAR_ANIMATION_MS) {
                data.time_bar.state = TIME_BAR_CLOSED;
                data.time_bar.state_start_time = timestamp;
                data.time_bar.y_offset = max_height;
            } else if (data.mouse.last_move_inside >= data.time_bar.state_start_time) {
                data.time_bar.state = TIME_BAR_OPENING;
                easyav1_timestamp time_left = TIME_BAR_ANIMATION_MS - (timestamp - data.time_bar.state_start_time);
                data.time_bar.state_start_time = timestamp - time_left;
            } else {
                data.time_bar.y_offset = max_height * (timestamp - data.time_bar.state_start_time) /
                    TIME_BAR_ANIMATION_MS;
            }

            break;

        // Invalid state, mark as closed
        default:

            data.time_bar.state = TIME_BAR_CLOSED;
            data.time_bar.y_offset = max_height;

            break;
    }
}

static void draw_time_bar(void)
{
    update_time_bar_status();

    if (data.time_bar.state == TIME_BAR_CLOSED) {
        return;
    }

    int window_width;
    int window_height;
    SDL_GetWindowSize(data.SDL.window, &window_width, &window_height);

    SDL_SetRenderDrawBlendMode(data.SDL.renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(data.SDL.renderer, 0, 0, 0, 0x80);

    unsigned int y_offset = window_height - TIME_BAR_HEIGHT + data.time_bar.y_offset;

    SDL_Rect rect = {
        .x = 0,
        .y = y_offset,
        .w = window_width,
        .h = TIME_BAR_HEIGHT
    };

    SDL_RenderFillRect(data.SDL.renderer, &rect);

    SDL_SetRenderDrawColor(data.SDL.renderer, 255, 255, 255, 255);

    easyav1_timestamp duration = easyav1_get_duration(data.easyav1);
    easyav1_timestamp current = easyav1_get_current_timestamp(data.easyav1);

    if (data.hovered_timestamp) {
        draw_timestamp(data.mouse.x - get_timestamp_width(data.hovered_timestamp) / 2,
            y_offset + 12, data.hovered_timestamp);
    }

    draw_timestamp(TIME_BAR_SIDE_PADDING, y_offset + 30, current);
    unsigned int x_offset = get_timestamp_width(current) + TIME_BAR_SIDE_PADDING;

    rect.x = TIME_BAR_SIDE_PADDING + x_offset;
    rect.y = y_offset + 28;
    rect.w = window_width - 2 * TIME_BAR_SIDE_PADDING - x_offset;
    rect.h = 10;
    SDL_RenderDrawRect(data.SDL.renderer, &rect);

    if (duration > 0) {
        rect.x += 2;
        rect.y += 2;
        rect.w = (window_width - 2 * TIME_BAR_SIDE_PADDING - 4 - x_offset) * ((float) current / (float) duration);
        rect.h -= 4;
        SDL_RenderFillRect(data.SDL.renderer, &rect);
    }
}

static void draw_play_icon(unsigned int size, uint8_t opacity)
{
    size -= size % 2;

    if (size > MAX_PLAY_BUTTON_SIZE) {
        size = MAX_PLAY_BUTTON_SIZE;
    }

    int width;
    int height;
    SDL_GetWindowSize(data.SDL.window, &width, &height);

    int x = (width - size) / 2;
    int y = (height - size) / 2;

#ifdef HAS_RENDER_GEOMETRY
    SDL_Vertex vertices[3] = {
        { .position.x = x, .position.y = y, .color = { 255, 255, 255, opacity } },
        { .position.x = x + size, .position.y = y + size / 2, .color = { 255, 255, 255, opacity } },
        { .position.x = x, .position.y = y + size, .color = { 255, 255, 255, opacity } }
    };
    SDL_RenderGeometry(data.SDL.renderer, 0, vertices, 3, 0, 0);
#else
    SDL_SetRenderDrawColor(data.SDL.renderer, 255, 255, 255, opacity);
    SDL_RenderDrawLines(data.SDL.renderer, play_icon_points, size + size / 2 + 1);
    SDL_RenderDrawLine(data.SDL.renderer, x, y, x, y + size);
#endif
}

static void draw_pause_icon(unsigned int size, uint8_t opacity)
{
    size -= size % 2;

    if (size > MAX_PLAY_BUTTON_SIZE) {
        size = MAX_PLAY_BUTTON_SIZE;
    }

    int width;
    int height;
    SDL_GetWindowSize(data.SDL.window, &width, &height);

    int x = (width - size) / 2;
    int y = (height - size) / 2;

    SDL_Rect rects[2] = {
        { .x = x + size / 12, .y = y, .w = size / 3, .h = size },
        { .x = x + size / 3 + size / 6 + size / 12, .y = y, .w = size / 3, .h = size }
    };

    SDL_SetRenderDrawColor(data.SDL.renderer, 255, 255, 255, opacity);
    SDL_RenderFillRects(data.SDL.renderer, rects, 2);
}

static void draw_play_pause_animation(void)
{
    if (data.playback.last_change == 0) {
        return;
    }

    easyav1_timestamp diff = SDL_GetTicks64() - data.playback.last_change;

    if (diff >= PLAY_PAUSE_ANIMATION_MS) {
        return;
    }

    if (data.playback.paused) {
        draw_pause_icon(diff, 0xff - (diff * 0xff) / PLAY_PAUSE_ANIMATION_MS);
    } else {
        draw_play_icon(diff, 0xff - (diff * 0xff) / PLAY_PAUSE_ANIMATION_MS);
    }
}

int main(int argc, char **argv)
{
    if (!parse_options(argc, argv)) {
        return 1;
    }

    if (data.options.displaying_help) {
        display_help(argv[0]);
        return 0;
    }

    init_easyav1(data.options.filename);

    if (!data.easyav1) {
        printf("Failed to initialize easyav1.\n");

        return 2;
    }

    if (!init_sdl()) {
        printf("Failed to initialize SDL.\n");
        easyav1_destroy(&data.easyav1);

        return 3;
    }

    if (!init_fonts()) {
        printf("Failed to initialize fonts.\n");
        quit_sdl();
        easyav1_destroy(&data.easyav1);

        return 4;
    }

    init_ui();

    if (!init_decoder_thread()) {
        quit_sdl();
        easyav1_destroy(&data.easyav1);

        return 5;
    }

    while (!data.quit) {

        handle_input();

        SDL_RenderClear(data.SDL.renderer);

        if (easyav1_get_status(data.easyav1) == EASYAV1_STATUS_ERROR) {
            data.quit = 1;
            break;
        }

        SDL_LockMutex(data.SDL.thread.mutex.seek);
        SDL_LockMutex(data.SDL.thread.mutex.picture);

        if (easyav1_has_video_track(data.easyav1)) {

            const easyav1_video_frame *frame = easyav1_get_video_frame(data.easyav1);

            // SDL can only handle YUV420 8-bit per component frames, so we just ignore other formats
            if (frame && frame->picture_type == EASYAV1_PICTURE_TYPE_YUV420_8BPC) {
                SDL_UpdateYUVTexture(data.SDL.textures.video, NULL, frame->data[0], frame->stride[0], frame->data[1],
                    frame->stride[1], frame->data[2], frame->stride[2]);
                SDL_SetTextureColorMod(data.SDL.textures.video, 255, 255, 255);
            }

            SDL_RenderCopy(data.SDL.renderer, data.SDL.textures.video, NULL, NULL);
        }

        draw_time_bar();
        draw_play_pause_animation();

        SDL_UnlockMutex(data.SDL.thread.mutex.picture);
        SDL_UnlockMutex(data.SDL.thread.mutex.seek);

        SDL_RenderPresent(data.SDL.renderer);

        if (data.options.loop && easyav1_is_finished(data.easyav1)) {
            request_seeking(SEEK_TO, 0);
        }
    }

    exit_decoder_thread();

    quit_sdl();

    easyav1_destroy(&data.easyav1);

    return 0;
}
