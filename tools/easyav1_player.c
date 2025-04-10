#include "easyav1.h"

#include "SDL3/SDL.h"

#include <inttypes.h>
#include <stdio.h>
#include <time.h>

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

static SDL_Point play_icon_points[MAX_PLAY_BUTTON_SIZE + MAX_PLAY_BUTTON_SIZE / 2 + 1];

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
        SDL_AudioStream *audio_stream;
        struct {
            SDL_Thread *decoder;
            struct {
                SDL_Mutex *seek;
                SDL_Mutex *picture;
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
    float aspect_ratio;
    int quit;
    easyav1_timestamp hovered_timestamp;
    easyav1_stream file_stream;
    easyav1_t *easyav1;
    struct {
        int displaying_help;
        int loop;
        int fullscreen;
        int keep_aspect_ratio;
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
    { "keep_aspect_ratio", "ar", OPTION_TYPE_BOOL, &data.options.keep_aspect_ratio, "Keep the video's original aspect ratio regardless of window size." },
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
    SDL_PutAudioStreamData(data.SDL.audio_stream, frame->pcm.interlaced, frame->bytes);
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
                            *option_list[i].value_to_change = SDL_atoi(argv[count]);
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

        printf("  %s%-*s %s%-*s  %s  %s\n", name_prefix, (int) largest_name, name,
            abbr_prefix, (int) largest_abbr, abbr, type, option_list[option].description);
    }

    printf("\n");
}

static int sdl_stream_read(void* buffer, size_t size, void *userdata)
{
	SDL_IOStream *stream = (SDL_IOStream *)userdata;
	size_t result = SDL_ReadIO(stream, buffer, size);

    if (result > 0) {
        return 1;
    } else {
		return SDL_GetIOStatus(stream) == SDL_IO_STATUS_EOF ? 0 : -1;
    }
}

static int sdl_stream_seek(int64_t offset, int origin, void* userdata)
{
	SDL_IOStream* stream = (SDL_IOStream*)userdata;
	SDL_IOWhence whence;

	switch (origin) {
	    case SEEK_SET:
		    whence = SDL_IO_SEEK_SET;
		    break;
	    case SEEK_CUR:
		    whence = SDL_IO_SEEK_CUR;
		    break;
	    case SEEK_END:
		    whence = SDL_IO_SEEK_END;
		    break;
        default:
			return -1;
	}

	return SDL_SeekIO(stream, offset, whence) == -1 ? -1 : 0;
}

int64_t sdl_stream_tell(void* userdata)
{
	SDL_IOStream* stream = (SDL_IOStream*)userdata;
	return SDL_TellIO(stream);
}

int create_file_stream(void)
{
    SDL_IOStream *sdl_stream = SDL_IOFromFile(data.options.filename, "rb");

	if (!sdl_stream) {
		printf("Error opening file: %s\n", SDL_GetError());
		return 0;
	}

	data.file_stream.read_func = sdl_stream_read;
    data.file_stream.seek_func = sdl_stream_seek;
    data.file_stream.tell_func = sdl_stream_tell;
    data.file_stream.userdata = sdl_stream;

	return 1;
}

static void close_file_stream(void)
{
    if (data.file_stream.userdata) {
        SDL_CloseIO(data.file_stream.userdata);
    }
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

    if (!create_file_stream()) {
        return 0;
    }

    data.easyav1 = easyav1_init_from_custom_stream(&data.file_stream, &settings);
    if (!data.easyav1) {
		close_file_stream();
        return 0;
    }

    data.aspect_ratio = (float) easyav1_get_video_width(data.easyav1) / easyav1_get_video_height(data.easyav1);

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
    if (data.SDL.audio_stream) {
        SDL_DestroyAudioStream(data.SDL.audio_stream);
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
    SDL_InitFlags init_flags = SDL_INIT_VIDEO;

    if (easyav1_has_audio_track(data.easyav1)) {
        init_flags |= SDL_INIT_AUDIO;
    }

    if (!SDL_Init(init_flags)) {
        printf("Failed to initialize SDL. Reason: %s\n", SDL_GetError());
        return 0;
    }

    const SDL_DisplayMode *display_mode = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    if (!display_mode) {
		printf("Failed to get display mode! Reason %s\n", SDL_GetError());
        return 0;
    }

    unsigned int video_width = easyav1_get_video_width(data.easyav1);
    unsigned int video_height = easyav1_get_video_height(data.easyav1);

    if (video_width > display_mode->w - 10) {
        video_width = display_mode->w - 10;
    }
    if (video_height > display_mode->h - 100) {
        video_height = display_mode->h - 100;
    }

    SDL_WindowFlags window_flags = SDL_WINDOW_RESIZABLE;

    if (data.options.fullscreen) {
        window_flags |= SDL_WINDOW_FULLSCREEN;
    }

    data.SDL.window = SDL_CreateWindow("easyav1_player", video_width, video_height, window_flags);

    if (!data.SDL.window) {
        printf("Failed to create window. Reason: %s\n", SDL_GetError());
        quit_sdl();
        return 0;
    }

    data.SDL.renderer = SDL_CreateRenderer(data.SDL.window, NULL);

    if (!data.SDL.renderer) {
        printf("Failed to create renderer. Reason: %s\n", SDL_GetError());
        quit_sdl();
        return 0;
    }

    SDL_SetRenderVSync(data.SDL.renderer, 1);
    SDL_SetRenderDrawColor(data.SDL.renderer, 0, 0, 0, 255);
    SDL_RenderClear(data.SDL.renderer);
    SDL_RenderPresent(data.SDL.renderer);

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
    }

    if (easyav1_has_audio_track(data.easyav1)) {
        SDL_AudioSpec audio_spec = {
            .freq = easyav1_get_audio_sample_rate(data.easyav1),
            .format = SDL_AUDIO_F32,
            .channels = easyav1_get_audio_channels(data.easyav1)
        };

        data.SDL.audio_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &audio_spec, NULL, NULL);

        if (data.SDL.audio_stream == 0) {
            printf("Failed to open audio stream device. Reason: %s\n", SDL_GetError());
            quit_sdl();
            return 0;
        }

        SDL_ResumeAudioStreamDevice(data.SDL.audio_stream);
    }

    return 1;
}

