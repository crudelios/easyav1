#include "easyav1.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
typedef struct {
    LARGE_INTEGER start;
    LARGE_INTEGER frequency;
} benchmark_clock;

void benchmark_clock_start(benchmark_clock *clock)
{
    memset(clock, 0, sizeof(benchmark_clock));

    QueryPerformanceCounter(&clock->start);
    QueryPerformanceFrequency(&clock->frequency);
}

int64_t benchmark_clock_get_elapsed_time(benchmark_clock *clock)
{
    LARGE_INTEGER current_time;
    QueryPerformanceCounter(&current_time);

    int64_t elapsed = current_time.QuadPart - clock->start.QuadPart;
    elapsed *= 1000;
    elapsed /= clock->frequency.QuadPart;

    return elapsed;
}

void benchmark_clock_reset_timer(benchmark_clock *clock)
{
    QueryPerformanceCounter(&clock->start);
}
#else 
#include <sys/time.h>
#include <time.h>

typedef struct {
    struct timespec start;
} benchmark_clock;

void benchmark_clock_start(benchmark_clock *clock)
{
    memset(clock, 0, sizeof(benchmark_clock));

    clock_gettime(CLOCK_MONOTONIC, &clock->start);
}

int64_t benchmark_clock_get_elapsed_time(benchmark_clock *clock)
{
    struct timespec current_time;
    clock_gettime(CLOCK_MONOTONIC, &current_time);

    int64_t elapsed = (current_time.tv_sec - clock->start.tv_sec) * 1000000000 +
                      current_time.tv_nsec - clock->start.tv_nsec;

    return elapsed / 1000000;
}

void benchmark_clock_reset_timer(benchmark_clock *clock)
{
    clock_gettime(CLOCK_MONOTONIC, &clock->start);
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
    settings.log_level = EASYAV1_LOG_LEVEL_ERROR;

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

    printf("Video duration: %" PRIu64 ":%02" PRIu64 " (%" PRIu64 " ms).\n",
        easyav1_get_duration(easyav1) / 60000, (easyav1_get_duration(easyav1) / 1000) % 60,
        easyav1_get_duration(easyav1));
    printf("Video size: %ux%u, %u FPS.\n", easyav1_get_video_width(easyav1), easyav1_get_video_height(easyav1),
        easyav1_get_video_fps(easyav1));
    fflush(stdout);

    benchmark_clock clock;
    benchmark_clock_start(&clock);
    int64_t total_time = 0;
    int64_t largest_frame_time = 0;
    easyav1_timestamp slowest_frame_timestamp = 0;
    uint64_t slowest_frame = 0;

    benchmark_clock printf_clock;
    benchmark_clock_start(&printf_clock);

    int printed_chars = 0;

    while (easyav1_decode_next(easyav1) == EASYAV1_STATUS_OK) {

        // Check if we have a video frame.
        if (easyav1_has_video_frame(easyav1) == EASYAV1_FALSE) {
            continue;
        }

        // Get the elapsed time.
        int64_t elapsed_time = benchmark_clock_get_elapsed_time(&clock);
        total_time += elapsed_time;

        // Check if this frame is the slowest one.
        if (elapsed_time > largest_frame_time) {
            largest_frame_time = elapsed_time;
            slowest_frame = easyav1_get_total_video_frames_processed(easyav1);
            slowest_frame_timestamp = easyav1_get_current_timestamp(easyav1);
        }

        // Update the progress every second.
        if (benchmark_clock_get_elapsed_time(&printf_clock) > 1000) {
            easyav1_timestamp current_timestamp = easyav1_get_current_timestamp(easyav1);
            uint64_t total_frames = easyav1_get_total_video_frames_processed(easyav1);
            double fps = total_frames / (total_time / 1000.0);
            double speed = current_timestamp / (double) (total_time == 0 ? 1 : total_time);


            int printed = printf("\rDecoding (%" PRIu64 ":%02" PRIu64 "): Decoded %" PRIu64 " frames in %" PRIu64 " ms (%lf fps average, %.3lfx).",
                current_timestamp / 60000, (current_timestamp / 1000) % 60, total_frames, total_time, fps, speed);

            while (printed_chars > printed) {
                printf(" ");
                printed_chars--;
            }

            printed_chars = printed;

            fflush(stdout);

            benchmark_clock_reset_timer(&printf_clock);
        }

        // Mark the frame as displayed.
        easyav1_get_video_frame(easyav1);

        // Restart the timer.
        benchmark_clock_reset_timer(&clock);
    }

    if (!easyav1_is_finished(easyav1)) {
        printf("\nFailed to decode the video.\n");
        easyav1_destroy(&easyav1);
        return 4;
    }

    uint64_t total_frames = easyav1_get_total_video_frames_processed(easyav1);
    easyav1_timestamp video_length = easyav1_get_duration(easyav1);
    double fps = total_frames / (total_time / 1000.0);
    double speed = video_length / (double) (total_time == 0 ? 1 : total_time);

    // Print the final result.
    int printed = printf("\rDecoded %" PRIu64 " frames in %" PRId64 " milliseconds (%lf fps average, %.3lfx).",
        total_frames, total_time, fps, speed);
    while (printed_chars > printed) {
        printf(" ");
        printed_chars--;
    }

    printf("\nSlowest frame: #%" PRIu64 " (at %" PRIu64 ":%02" PRIu64 ") - %" PRId64 " milliseconds (%lf fps).\n", slowest_frame,
        slowest_frame_timestamp / 60000, (slowest_frame_timestamp / 1000) % 60,
        largest_frame_time, 1000 / (double) largest_frame_time);

    easyav1_destroy(&easyav1);

    return 0;
}
