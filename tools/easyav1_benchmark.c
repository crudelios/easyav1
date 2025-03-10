#include "easyav1.h"

#include <stddef.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
typedef struct {
    LARGE_INTEGER start;
    LARGE_INTEGER end;
    LARGE_INTEGER frequency;
} benchmark_clock;

void benchmark_clock_start(benchmark_clock *clock)
{
    memset(clock, 0, sizeof(benchmark_clock));

    QueryPerformanceCounter(&clock->start);
    QueryPerformanceFrequency(&clock->frequency);
}

int64_t benchmark_clock_stop(benchmark_clock *clock)
{
    QueryPerformanceCounter(&clock->end);

    int64_t elapsed = clock->end.QuadPart - clock->start.QuadPart;
    elapsed *= 1000;
    elapsed /= clock->frequency.QuadPart;

    return elapsed;
}
#endif


int main(int argc, const char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <filename>\n", argv[0]);
        return 1;
    }

    easyav1_settings settings = easyav1_default_settings();
    settings.enable_audio = 0;
    settings.skip_unprocessed_frames = 0;

    easyav1_t *easyav1 = easyav1_init_from_filename(argv[1], &settings);

    if (!easyav1) {
        printf("Failed to initialize easyav1.\n");
        return 2;
    }

    if (!easyav1_has_video_track(easyav1)) {
        printf("The video does not contain a video track.\n");
        easyav1_destroy(&easyav1);
        return 3;
    }

    printf("Video size: %ux%u\n", easyav1_get_video_width(easyav1), easyav1_get_video_height(easyav1));

    benchmark_clock clock;
    benchmark_clock_start(&clock);

    while (easyav1_decode_next(easyav1) == EASYAV1_STATUS_OK);

    if (!easyav1_is_finished(easyav1)) {
        printf("Failed to decode the video.\n");
        easyav1_destroy(&easyav1);
        return 4;
    }

    int64_t elapsed = benchmark_clock_stop(&clock);
    uint64_t total_frames = easyav1_get_total_video_frames_processed(easyav1);
    double fps = total_frames / (elapsed / 1000.0);


    printf("Decoded %llu frames in %lld milliseconds (%lf fps).\n", total_frames, elapsed, fps);

    easyav1_destroy(&easyav1);

    return 0;
}