static int init_fonts(void)
{
    SDL_Surface *font_surface = SDL_CreateSurface(FONT_WIDTH * FONT_IMAGE_COLS, FONT_HEIGHT * FONT_IMAGE_ROWS, SDL_PIXELFORMAT_RGBA8888);

    if (!font_surface) {
        printf("Failed to create font surface. Reason: %s\n", SDL_GetError());
        return 0;
    }

    const SDL_PixelFormatDetails *format_details = SDL_GetPixelFormatDetails(font_surface->format);

    for (int i = 0; i < FONT_IMAGE_COLS * FONT_IMAGE_ROWS; i++) {
        font_positions[i].x = i % FONT_IMAGE_COLS * FONT_WIDTH;
        font_positions[i].y = i / FONT_IMAGE_COLS * FONT_HEIGHT;

        SDL_Rect rect = { font_positions[i].x, font_positions[i].y, 1, 1 };

        for (int y = 0; y < FONT_HEIGHT; y++) {
            for (int x = 0; x < FONT_WIDTH; x++) {
                if (FONT[i][y] & (1 << x)) {
                    SDL_FillSurfaceRect(font_surface, &rect, SDL_MapRGBA(format_details, NULL, 0xff, 0xff, 0xff, 0xff));
                }
                rect.x += 1;
            }
            rect.x = font_positions[i].x;
            rect.y ++;
        }
    }

    data.SDL.textures.font = SDL_CreateTextureFromSurface(data.SDL.renderer, font_surface);

    SDL_DestroySurface(font_surface);

    if (!data.SDL.textures.font) {
        printf("Failed to create font texture. Reason: %s\n", SDL_GetError());
        return 0;
    }

    return 1;
}

static void init_ui(void)
{
    data.time_bar.state = TIME_BAR_OPEN;
    data.time_bar.state_start_time = SDL_GetTicks();

}

static int easyav1_decode_thread(void *userdata)
{
    easyav1_timestamp last_timestamp = SDL_GetTicks();
    easyav1_timestamp current_timestamp = last_timestamp;

    SDL_LockMutex(data.SDL.thread.mutex.picture);

    static easyav1_timestamp last_seek_time = 0;

    while (easyav1_decode_for(data.easyav1, current_timestamp - last_timestamp) != EASYAV1_STATUS_ERROR) {

        SDL_UnlockMutex(data.SDL.thread.mutex.picture);

        if (data.quit) {
            break;
        }

        int did_seek = 0;

        // Handle seeking
        if (data.seek.mode != SEEK_NONE) {

            SDL_LockMutex(data.SDL.thread.mutex.seek);

            SDL_ClearAudioStream(data.SDL.audio_stream);

            easyav1_timestamp timestamp = easyav1_get_current_timestamp(data.easyav1);

            int should_seek = 1;

            switch (data.seek.mode) {       
                case SEEK_BACKWARD:
                    data.seek.timestamp = SKIP_TIME_MS >= timestamp ? 0 : timestamp - SKIP_TIME_MS;
                    break;
                case SEEK_FORWARD:
                    data.seek.timestamp = timestamp + SKIP_TIME_MS;
                    break;
                case SEEK_TO:
                    if (last_seek_time == data.seek.timestamp) {
                        easyav1_timestamp difference = 0;
                        
                        if (data.seek.timestamp > timestamp) {
                            difference = data.seek.timestamp - timestamp;
                        } else {
                            difference = timestamp - data.seek.timestamp;
                        }

                        if (difference < 200) {
                            should_seek = 0;
                        }
                    }
                    break;
                default:
                    data.seek.mode = SEEK_NONE;
                    break;
            }

            data.seek.mode = SEEK_NONE;

            if (should_seek) {

                if (easyav1_seek_to_timestamp(data.easyav1, data.seek.timestamp) != EASYAV1_STATUS_OK) {
                    printf("Failed to seek to timestamp %" PRIu64 "\n", data.seek.timestamp);
                    data.quit = 1;

                    SDL_UnlockMutex(data.SDL.thread.mutex.seek);

                    break;
                }

                last_seek_time = data.seek.timestamp;

                did_seek = 1;

            }

            SDL_UnlockMutex(data.SDL.thread.mutex.seek);
        } else {
            // Prevent busy waiting
            if (current_timestamp == last_timestamp) {
                SDL_Delay(1);
            }
        }

        // Update timestamp
        last_timestamp = current_timestamp;
        current_timestamp = SDL_GetTicks();

        // Pause the video
        if (data.mouse.pressed || data.playback.paused || did_seek) {
            last_timestamp = current_timestamp;
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
        snprintf(buffer, size, "%" PRIu64 ":%02" PRIu64 ":%02" PRIu64, timestamp / 3600000,
            timestamp / 60000, (timestamp / 1000) % 60);
    } else {
        snprintf(buffer, size, "%" PRIu64 ":%02" PRIu64, timestamp / 60000, (timestamp / 1000) % 60);
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

        SDL_FRect src = {
            .x = font_positions[c].x,
            .y = font_positions[c].y,
            .w = FONT_WIDTH,
            .h = FONT_HEIGHT
        };

        SDL_FRect dst = {
            .x = x,
            .y = y,
            .w = FONT_WIDTH,
            .h = FONT_HEIGHT
        };

        SDL_RenderTexture(data.SDL.renderer, data.SDL.textures.font, &src, &dst);

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
        if (ev.type == SDL_EVENT_QUIT || (ev.type == SDL_EVENT_KEY_UP && ev.key.key == SDLK_ESCAPE)) {
            data.quit = 1;
        }

        if (ev.type == SDL_EVENT_KEY_UP && ev.key.key == SDLK_RIGHT) {
            request_seeking(SEEK_FORWARD, SKIP_TIME_MS);
        }

        if (ev.type == SDL_EVENT_KEY_UP && ev.key.key == SDLK_LEFT) {
            request_seeking(SEEK_BACKWARD, SKIP_TIME_MS);
        }

        if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev.button.button == SDL_BUTTON_LEFT && ev.button.clicks == 2) {
            data.mouse.double_click = 1;
        }
    }
}

static void toggle_fullscreen(void)
{
    SDL_WindowFlags flags = SDL_GetWindowFlags(data.SDL.window);

    if (flags & SDL_WINDOW_FULLSCREEN) {
        SDL_SetWindowFullscreen(data.SDL.window, false);
        SDL_ShowCursor();
    } else {
        SDL_SetWindowFullscreen(data.SDL.window, true);
    }
}

static void handle_input(void)
{
    handle_events();

    int width;
    int height;
    SDL_GetWindowSize(data.SDL.window, &width, &height);

    float mouse_x;
    float mouse_y;
    int mouse_moved = 0;
    int mouse_was_pressed = data.mouse.pressed;

    data.mouse.pressed = (SDL_GetMouseState(&mouse_x, &mouse_y) & SDL_BUTTON_LMASK) != 0;

    if (data.mouse.double_click) {
        data.mouse.double_click = 0;
        toggle_fullscreen();
    }

    if (mouse_x != data.mouse.x || mouse_y != data.mouse.y) {
        if ((mouse_x >= 0 && mouse_y >= 0 && mouse_x < width && mouse_y < height) ||
            mouse_was_pressed) {
            data.mouse.last_move_inside = SDL_GetTicks();
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
            data.playback.last_change = SDL_GetTicks();
        }
    }

    data.hovered_timestamp = mouse_is_hovering_timestamp ? hovered_timestamp : 0;
}

static void update_time_bar_status(void)
{
    unsigned int max_height = TIME_BAR_HEIGHT;
    easyav1_timestamp timestamp = SDL_GetTicks();

    int is_fullscreen = SDL_GetWindowFlags(data.SDL.window) & SDL_WINDOW_FULLSCREEN;

    switch (data.time_bar.state) {
        case TIME_BAR_CLOSED:

            if (data.time_bar.state_start_time < data.mouse.last_move_inside) {
                data.time_bar.state = TIME_BAR_OPENING;
                data.time_bar.state_start_time = timestamp;

                if (is_fullscreen) {
                    SDL_ShowCursor();
                }
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

                if (is_fullscreen) {
                    SDL_HideCursor();
                }
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

    SDL_FRect rect = {
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
    SDL_RenderRect(data.SDL.renderer, &rect);

    if (duration > 0) {
        rect.x += 2;
        rect.y += 2;
        rect.w = (window_width - 2 * TIME_BAR_SIDE_PADDING - 4 - x_offset) * ((float) current / (float) duration);
        rect.h -= 4;
        SDL_RenderFillRect(data.SDL.renderer, &rect);
    }
}

static void draw_play_icon(unsigned int size, float opacity)
{
    size -= size % 2;

    if (size > MAX_PLAY_BUTTON_SIZE) {
        size = MAX_PLAY_BUTTON_SIZE;
    }

    int width;
    int height;
    SDL_GetWindowSize(data.SDL.window, &width, &height);

    float x = (width - size) / 2.0f;
    float y = (height - size) / 2.0f;

    SDL_Vertex vertices[3] = {
        { .position.x = x, .position.y = y, .color = { 1, 1, 1, opacity } },
        { .position.x = x + size, .position.y = y + size / 2.0f, .color = { 1, 1, 1, opacity } },
        { .position.x = x, .position.y = y + size, .color = { 1, 1, 1, opacity } }
    };
    SDL_RenderGeometry(data.SDL.renderer, 0, vertices, 3, 0, 0);
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

    float x = (width - size) / 2.0f;
    float y = (height - size) / 2.0f;

    SDL_FRect rects[2] = {
        { .x = x + size / 12.0f, .y = y, .w = size / 3.0f, .h = size },
        { .x = x + size / 3.0f + size / 6 + size / 12.0f, .y = y, .w = size / 3.0f, .h = size }
    };

    SDL_SetRenderDrawColor(data.SDL.renderer, 255, 255, 255, opacity);
    SDL_RenderFillRects(data.SDL.renderer, rects, 2);
}

static void draw_play_pause_animation(void)
{
    if (data.playback.last_change == 0) {
        return;
    }

    easyav1_timestamp diff = SDL_GetTicks() - data.playback.last_change;

    if (diff >= PLAY_PAUSE_ANIMATION_MS) {
        return;
    }

    if (data.playback.paused) {
        draw_pause_icon(diff, 0xff - (diff * 0xff) / PLAY_PAUSE_ANIMATION_MS);
    } else {
        draw_play_icon(diff, 1.0f - (diff / (float) PLAY_PAUSE_ANIMATION_MS));
    }
}

static const SDL_FRect *get_aspect_ratio_rect(void)
{
    if (!data.options.keep_aspect_ratio) {
        return NULL;
    }

    int width;
    int height;

    SDL_GetWindowSize(data.SDL.window, &width, &height);

    static SDL_FRect rect;

    float window_aspect_ratio = width / (float) height;

    if (window_aspect_ratio > data.aspect_ratio) {
        float new_width = height * data.aspect_ratio;
        rect.x = (width - new_width) / 2.0f;
        rect.y = 0;
        rect.w = new_width;
        rect.h = height;
    } else {
        float new_height = width / data.aspect_ratio;
        rect.x = 0;
        rect.y = (height - new_height) / 2.0f;
        rect.w = width;
        rect.h = new_height;
    }

    return &rect;
}

static void selected_file(void *userdata, const char * const *filelist, int filter)
{
    if (!filelist) {
        printf("Error creating the file dialog window.\n");
        *((int*)userdata) = 1;
        return;
    }
    
    if (!*filelist) {
        *((int*)userdata) = 1;
        return;
    }

    static char filename[300];

    snprintf(filename, 300, "%s", filelist[0]);

    data.options.filename = filename;

    *((int*)userdata) = 1;
}

static int show_open_file_dialog(void)
{
    SDL_DialogFileFilter filter = { "WebM Video Files", "webm" };

    int file_chosen = 0;

    SDL_PropertiesID file_dialog_properties = SDL_CreateProperties();

    if (file_dialog_properties) {
        SDL_SetPointerProperty(file_dialog_properties, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, &filter);
        SDL_SetNumberProperty(file_dialog_properties, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, 1);
        SDL_SetStringProperty(file_dialog_properties, SDL_PROP_FILE_DIALOG_TITLE_STRING, "Please select a WebM video file:");
        SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFILE, selected_file, &file_chosen, file_dialog_properties);
    } else {
        SDL_ShowOpenFileDialog(selected_file, &file_chosen, NULL, &filter, 1, NULL, false);
    }

    while (!file_chosen) {
        SDL_Delay(5);
    }

    if (!data.options.filename) {
        return 0;
    }

    return 1;
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

    if (!data.options.filename && !show_open_file_dialog()) {
        const char *file_name = parse_file_name(argv[0]);

        printf("Usage: \"%s [OPTIONS] <filename>\"\n\n", file_name);  
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Select a file", "Please select a valid video file.", NULL);

        return 1;
    }

    init_easyav1(data.options.filename);

    if (!data.easyav1) {
        printf("Failed to initialize easyav1.\n");

        return 2;
    }

    if (!init_sdl()) {
        printf("Failed to initialize SDL.\n");
        easyav1_destroy(&data.easyav1);
		close_file_stream();

        return 3;
    }

    if (!init_fonts()) {
        printf("Failed to initialize fonts.\n");
        quit_sdl();
        easyav1_destroy(&data.easyav1);
        close_file_stream();

        return 4;
    }

    init_ui();

    if (!init_decoder_thread()) {
        quit_sdl();
        easyav1_destroy(&data.easyav1);
        close_file_stream();

        return 5;
    }

    while (!data.quit) {

        handle_input();

        SDL_SetRenderDrawColor(data.SDL.renderer, 0, 0, 0, 255);
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

            SDL_RenderTexture(data.SDL.renderer, data.SDL.textures.video, NULL, get_aspect_ratio_rect());
        }

        draw_time_bar();
        draw_play_pause_animation();

        SDL_UnlockMutex(data.SDL.thread.mutex.picture);
        SDL_UnlockMutex(data.SDL.thread.mutex.seek);

        SDL_RenderPresent(data.SDL.renderer);

        if (easyav1_is_finished(data.easyav1)) {
            SDL_FlushAudioStream(data.SDL.audio_stream);

            if (data.options.loop) {
                request_seeking(SEEK_TO, 0);
            }
        }
    }

    exit_decoder_thread();

    quit_sdl();

    easyav1_destroy(&data.easyav1);

    close_file_stream();

    return 0;
}
