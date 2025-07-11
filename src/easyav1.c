#include "easyav1.h"

#define OGG_IMPL
#define VORBIS_IMPL

#include "nestegg/nestegg.h"
#include "dav1d/dav1d.h"
#include "minivorbis/minivorbis.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <process.h>

/**
 * Windows-specific thread handling.
 *
 * This code is based on the pthreads implementation in the dav1d project.
 */

typedef struct {
    HANDLE h;
    void *(*func)(void *);
    void *arg;
} pthread_t;

typedef void pthread_attr_t;

typedef SRWLOCK pthread_mutex_t;
typedef CONDITION_VARIABLE pthread_cond_t;

#else
#include <pthread.h>
#endif

#define AUDIO_BUFFER_SIZE 4096
#define PACKET_QUEUE_BASE_CAPACITY 16
#define VIDEO_FRAMES_TO_PREFETCH 10
#define VIDEO_FRAME_QUEUE_SIZE (VIDEO_FRAMES_TO_PREFETCH + 1)

#define VORBIS_HEADERS_COUNT 3

#define DECODE_UNTIL_SKIP_MS 1000

#define INVALID_TIMESTAMP ((easyav1_timestamp) -1)

#define EASYAV1_STATUS_IS_ERROR(status) ((status) <= EASYAV1_STATUS_ERROR)


/**
 * Stream types - used to determine how to read the data
 */
typedef enum {
    STREAM_TYPE_NONE = 0, // No stream type
    STREAM_TYPE_FILE,     // Streaming from a file
    STREAM_TYPE_MEMORY    // Streaming from memory
} stream_type;


/**
 * Thread commands - used to control the thread state (pause, resume, stop)
 */
typedef enum {
    THREAD_COMMAND_NONE = 0,   // No command, keep current state
    THREAD_COMMAND_PAUSE = 1,  // Pause the thread
    THREAD_COMMAND_STOP = 2    // Stop the thread
} thread_command;


/**
 * Seeking mode - used to determine the current seeking state
 */
typedef enum {
    NOT_SEEKING = 0,            // Not seeking
    STARTING_SEEKING = 1,       // Starting seeking
    SEEKING_FOR_SQHDR = 2,      // Seeking for sequence header
    SEEKING_FOR_KEYFRAME = 3,   // Seeking for keyframe
    SEEKING_FOUND_KEYFRAME = 4, // Found keyframe, waiting for packets
    SEEKING_FOR_TIMESTAMP = 5   // Seeking for the requested timestamp
} seeking_mode;


/**
 * Packet type - whether the packet is video or audio
 */
typedef enum {
    PACKET_TYPE_VIDEO = 0, // Video packet
    PACKET_TYPE_AUDIO = 1  // Audio packet
} easyav1_packet_type;


/**
 * Packet data structure - used to store the webm packet data and metadata
 */
typedef struct {
    easyav1_timestamp timestamp; // The timestamp of the packet
    nestegg_packet *packet;      // The internal packet handle
    easyav1_bool is_keyframe;    // Whether the current packet is a keyframe
    easyav1_packet_type type;    // The type of the packet (video or audio)
    easyav1_bool decoded;        // Whether the packet has been decoded
    easyav1_bool is_seek_packet; // Whether the packet is a seek packet
} easyav1_packet;


/**
 * Packet queue - used to store the packets in a queue, to be processed later
 */
typedef struct {
    easyav1_packet *items; // The packets in the queue
    size_t count;          // The total number of items
    size_t capacity;       // The memory capacity of the queue
    size_t begin;          // The index of the first item in the queue
} easyav1_packet_queue;


/**
 * The main easyav1 structure - used to store all the data and metadata for the easyav1 library
 */
struct easyav1_t {

    /**
     * The WebM context - used to store the webm context and metadata
     */
    struct {
        nestegg *context;           // The webm context handle
        unsigned int num_tracks;    // The total number of tracks in the webm file
        unsigned int video_tracks;  // The number of video tracks in the webm file
        unsigned int audio_tracks;  // The number of audio tracks in the webm file
    } webm;


    /**
     * The video decoder context - used to store the video decoder context and metadata
     */
    struct {

        Dav1dContext *context;      // The video decoder context handle

        Dav1dSequenceHeader *sqhdr; // The current sequence header for the video decoder

        Dav1dPicture picture;       // The current picture being displayed

        easyav1_bool active;        // Whether the video decoder is active
        unsigned int track;         // The track number of the video in the webm container

        unsigned int width;         // The width of the video
        unsigned int height;        // The height of the video

        unsigned int fps;           // The frames per second of the video

        uint64_t processed_frames;  // The number of frames processed by the decoder

        easyav1_video_frame frame; // The current video frame data and metadata

        /**
         * The video frame queue - used to store the video frames in a queue, to be processed later
         */
        struct {
            Dav1dPicture frames[VIDEO_FRAME_QUEUE_SIZE]; // The video frames in the queue

            size_t count;        // The total number of items in the queue
            size_t begin;        // The index of the first item in the queue
        } frame_queue;


        /**
         * The video decoder thread - used to store the video decoder thread data and metadata
         */
        struct {

            /**
             * Structure holding the video decoder thread mutexts
             */
            struct {
                pthread_mutex_t io;      // The input and output mutex for the decoder thread - used to lock the packets and the video frame display queue
                pthread_mutex_t decoder; // The decoder mutex for the decoder thread - used to lock the video decoder context
                pthread_mutex_t info;    // The info mutex for the decoder thread - used to lock all other variables that are accessed by both threads
                pthread_mutex_t status;  // The status mutex for the decoder thread - used to lock the video decoder status
            } mutexes;

            /**
             * Structure holding the video decoder thread condition variables
             */
            struct {
                pthread_cond_t has_packets;               // Used to signal when there are new video frame packets to process
                pthread_cond_t has_frames_to_display;     // Used to signal when there are new video frames to display

                pthread_cond_t has_changed_status;        // Used to signal when the decoder status has changed
            } conditions;

            pthread_t decoder;      // The video decoder thread handle
            thread_command command; // The current command to give to the video decoder thread

        } decoder_thread;

    } video;


    /**
     * The audio decoder context - used to store the audio decoder context and metadata
     */
    struct {

        /**
         * Vorbis decoder data
         */
        struct {
            vorbis_info info;     // Info data
            vorbis_block block;   // Block data
            vorbis_dsp_state dsp; // DSP data
        } vorbis;

        easyav1_bool active;                // Whether the audio decoder is active
        unsigned int track;                 // The track number of the audio in the webm container

        unsigned int channels;              // The number of channels in the audio
        unsigned int sample_rate;           // The sample rate of the audio

        float *buffer;                      // The decoded audio buffer
        easyav1_bool has_samples_in_buffer; // Whether the audio buffer has samples in it

        easyav1_audio_frame frame;          // The current audio frame data and metadata

    } audio;


    /**
     * The packet queues - used to store the video and audio packets separate queues, to be processed later
     */
    struct {

        easyav1_bool synced;              // Whether the video and audio packets are synced
        easyav1_bool all_fetched;         // Whether all packets have been fetched

        easyav1_packet_queue video_queue; // The video packet queue
        easyav1_packet_queue audio_queue; // The audio packet queue

        int64_t audio_offset;             // The offset of the audio time in the webm container, in ms

    } packets;


    /**
     * The stream data - used to store the stream data and metadata
     */
    struct {
        void *data;       // Internal data handle
        stream_type type; // The type of stram in use
    } stream;


    /**
     * The seeking structure - used to handle seeking in the stream
     */
    struct {
        seeking_mode mode;           // The current seeking mode
        easyav1_timestamp timestamp; // The timestamp to seek to, in ms
    } seek;


    /**
     * The playback structure, used to handle playback from `easyav1_play`
     */
    struct {
        easyav1_bool active;   // Whether playback is active
        easyav1_bool do_pause; // Signals the playback thread to pause

        pthread_mutex_t mutex; // The playback mutex - used to lock the playback state
        pthread_t thread;      // The playback thread handle

        /**
         * Seeking during playback state
         * This is used to request a seek during playback, which will be processed in the playback thread
         */
        struct {
            easyav1_bool requested;      // Whether a seek is requested
            easyav1_timestamp timestamp; // The timestamp to seek to, in ms
        } seek;
    } playback;

    easyav1_status status;        // The current status of the easyav1 library

    easyav1_settings settings;    // The easyav1 settings data

    easyav1_timestamp position;   // The current position of the stream, in ms
    easyav1_timestamp duration;   // The total duration of the stream, in ms
    easyav1_timestamp time_scale; // The time scale conversion from the internal packet timestamp to ms

};


/**
 * Memory buffer data structure - used to navigate memory buffers
 */
typedef struct {
    uint8_t *data;  // The pointer to the buffer data
    int64_t offset; // The current offset in the buffer
    size_t size;    // The size of the buffer
} easyav1_memory;


/**
 * Default settings for the easyav1 library - used to set the default values for the easyav1 settings
 */
static const easyav1_settings DEFAULT_SETTINGS = {
    .enable_video = EASYAV1_TRUE,
    .enable_audio = EASYAV1_TRUE,
    .skip_unprocessed_frames = EASYAV1_TRUE,
    .interlace_audio = EASYAV1_TRUE,
    .close_handle_on_destroy = EASYAV1_FALSE,
    .callbacks = {
        .video = NULL,
        .audio = NULL,
        .userdata = NULL
    },
    .video_track = 0,
    .audio_track = 0,
    .use_fast_seeking = EASYAV1_FALSE,
    .audio_offset_time = 0,
    .log_level = EASYAV1_LOG_LEVEL_WARNING
};

/**
 * Decoder function type - used to define the decoder function signature
 */
typedef easyav1_status(*decoder_function)(easyav1_t *, easyav1_packet *, uint8_t *, size_t);


/**
 * Utility functions
 */

/**
  * @brief Converts a timestamp from the internal timestamp format to milliseconds.
  *
  * @param easyav1 The easyav1 context.
  * @param ns The timestamp in the internal format.
  *
  * @return The timestamp in milliseconds.
  */
static inline easyav1_timestamp internal_timestamp_to_ms(const easyav1_t *easyav1, easyav1_timestamp internal_ts)
{
    return internal_ts / easyav1->time_scale;
}

/**
  * @brief Converts a timestamp from milliseconds to the internal timestamp format.
  *
  * @param easyav1 The easyav1 context.
  * @param ms The timestamp in milliseconds.
  *
  * @return The timestamp in the internal format.
  */
static inline easyav1_timestamp ms_to_internal_timestmap(const easyav1_t *easyav1, easyav1_timestamp ms)
{
    return ms * easyav1->time_scale;
}


/**
 * Logging functions
 */

 /**
 * Logs a message to the console with the given log level, line number, function name, and format string.
 * The log level is used to determine the severity of the message, and the format string is used to format the message.
 *
 * @param level The log level of the message.
 *
 * @param ... The rest of the params are the same as `printf`.
 */
#define log(level, ...) \
if ((!easyav1 && DEFAULT_SETTINGS.log_level >= level) || (easyav1 && easyav1->settings.log_level >= level)) { \
    log_internal(level, __LINE__, __func__, __VA_ARGS__); \
}


 /**
 * Logs a message to the console with the given log level, line number, function name, and format string.
 *
 * @param level The log level of the message.
 * @param line The line number of the message.
 * @param func_name The function name of the message.
 * @param format The format string of the message.
 *
 * @param ... The rest of the params are the same as `printf`.
 */
static void log_internal(easyav1_log_level_t level, unsigned int line, const char *func_name, const char *format, ...)
{
    if (level == EASYAV1_LOG_LEVEL_ERROR) {
        fprintf(stderr, "(easyav1) Error: ");
    } else if (level == EASYAV1_LOG_LEVEL_WARNING) {
        fprintf(stderr, "(easyav1) Warning: ");
    } else {
        fprintf(stderr, "(easyav1) Info: ");
    }
    fprintf(stderr, "line %u (%s) - ", line, func_name);
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
    va_end(args);
    fflush(stderr);
}


/**
 * Log function that is called from nestegg webm decoder.
 */
static void log_from_nestegg(nestegg *ne, unsigned int severity, const char *format, ...)
{
    if (severity >= NESTEGG_LOG_CRITICAL) {
        fprintf(stderr, "(nestegg) Critical: ");
    } else if (severity >= NESTEGG_LOG_ERROR) {
        fprintf(stderr, "(nestegg) Error: ");
    } else if (severity >= NESTEGG_LOG_WARNING) {
        fprintf(stderr, "(nestegg) Warning: ");
    } else {
        return;
    }

    va_list args;

    va_start(args, format);

    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");

    va_end(args);

    fflush(stderr);
}


/**
 * Log function that is called from dav1d AV1 decoder.
 */
static void log_from_dav1d(void *userdata, const char *format, va_list args)
{
    fprintf(stderr, "(dav1d): ");
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");

    va_end(args);

    fflush(stderr);
}


/**
 * Prints a log message and sets the encoder to an error state
 *
 * @param error_type The error type to set the encoder to.
 * @param message The log message to print.
 *
 * @param ... The rest of the params are the same as printf.
 */
#define LOG_AND_SET_ERROR(error_type, message, ...) \
    log(EASYAV1_LOG_LEVEL_ERROR, message, ##__VA_ARGS__); \
    if (easyav1) { \
        easyav1->status = error_type; \
    } \


#ifdef _WIN32

/**
 * Threading functions - pthread compatibility layer for Windows
 */

/**
 * @brief Thread entry point function - used to start the thread and call the thread function
 *
 * @param data The thread data to pass to the thread function.
 *
 * @return Always `0`.
 */
static unsigned __stdcall thread_entrypoint(void *const data)
{
    pthread_t *const thread = data;
    thread->arg = thread->func(thread->arg);
    return 0;
}

/**
 * @brief Creates a new thread and starts it with the given function and argument.
 *
 * @param thread The thread to create.
 * @param attr The thread attributes to use (not used).
 * @param func The function to call in the thread.
 * @param arg The argument to pass to the function.
 *
 * @return `0` on success, `1` on error.
 */
static int pthread_create(pthread_t *const thread, const pthread_attr_t *const attr,
    void *(*const func)(void *), void *const arg)
{
    thread->func = func;
    thread->arg = arg;
    thread->h = (HANDLE) _beginthreadex(NULL, 0, thread_entrypoint, thread, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    return thread->h == NULL;
}

/**
 * @brief Joins the thread and waits for it to finish.
 *
 * @param thread The thread to join.
 * @param res The result of the thread function (if not NULL).
 *
 * @return `0` on success, `1` on error.
 */
static int pthread_join(pthread_t *const thread, void **const res)
{
    if (WaitForSingleObject(thread->h, INFINITE)) {
        return 1;
    }

    if (res) {
        *res = thread->arg;
    }

    return CloseHandle(thread->h) == 0;
}

/**
 * @brief Joins the thread and waits for it to finish. This is a macro that wraps the pthread_join function.
 *
 * @param thread The thread to join.
 * @param res The result of the thread function (if not NULL).
 *
 * @return `0` on success, `1` on error.
 */
#define pthread_join(thread, res) pthread_join(&(thread), res)

/**
 * @brief Initializes a mutex variable.
 *
 * @param mutex The mutex to initialize.
 * @param attr The mutex attributes to use (not used).
 *
 * @return `0` on success, `1` on error.
 */
static inline int pthread_mutex_init(pthread_mutex_t *const mutex, const void *const attr)
{
    InitializeSRWLock(mutex);
    return 0;
}

/**
 * @brief Destroys a mutex variable.
 *
 * @param mutex The mutex to destroy.
 *
 * @return `0` on success, `1` on error.
 */
static inline int pthread_mutex_destroy(pthread_mutex_t *const mutex)
{
    return 0;
}

/**
 * @brief Locks a mutex variable.
 *
 * @param mutex The mutex to lock.
 *
 * @return `0` on success, `1` on error.
 */
static inline int pthread_mutex_lock(pthread_mutex_t *const mutex)
{
    AcquireSRWLockExclusive(mutex);
    return 0;
}

/**
 * @brief Unlocks a mutex variable.
 *
 * @param mutex The mutex to unlock.
 *
 * @return `0` on success, `1` on error.
 */
static inline int pthread_mutex_unlock(pthread_mutex_t *const mutex)
{
    ReleaseSRWLockExclusive(mutex);
    return 0;
}

/**
 * @brief Initializes a condition variable.
 *
 * @param cond The condition variable to initialize.
 * @param attr The condition variable attributes to use (not used).
 *
 * @return `0` on success, `1` on error.
 */
static inline int pthread_cond_init(pthread_cond_t *const cond, const void *const attr)
{
    InitializeConditionVariable(cond);
    return 0;
}

/**
 * @brief Destroys a condition variable.
 *
 * @param cond The condition variable to destroy.
 *
 * @return `0` on success, `1` on error.
 */
static inline int pthread_cond_destroy(pthread_cond_t *const cond)
{
    return 0;
}

/**
 * @brief Waits for a condition to be signal.
 *
 * @param cond The condition variable to wait on.
 *
 * @return `0` on success, `1` on error.
 */
static inline int pthread_cond_wait(pthread_cond_t *const cond, pthread_mutex_t *const mutex)
{
    return SleepConditionVariableSRW(cond, mutex, INFINITE, 0) == 0;
}

/**
 * @brief Signals a condition variable.
 *
 * @param cond The condition variable to signal.
 *
 * @return `0` on success, `1` on error.
 */
static inline int pthread_cond_signal(pthread_cond_t *const cond)
{
    WakeConditionVariable(cond);
    return 0;
}

#endif // _WIN32


#ifdef __SWITCH__

#include <unistd.h>

// Hack to make sysconf work on Switch
// This is a workaround for the fact that sysconf is not implemented in the Switch SDK.
long sysconf(int value)
{
    return (value == _SC_NPROCESSORS_ONLN) ? 3 : -1;
}

#endif // __SWITCH__


/*
 * Time management functions
 */

/**
 * @brief Sleeps for a specified number of milliseconds.
 * 
 * @param ms The number of milliseconds to sleep.
 */
static inline void easyav1_sleep(unsigned int ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts = { 0 };
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;
    nanosleep(&ts, NULL);
#endif
}


/**
 * @brief Returns the number of milliseconds since this function was first called.
 * 
 * @return The number of milliseconds since this function was first called (`0`on the first call).
 */
static easyav1_timestamp easyav1_get_ticks(void)
{
#ifdef _WIN32
    static easyav1_timestamp start;
    if (!start) {
        start = GetTickCount64();
    }
    return GetTickCount64() - start;
#else
    static struct timespec start;
    struct timespec now;
    if (start.tv_sec == 0 && start.tv_nsec == 0) {
        clock_gettime(CLOCK_MONOTONIC, &start);
    }
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
#endif
}


/*
 * I/O functions
 */

/**
 * File read function - used to read data from a file
 *
 * @param buf The buffer to read the data into.
 * @param size The size of the data to read.
 * @param userdata The user data to pass to the read function.
 *
 * @return 1 if read was successful, 0 if the end of file was reached or -1 on error.
 */
static int file_read(void *buf, size_t size, void *userdata)
{
    FILE *f = (FILE *) userdata;
    if (!f) {
        return -1;
    }
    size_t r = fread(buf, 1, size, f);
    if (feof(f)) {
        return 0;
    }
    return r == size ? 1 : -1;
}


/**
 * File seek function - used to seek to a specific position in a file
 *
 * @param offset The offset to seek to.
 * @param origin The origin to seek from.
 * @param userdata The user data to pass to the seek function.
 *
 * @return 0 on success, -1 on error.
 */
static int file_seek(int64_t offset, int origin, void *userdata)
{
    FILE *f = (FILE *) userdata;
    if (!f) {
        return -1;
    }
#ifdef _WIN32
    return _fseeki64(f, offset, origin);
#else
    return fseek(f, offset, origin);
#endif
}


/**
 * File tell function - used to get the current position in a file
 *
 * @param userdata The user data to pass to the tell function.
 *
 * @return The current position in the file or -1 on error.
 */
static int64_t file_tell(void *userdata)
{
    FILE *f = (FILE *) userdata;
    if (!f) {
        return -1;
    }
#ifdef _WIN32
    return _ftelli64(f);
#else
    return ftell(f);
#endif
}


/**
 * Memory read function - used to read data from a memory buffer
 *
 * @param buf The buffer to read the data into.
 * @param size The size of the data to read.
 * @param userdata The user data to pass to the read function.
 *
 * @return 1 if read was successful, 0 if the end of file was reached or -1 on error.
 */
static int memory_read(void *buf, size_t size, void *userdata)
{
    easyav1_memory *mem = (easyav1_memory *) userdata;
    if (!mem || !mem->data) {
        return -1;
    }
    int has_more_bytes = 1;
    size_t max = mem->size - mem->offset;
    if (size > max) {
        size = max;
        has_more_bytes = 0;
    }
    if (!size) {
        return has_more_bytes;
    }
    memcpy(buf, mem->data + mem->offset, size);
    mem->offset += size;
    return has_more_bytes;
}


/**
 * Memory seek function - used to seek to a specific position in a memory buffer
 *
 * @param offset The offset to seek to.
 * @param origin The origin to seek from.
 * @param userdata The user data to pass to the seek function.
 *
 * @return 0 on success, -1 on error.
 */
static int memory_seek(int64_t offset, int origin, void *userdata)
{
    easyav1_memory *mem = (easyav1_memory *) userdata;
    if (!mem || !mem->data) {
        return -1;
    }
    if (origin == SEEK_SET) {
        mem->offset = offset;
    } else if (origin == SEEK_CUR) {
        mem->offset += offset;
    } else if (origin == SEEK_END) {
        mem->offset = mem->size - offset;
    }
    if (mem->offset > mem->size) {
        mem->offset = mem->size;
    }
    return 0;
}


/**
 * Memory tell function - used to get the current position in a memory buffer
 *
 * @param userdata The user data to pass to the tell function.
 *
 * @return The current position in the memory buffer or -1 on error.
 */
static int64_t memory_tell(void *userdata)
{
    easyav1_memory *mem = (easyav1_memory *) userdata;
    if (!mem || !mem->data) {
        return -1;
    }
    return mem->offset;
}


/*********************************************************************************
 * EasyAV1 decoder functions
 *********************************************************************************/

/**
 * @brief Initializes the requested video and audio tracks.
 *
 * @param easyav1 The easyav1 context to initialize the tracks for.
 *
 * @return `EASYAV1_STATUS_OK` on success, `EASYAV1_STATUS_ERROR` on error.
 */
static easyav1_status init_webm_tracks(easyav1_t *easyav1);

/**
 * @brief Initializes the requested video track.
 *
 * @param easyav1 The easyav1 context to initialize the video track for.
 * @param track The track number of the video in the webm container.
 *
 * @return `EASYAV1_STATUS_OK` on success, `EASYAV1_STATUS_ERROR` on error.
 */
static easyav1_status init_video(easyav1_t *easyav1, unsigned int track);

/**
 * @brief Initializes the requested audio track.
 *
 * @param easyav1 The easyav1 context to initialize the audio track for.
 * @param track The track number of the audio in the webm container.
 *
 * @return `EASYAV1_STATUS_OK` on success, `EASYAV1_STATUS_ERROR` on error.
 */
static easyav1_status init_audio(easyav1_t *easyav1, unsigned int track);

/**
 * @brief Initializes the video decoder thread.
 *
 * @param easyav1 The easyav1 context to initialize the video decoder thread for.
 *
 * @return `EASYAV1_STATUS_OK` on success, `EASYAV1_STATUS_ERROR` on error.
 */
static easyav1_status init_video_decoder_thread(easyav1_t *easyav1);

/**
 * @brief Sets up memory for the audio buffer, taking into account the number of channels and interlacing.
 *
 * @param easyav1 The easyav1 context to prepare the audio buffer for.
 *
 * @return `EASYAV1_STATUS_OK` on success, `EASYAV1_STATUS_ERROR` on error.
 */
static easyav1_status prepare_audio_buffer(easyav1_t *easyav1);

/**
 * @brief Increases the memory capacity of the packet queue to accommodate more packets.
 *
 * @param easyav1 The easyav1 context to increase the packet queue capacity for.
 * @param queue The packet queue to increase the capacity for.
 *
 * @return `EASYAV1_STATUS_OK` on success, `EASYAV1_STATUS_ERROR` on error.
 */
static easyav1_status increase_packet_queue_capacity(easyav1_t *easyav1, easyav1_packet_queue *queue);

/**
 * @brief Provides a new queue slot for a packet.
 *
 * @param easyav1 The easyav1 context to create the packet for.
 * @param queue The packet queue to add the packet to.
 *
 * @return A pointer to the new packet's queue slot or `NULL` on error.
 */
static easyav1_packet *queue_new_packet(easyav1_t *easyav1, easyav1_packet_queue *queue);

/**
 * @brief Gets the oldest video packet in the queue that hasn't been decoded yet.
 *
 * The packet is not removed from the queue.
 *
 * @param easyav1 The easyav1 context to get the packet from.
 * @param queue The video packet queue.
 *
 * @return A pointer to the oldest video packet in the queue or `NULL` if there are no packets to decode.
 */
static easyav1_packet *get_video_packet_to_decode(easyav1_t *easyav1, easyav1_packet_queue *queue);

/**
 * @brief Gets the oldest packet from the queue.
 *
 * The packet is not removed from the queue.
 *
 * @param easyav1 The easyav1 context to get the packet from.
 * @param queue The packet queue to get the packet from.
 *
 * @return A pointer to the oldest packet in the queue or `NULL` if there are no packets.
 */
static easyav1_packet *retrieve_first_packet_from_queue(easyav1_t *easyav1, easyav1_packet_queue *queue);

/**
 * @brief Gets the most recent packet from the queue.
 *
 * The packet is not removed from the queue.
 *
 * @param easyav1 The easyav1 context to get the packet from.
 * @param queue The packet queue to get the packet from.
 *
 * @return A pointer to the most recent packet in the queue or `NULL` if there are no packets.
 */
static easyav1_packet *retrieve_last_packet_from_queue(easyav1_t *easyav1, easyav1_packet_queue *queue);

/**
 * @brief Removes the oldest packet from the queue.
 *
 * @param easyav1 The easyav1 context to remove the packet from.
 * @param queue The packet queue to remove the packet from.
 */
static void release_packet_from_queue(easyav1_t *easyav1, easyav1_packet *packet);

/**
 * @brief Removes all packets from the queue.
 *
 * @param easyav1 The easyav1 context to remove the packets from.
 * @param queue The packet queue to remove the packets from.
 */
static void release_packets_from_queue(easyav1_t *easyav1, easyav1_packet_queue *queue);

/**
 * @brief Destroys the packet queue and frees the memory used by it.
 *
 * @param easyav1 The easyav1 context to destroy the packet queue for.
 * @param queue The packet queue to destroy.
 */
static void destroy_packet_queue(easyav1_t *easyav1, easyav1_packet_queue *queue);

/**
 * @brief Fetches a new WebM packet, allocates memory for it, sets its properties and adds it to the respective queue.
 *
 * @param easyav1 The easyav1 context to fetch the packet for.
 *
 * @return A pointer to the new packet or `NULL` on error.
 */
static easyav1_packet *prepare_new_packet(easyav1_t *easyav1);

/**
 * @brief Syncs the video and audio packets, feeding the video packet queue for the decoder thread and fetching audio packets
 * to respect the `audio_offset_time` setting.
 *
 * @param easyav1 The easyav1 context to sync the packet queues for.
 *
 * @return `EASYAV1_STATUS_OK` on success `EASYAV1_STATUS_FINISHED` if there are no more pending packets or
 * `EASYAV1_STATUS_ERROR` on error.
 */
static easyav1_status sync_packet_queues(easyav1_t *easyav1);

/**
 * @brief Gets the next packet from the video or audio queue, depending on the timestamp of the packet.
 *
 * @param easyav1 The easyav1 context to get the packet from.
 *
 * @return A pointer to the next packet to decode or `NULL` on error or if there are no more packets to fetch.
 */
static easyav1_packet *get_next_packet(easyav1_t *easyav1);

/**
 * @brief Adds a decoded video frame to the queue for display.
 *
 * If the queue is full, the oldest decoded frame is removed.
 *
 * @param easyav1 The easyav1 context to add the video frame to.
 * @param pic The decoded video frame to add to the queue.
 */
static void enqueue_video_frame(easyav1_t *easyav1, Dav1dPicture *pic);

/**
 * @brief Grabs the oldest video frame from the queue.
 *
 * The frame is not removed from the queue.
 *
 * @param easyav1 The easyav1 context to get the video frame from.
 *
 * @return A pointer to the oldest video frame in the queue or `NULL` if there are no frames to display.
 */
static Dav1dPicture *get_oldest_video_frame_from_queue(easyav1_t *easyav1);

/**
 * @brief Removes a video frame from the queue.
 *
 * @param easyav1 The easyav1 context to remove the video frame from.
 */
static void dequeue_video_frame(easyav1_t *easyav1);

/**
 * @brief Removes all video frames that have been displayed from the queue.
 *
 * @param easyav1 The easyav1 context to remove the video frames from.
 */
static void dequeue_used_video_frames(easyav1_t *easyav1);

/**
 * @brief Removes all video frames from the queue.
 *
 * @param easyav1 The easyav1 context to remove the video frames from.
 */
static void dequeue_all_video_frames(easyav1_t *easyav1);


/**
 * @brief Updates the frame picture type.
 *
 * @param easyav1 The easyav1 instance.
 * @param frame The video frame to update.
 * @param sqhdr The sequence header to use.
 *
 * @return `EASYAV1_TRUE` if the frame picture type is valid, `EASYAV1_FALSE` otherwise.
 */
static easyav1_bool update_frame_picture_type(easyav1_t *easyav1, easyav1_video_frame *frame, Dav1dSequenceHeader *sqhdr);


/**
 * @brief Calls the video callback function with the decoded video frame data.
 *
 * @param easyav1 The easyav1 context to call the video callback for.
 */
static void callback_video(easyav1_t *easyav1);

/**
 * @brief Calls the audio callback function with the decoded audio frame data.
 *
 * @param easyav1 The easyav1 context to call the audio callback for.
 */
static void callback_audio(easyav1_t *easyav1);

/**
 * @brief Video decoder thread function.
 *
 * Processes all video packets in the queue and decodes them.
 * If the queue is empty, it waits for new packets to be added.
 *
 * @param arg The easyav1 context to decode the video packets for.
 *
 * @return Always `0`.
 */
static void *video_decoder_thread(void *arg);

/**
 * @brief Pauses the video decoder thread.
 *
 * The video decoder thread is guaranteed to pause before processing a new packet from the queue.
 *
 * This function blocks until the decoder thread is paused.
 *
 * @param easyav1 The easyav1 context to pause the video decoder for.
 */
static void pause_video_decoder_thread(easyav1_t *easyav1);

/**
 * @brief Resumes the video decoder thread.
 *
 * @param easyav1 The easyav1 context to resume the video decoder for.
 */
static void resume_video_decoder_thread(easyav1_t *easyav1);

/**
 * @brief Stops the video decoder thread.
 *
 * This function blocks until the decoder thread is stopped (i.e. `video_decoder_thread` returns).
 *
 * @note This function expects the decoder thread to be paused before calling it.
 * Not doing so will result in undefined behavior.
 *
 * @param easyav1 The easyav1 context to stop the video decoder for.
 */
static void stop_video_decoder_thread(easyav1_t *easyav1);

/**
 * @brief Handles a command sent to the video decoder thread.
 *
 * The command is set by locking the `status` mutex and setting the `command` variable.
 * It is then processed by the decoder thread at the convenient time.
 * The command is reset to `THREAD_COMMAND_NONE` after processing a pause.
 *
 * @param easyav1 The easyav1 context to handle the command for.
 */
static thread_command handle_video_decoder_thread_command(easyav1_t *easyav1);

/**
 * @brief Callback function that seeks for a sequence header.
 *
 * This function is only called when the decoder is seeking for a sequence header and it runs from the main thread.
 * If a sequence header is found, the decoder is set to the `SEEKING_FOR_KEYFRAME` state.
 *
 * @param easyav1 The easyav1 context to seek for the sequence header for.
 * @param packet The packet to seek for the sequence header in.
 * @param data The packet data to seek for the sequence header in.
 * @param size The size of the data to seek for the sequence header in.
 *
 * @return `EASYAV1_STATUS_OK` on success, `EASYAV1_STATUS_ERROR` on error.
 */
static easyav1_status seek_sequence_header(easyav1_t *easyav1, easyav1_packet *packet, uint8_t *data, size_t size);

/**
 * @brief Video decoder function.
 *
 * Decodes the video packet data and adds it to the video frame queue.
 * This function always runs on the video decoder thread.
 *
 * @param easyav1 The easyav1 context to decode the video packet for.
 * @param packet The packet to decode.
 * @param pic The `Dav1dPicture` to decode the video packet into.
 *
 * @return `EASYAV1_STATUS_OK` on success, `EASYAV1_STATUS_ERROR` on error.
 */
static easyav1_status decode_video(easyav1_t *easyav1, easyav1_packet *packet, Dav1dPicture *pic);

/**
 * @brief Audio decoder function.
 *
 * Decodes the audio packet data and adds it to audio buffer.
 *
 * @param easyav1 The easyav1 context to decode the audio packet for.
 * @param packet The packet to decode.
 * @param data The packet data to decode.
 * @param size The size of the data to decode.
 *
 * @return `EASYAV1_STATUS_OK` on success, `EASYAV1_STATUS_ERROR` on error.
 */
static easyav1_status decode_audio(easyav1_t *easyav1, easyav1_packet *packet, uint8_t *data, size_t size);

/**
 * @brief Prepares the audio to store the new decoded samples.
 *
 * If the amount of decoded samples is larger than the free buffer space, existing samples are "shifted left" in the
 * buffer, with the oldest samples being removed.
 *
 * @param easyav1 The easyav1 context to prepare the audio buffer for.
 * @param decoded_samples The number of decoded samples that will be added to the buffer.
 *
 * @return The first offset in the decoded audio samples which can be added to the buffer.
 */
static unsigned int prepare_audio_buffer_for_new_samples(easyav1_t *easyav1, int decoded_samples);

/**
 * @brief Sends the packet data to the decoder function.
 *
 * This function gets the actual data from the WebM container and decodes it.
 *
 * @param easyav1 The easyav1 context to send the packet data to.
 * @param packet The packet to send the data from.
 * @param decode The decoder callback to send the data to.
 *
 * @return `EASYAV1_STATUS_OK` on success, `EASYAV1_STATUS_ERROR` on error.
 */
static easyav1_status send_packet_data_to_decoder(easyav1_t *easyav1, easyav1_packet *packet, decoder_function decode);

/**
 * @brief Requests the decoding of a single packet.
 *
 * For audio packets, the packet data is passed to the vorbis decoder and decoded immediately.
 * For video packets, if not seeking or seeking for the correct timestamp, the function checks if the decoder thread has
 * already decoded the packet. If not, the function waits for the packet to be decoded. If seeking for a sequence header
 * or a keyframe, the function does the checks on the main thread.
 *
 * @param easyav1 The easyav1 context to decode the packet for.
 * @param packet The packet to decode.
 *
 * @return `EASYAV1_STATUS_OK` on success, `EASYAV1_STATUS_ERROR` on error.
 */
static easyav1_status decode_packet(easyav1_t *easyav1, easyav1_packet *packet);

/**
 * @brief Decodes packets until the given timestamp is reached.
 *
 * @param easyav1 The easyav1 context to decode packets for.
 * @param timestamp The timestamp to decode packets until.
 * @return easyav1_status `EASYAV1_STATUS_OK` on success, `EASYAV1_STATUS_ERROR` on error.
 */
static easyav1_status do_decode_until(easyav1_t *easyav1, easyav1_timestamp timestamp);

/**
 * @brief Gets the closest cue point before the given timestamp.
 *
 * @param easyav1 The easyav1 context to get the cue point for.
 * @param timestamp The timestamp to get the cue point for.
 *
 * @return The closest cue point to the given timestamp. If there are no cue points, `0` is returned.
 */
static easyav1_timestamp get_closest_cue_point(const easyav1_t *easyav1, easyav1_timestamp timestamp);

/**
 * @brief Seeks to the given timestamp in the WebM file.
 *
 * This function seeks to the closest cue point before the given timestamp and then seeks to the exact timestamp.
 * If the timestamp is before the first cue point, it seeks to the beginning of the file.
 *
 * @param easyav1 The easyav1 context to seek in.
 * @param timestamp The timestamp to seek to.
 *
 * @return `EASYAV1_STATUS_OK` on success, `EASYAV1_STATUS_ERROR` on error.
 */
static easyav1_status do_seek_to_timestamp(easyav1_t *easyav1, easyav1_timestamp timestamp);

/**
 * @brief Changes the current video or audio track to the given track ID.
 *
 * @param easyav1 The easyav1 context to change the track for.
 * @param type The type of the track to change to (video or audio).
 * @param track_id The ID of the track to change to.
 *
 * @return `EASYAV1_STATUS_OK` on success, `EASYAV1_STATUS_ERROR` on error.
 */
static easyav1_status change_track(easyav1_t *easyav1, easyav1_packet_type type, unsigned int track_id);

/**
 * @brief Destroys the video decoder context and thread and frees the memory used by it.
 *
 * @param easyav1 The easyav1 context to destroy the video decoder for.
 */
static void destroy_video(easyav1_t *easyav1);

/**
 * @brief Destroys the audio decoder context and frees the memory used by it.
 *
 * @param easyav1 The easyav1 context to destroy the audio decoder for.
 */
static void destroy_audio(easyav1_t *easyav1);


/**
 * Initialization functions
 */

static easyav1_status init_webm_tracks(easyav1_t *easyav1)
{
    if (nestegg_track_count(easyav1->webm.context, &easyav1->webm.num_tracks)) {
        LOG_AND_SET_ERROR(EASYAV1_STATUS_DECODER_ERROR, "Failed to get track count");
        return EASYAV1_STATUS_ERROR;
    }

    easyav1->webm.video_tracks = 0;
    easyav1->webm.audio_tracks = 0;

    easyav1_bool has_video_track = EASYAV1_FALSE;
    easyav1_bool has_audio_track = EASYAV1_FALSE;

    unsigned int video_track;
    unsigned int audio_track;

    for (unsigned int track = 0; track < easyav1->webm.num_tracks; track++) {

        int type = nestegg_track_type(easyav1->webm.context, track);

        if (type == -1) {
            LOG_AND_SET_ERROR(EASYAV1_STATUS_DECODER_ERROR, "Failed to get track type.");
            return EASYAV1_STATUS_ERROR;
        }

        if (type == NESTEGG_TRACK_UNKNOWN) {
            log(EASYAV1_LOG_LEVEL_WARNING, "Unknown track type found, ignoring.");
            continue;
        }

        int codec = nestegg_track_codec_id(easyav1->webm.context, track);

        if (codec == -1) {
            LOG_AND_SET_ERROR(EASYAV1_STATUS_DECODER_ERROR, "Failed to get codec for track %u.", track);
            return EASYAV1_STATUS_ERROR;
        }

        if (type == NESTEGG_TRACK_VIDEO) {

            // Video already found or disabled - skip
            if (easyav1->settings.enable_video == EASYAV1_FALSE || has_video_track == EASYAV1_TRUE ||
                easyav1->webm.video_tracks != easyav1->settings.video_track) {
                easyav1->webm.video_tracks++;
                continue;
            }

            log(EASYAV1_LOG_LEVEL_INFO, "Found requested video track %u at webm track %u.",
                easyav1->webm.video_tracks, track);

            easyav1->webm.video_tracks++;

            if (codec != NESTEGG_CODEC_AV1) {
                log(EASYAV1_LOG_LEVEL_WARNING,
                    "Unsupported video codec found. Only AV1 codec is supported. Not displaying video.");
                continue;
            }

            has_video_track = EASYAV1_TRUE;
            video_track = track;
        }

        if (type == NESTEGG_TRACK_AUDIO) {

            // Audio already found or disabled - skip
            if (easyav1->settings.enable_audio == EASYAV1_FALSE || has_audio_track == EASYAV1_TRUE ||
                easyav1->webm.audio_tracks != easyav1->settings.audio_track) {
                easyav1->webm.audio_tracks++;
                continue;
            }

            log(EASYAV1_LOG_LEVEL_INFO, "Found requested audio track %u at webm track %u.",
                easyav1->webm.audio_tracks, track);

            easyav1->webm.audio_tracks++;

            if (codec != NESTEGG_CODEC_VORBIS) {
                log(EASYAV1_LOG_LEVEL_WARNING,
                    "Unsupported audio codec found. Only vorbis codec is supported. Not playing audio.");
                continue;
            }

            has_audio_track = EASYAV1_TRUE;
            audio_track = track;
        }
    }

    if (has_video_track == EASYAV1_TRUE) {
        if (init_video(easyav1, video_track) == EASYAV1_STATUS_ERROR) {
            return EASYAV1_STATUS_ERROR;
        }
    }

    if (has_audio_track == EASYAV1_TRUE) {
        if (init_audio(easyav1, audio_track) == EASYAV1_STATUS_ERROR) {
            return EASYAV1_STATUS_ERROR;
        }
    }

    log(EASYAV1_LOG_LEVEL_INFO, "Total video tracks: %u", easyav1->webm.video_tracks);
    log(EASYAV1_LOG_LEVEL_INFO, "Total audio tracks: %u", easyav1->webm.audio_tracks);

    return EASYAV1_STATUS_OK;
}

static easyav1_status init_video(easyav1_t *easyav1, unsigned int track)
{
    nestegg_video_params params;

    if (nestegg_track_video_params(easyav1->webm.context, track, &params)) {
        LOG_AND_SET_ERROR(EASYAV1_STATUS_DECODER_ERROR, "Failed to get video track parameters.");
        return EASYAV1_STATUS_ERROR;
    }

    easyav1_timestamp frame_duration;

    if (nestegg_track_default_duration(easyav1->webm.context, track, &frame_duration)) {
        LOG_AND_SET_ERROR(EASYAV1_STATUS_DECODER_ERROR, "Failed to get video track frame duration.");
        return EASYAV1_STATUS_ERROR;
    }

    easyav1->video.fps = ms_to_internal_timestmap(easyav1, 1000) / frame_duration;

    Dav1dSettings dav1d_settings;
    dav1d_default_settings(&dav1d_settings);
    dav1d_settings.logger = (Dav1dLogger) { .cookie = 0, .callback = log_from_dav1d };

    if (dav1d_open(&easyav1->video.context, &dav1d_settings) < 0) {
        LOG_AND_SET_ERROR(EASYAV1_STATUS_DECODER_ERROR, "Failed to initialize AV1 decoder.");
        return EASYAV1_STATUS_ERROR;
    }

    easyav1->video.active = EASYAV1_TRUE;
    easyav1->video.track = track;

    easyav1->video.width = params.width;
    easyav1->video.height = params.height;

    easyav1->video.sqhdr = NULL;

    if (init_video_decoder_thread(easyav1) == EASYAV1_STATUS_ERROR) {
        return EASYAV1_STATUS_ERROR;
    }

    log(EASYAV1_LOG_LEVEL_INFO, "Video initialized. Size: %ux%u, %u FPS.", easyav1->video.width, easyav1->video.height,
        easyav1->video.fps);

    return EASYAV1_STATUS_OK;
}

static easyav1_status init_audio(easyav1_t *easyav1, unsigned int track)
{
    unsigned int headers;

    if (nestegg_track_codec_data_count(easyav1->webm.context, track, &headers)) {
        LOG_AND_SET_ERROR(EASYAV1_STATUS_DECODER_ERROR, "Failed to get audio codec header count.");
        return EASYAV1_STATUS_ERROR;
    }

    // Vorbis data should always have 3 headers
    if (headers != VORBIS_HEADERS_COUNT) {
        LOG_AND_SET_ERROR(EASYAV1_STATUS_DECODER_ERROR, "Vorbis data should always have 3 headers.");
        return EASYAV1_STATUS_ERROR;
    }

    vorbis_comment comment;

    vorbis_info_init(&easyav1->audio.vorbis.info);
    vorbis_comment_init(&comment);

    for (int header = 0; header < VORBIS_HEADERS_COUNT; header++) {
        unsigned char *header_data;
        size_t header_size;

        if (nestegg_track_codec_data(easyav1->webm.context, track, header, &header_data, &header_size)) {
            LOG_AND_SET_ERROR(EASYAV1_STATUS_DECODER_ERROR, "Failed to get audio codec header data for header %u.",
                header);
            return EASYAV1_STATUS_ERROR;
        }

        ogg_packet packet;
        packet.packet = header_data;
        packet.bytes = header_size;
        packet.b_o_s = header == 0;
        packet.e_o_s = header == VORBIS_HEADERS_COUNT - 1;
        packet.granulepos = 0;
        packet.packetno = header;

        if (vorbis_synthesis_headerin(&easyav1->audio.vorbis.info, &comment, &packet)) {
            LOG_AND_SET_ERROR(EASYAV1_STATUS_DECODER_ERROR, "Failed to process audio codec header %u.", header);
            return EASYAV1_STATUS_ERROR;
        }
    }

    vorbis_comment_clear(&comment);

    if (vorbis_synthesis_init(&easyav1->audio.vorbis.dsp, &easyav1->audio.vorbis.info)) {
        LOG_AND_SET_ERROR(EASYAV1_STATUS_DECODER_ERROR, "Failed to initialize vorbis synthesis.");
        return EASYAV1_STATUS_ERROR;
    }

    if (vorbis_block_init(&easyav1->audio.vorbis.dsp, &easyav1->audio.vorbis.block)) {
        LOG_AND_SET_ERROR(EASYAV1_STATUS_DECODER_ERROR, "Failed to initialize vorbis block.");
        return EASYAV1_STATUS_ERROR;
    }

    nestegg_audio_params params;
    if (nestegg_track_audio_params(easyav1->webm.context, track, &params)) {
        LOG_AND_SET_ERROR(EASYAV1_STATUS_DECODER_ERROR, "Failed to get audio track parameters.");
        return EASYAV1_STATUS_ERROR;
    }

    easyav1->audio.active = EASYAV1_TRUE;
    easyav1->audio.track = track;

    easyav1->audio.channels = params.channels;
    easyav1->audio.sample_rate = params.rate;
    easyav1->packets.audio_offset = easyav1->settings.audio_offset_time +
        internal_timestamp_to_ms(easyav1, params.codec_delay);

    if (prepare_audio_buffer(easyav1) == EASYAV1_STATUS_ERROR) {
        return EASYAV1_STATUS_ERROR;
    }

    log(EASYAV1_LOG_LEVEL_INFO, "Audio initialized. Channels: %u, sample rate: %uhz.",
        easyav1->audio.channels, easyav1->audio.sample_rate);

    return EASYAV1_STATUS_OK;
}

static easyav1_status init_video_decoder_thread(easyav1_t *easyav1)
{
    if (easyav1->video.decoder_thread.command != THREAD_COMMAND_NONE) {
        return EASYAV1_STATUS_ERROR;
    }

    if (pthread_mutex_init(&easyav1->video.decoder_thread.mutexes.io, NULL) ||
        pthread_mutex_init(&easyav1->video.decoder_thread.mutexes.decoder, NULL) ||
        pthread_mutex_init(&easyav1->video.decoder_thread.mutexes.info, NULL) ||
        pthread_mutex_init(&easyav1->video.decoder_thread.mutexes.status, NULL)) {
        LOG_AND_SET_ERROR(EASYAV1_STATUS_DECODER_ERROR, "Failed to create decoder thread mutexes.");
        return EASYAV1_STATUS_ERROR;
    }

    if (pthread_cond_init(&easyav1->video.decoder_thread.conditions.has_packets, NULL) ||
        pthread_cond_init(&easyav1->video.decoder_thread.conditions.has_frames_to_display, NULL) ||
        pthread_cond_init(&easyav1->video.decoder_thread.conditions.has_changed_status, NULL)) {
        LOG_AND_SET_ERROR(EASYAV1_STATUS_DECODER_ERROR, "Failed to create decoder thread condition variable.");
        return EASYAV1_STATUS_ERROR;
    }

    if (pthread_create(&easyav1->video.decoder_thread.decoder, NULL, video_decoder_thread, easyav1)) {
        LOG_AND_SET_ERROR(EASYAV1_STATUS_DECODER_ERROR, "Failed to create decoder thread.");
        return EASYAV1_STATUS_ERROR;
    }

    return EASYAV1_STATUS_OK;
}

static easyav1_status prepare_audio_buffer(easyav1_t *easyav1)
{
    unsigned max_samples = AUDIO_BUFFER_SIZE * easyav1->audio.channels;

    easyav1->audio.buffer = malloc(max_samples * sizeof(float));

    if (!easyav1->audio.buffer) {
        LOG_AND_SET_ERROR(EASYAV1_STATUS_OUT_OF_MEMORY, "Failed to allocate audio buffer.");
        return EASYAV1_STATUS_ERROR;
    }

    memset(easyav1->audio.buffer, 0, max_samples * sizeof(float));

    easyav1->audio.frame.channels = easyav1->audio.channels;

    if (easyav1->settings.interlace_audio) {
        easyav1->audio.frame.pcm.interlaced = easyav1->audio.buffer;
        return EASYAV1_STATUS_OK;
    }

    easyav1->audio.frame.pcm.deinterlaced = malloc(easyav1->audio.channels * sizeof(float *));
    if (!easyav1->audio.frame.pcm.deinterlaced) {
        LOG_AND_SET_ERROR(EASYAV1_STATUS_OUT_OF_MEMORY, "Failed to allocate deinterlaced audio buffer.");
        return EASYAV1_STATUS_ERROR;
    }
    for (unsigned int i = 0; i < easyav1->audio.channels; i++) {
        easyav1->audio.frame.pcm.deinterlaced[i] = easyav1->audio.buffer + i * AUDIO_BUFFER_SIZE;
    }

    return EASYAV1_STATUS_OK;
}

easyav1_t *easyav1_init_from_custom_stream(const easyav1_stream *stream, const easyav1_settings *settings)
{
    easyav1_t *easyav1 = NULL;

    if (!stream || !stream->read_func || !stream->seek_func || !stream->tell_func) {
        LOG_AND_SET_ERROR(EASYAV1_STATUS_INVALID_ARGUMENT, "Stream is NULL or missing read, seek or tell functions");
        return NULL;
    }

    easyav1 = malloc(sizeof(easyav1_t));

    if (!easyav1) {
        LOG_AND_SET_ERROR(EASYAV1_STATUS_OUT_OF_MEMORY, "Failed to allocate memory for the easyav1 handle");
        return NULL;
    }

    memset(easyav1, 0, sizeof(easyav1_t));

    easyav1->status = EASYAV1_STATUS_OK;

    if (settings) {
        easyav1->settings = *settings;
    } else {
        easyav1->settings = DEFAULT_SETTINGS;
        log(EASYAV1_LOG_LEVEL_INFO, "No settings provided, using default settings.");
    }

    nestegg_io io = {
        .read = stream->read_func,
        .seek = stream->seek_func,
        .tell = stream->tell_func,
        .userdata = stream->userdata
    };

    if (nestegg_init(&easyav1->webm.context, io, log_from_nestegg, -1)) {
        LOG_AND_SET_ERROR(EASYAV1_STATUS_INVALID_STATE, "Failed to initialize webm context");
        easyav1_destroy(&easyav1);
        return NULL;
    }

    easyav1_timestamp duration;

    if (nestegg_duration(easyav1->webm.context, &duration)) {
        LOG_AND_SET_ERROR(EASYAV1_STATUS_DECODER_ERROR, "Failed to get duration");
        easyav1_destroy(&easyav1);
        return NULL;
    }

    if (nestegg_tstamp_scale(easyav1->webm.context, &easyav1->time_scale)) {
        LOG_AND_SET_ERROR(EASYAV1_STATUS_DECODER_ERROR, "Failed to get time scale.");
        easyav1_destroy(&easyav1);
        return NULL;
    }

    if (easyav1->time_scale == 0) {
        LOG_AND_SET_ERROR(EASYAV1_STATUS_DECODER_ERROR, "Time scale is 0.");
        easyav1_destroy(&easyav1);
        return NULL;
    }

    easyav1->duration = internal_timestamp_to_ms(easyav1, duration);

    log(EASYAV1_LOG_LEVEL_INFO, "File duration: %llu minutes and %llu seconds.",
        easyav1->duration / 60000, (easyav1->duration / 1000) % 60);

    if (init_webm_tracks(easyav1) == EASYAV1_STATUS_ERROR || sync_packet_queues(easyav1) != EASYAV1_STATUS_OK) {
        easyav1_destroy(&easyav1);
        return NULL;
    }

    return easyav1;
}

easyav1_t *easyav1_init_from_memory(const void *data, size_t size, const easyav1_settings *settings)
{
    easyav1_t *easyav1 = NULL;

    if (!data || !size) {
        log(EASYAV1_LOG_LEVEL_ERROR, "Data is NULL or size is 0");
        return NULL;
    }

    easyav1_memory *mem = malloc(sizeof(easyav1_memory));

    if (!mem) {
        log(EASYAV1_LOG_LEVEL_ERROR, "Failed to allocate memory for memory stream");
        return NULL;
    }

    mem->data = (uint8_t *) data;
    mem->size = size;
    mem->offset = 0;

    easyav1_stream memory_stream = {
        .read_func = memory_read,
        .seek_func = memory_seek,
        .tell_func = memory_tell,
        .userdata = mem
    };

    easyav1 = easyav1_init_from_custom_stream(&memory_stream, settings);

    if (!easyav1) {
        log(EASYAV1_LOG_LEVEL_ERROR, "Failed to create easyav1 handle from memory stream");
        free(mem);
        return NULL;
    }

    easyav1->stream.type = STREAM_TYPE_MEMORY;
    easyav1->stream.data = mem;

    return easyav1;
}

easyav1_t *easyav1_init_from_file(FILE *f, const easyav1_settings *settings)
{
    easyav1_t *easyav1 = NULL;

    if (!f) {
        log(EASYAV1_LOG_LEVEL_ERROR, "File handle is NULL");
        return NULL;
    }

    easyav1_stream file_stream = {
        .read_func = file_read,
        .seek_func = file_seek,
        .tell_func = file_tell,
        .userdata = f
    };

    easyav1 = easyav1_init_from_custom_stream(&file_stream, settings);

    if (!easyav1) {
        log(EASYAV1_LOG_LEVEL_ERROR, "Failed to create easyav1 structure from file handle");
        if ((settings && settings->close_handle_on_destroy) || (!settings && DEFAULT_SETTINGS.close_handle_on_destroy)) {
            fclose(f);
        }
        return NULL;
    }

    if (easyav1->settings.close_handle_on_destroy) {
        easyav1->stream.type = STREAM_TYPE_FILE;
        easyav1->stream.data = f;
    }

    return easyav1;
}

easyav1_t *easyav1_init_from_filename(const char *filename, const easyav1_settings *settings)
{
    easyav1_t *easyav1 = NULL;

    if (!filename) {
        log(EASYAV1_LOG_LEVEL_ERROR, "Filename is NULL");
        return NULL;
    }

    FILE *f = fopen(filename, "rb");

    if (!f) {
        log(EASYAV1_LOG_LEVEL_ERROR, "Failed to open file %s", filename);
        return NULL;
    }

    easyav1 = easyav1_init_from_file(f, settings);

    if (!easyav1) {
        log(EASYAV1_LOG_LEVEL_ERROR, "Failed to create easyav1 structure from file %s", filename);
        fclose(f);
        return NULL;
    }

    easyav1->stream.type = STREAM_TYPE_FILE;
    easyav1->stream.data = f;

    return easyav1;
}


/**
 * WebM packet handling functions
 */

static easyav1_status increase_packet_queue_capacity(easyav1_t *easyav1, easyav1_packet_queue *queue)
{
    size_t new_capacity = queue->capacity + PACKET_QUEUE_BASE_CAPACITY;
    easyav1_packet *new_queue = malloc(new_capacity * sizeof(easyav1_packet));

    if (!new_queue) {
        LOG_AND_SET_ERROR(EASYAV1_STATUS_OUT_OF_MEMORY, "Failed to allocate memory for packet queue.");
        return EASYAV1_STATUS_ERROR;
    }

    if (queue->capacity) {
        memcpy(new_queue, queue->items + queue->begin, (queue->capacity - queue->begin) * sizeof(easyav1_packet));
        memcpy(new_queue + queue->capacity - queue->begin, queue->items, queue->begin * sizeof(easyav1_packet));
    }

    memset(new_queue + queue->capacity, 0, PACKET_QUEUE_BASE_CAPACITY * sizeof(easyav1_packet));

    // This is the only instance where we need to lock the decoder thread mutexes, because we are changing
    // the packet queue's memory location and the video decoder may be using its data at the same time.
    pthread_mutex_lock(&easyav1->video.decoder_thread.mutexes.decoder);

    free(queue->items);

    queue->items = new_queue;
    queue->capacity = new_capacity;
    queue->begin = 0;

    pthread_mutex_unlock(&easyav1->video.decoder_thread.mutexes.decoder);

    return EASYAV1_STATUS_OK;
}

static easyav1_packet *queue_new_packet(easyav1_t *easyav1, easyav1_packet_queue *queue)
{
    if (queue->count == queue->capacity) {
        if (increase_packet_queue_capacity(easyav1, queue) == EASYAV1_STATUS_ERROR) {
            return NULL;
        }
    }

    size_t index = (queue->begin + queue->count) % queue->capacity;

    queue->count++;

    return &queue->items[index];
}

static easyav1_packet *get_video_packet_to_decode(easyav1_t *easyav1, easyav1_packet_queue *queue)
{
    if (!queue->count) {
        return NULL;
    }

    size_t packets_after_timestamp = 0;

    pthread_mutex_lock(&easyav1->video.decoder_thread.mutexes.info);

    easyav1_timestamp timestamp = easyav1->position;
    
    pthread_mutex_unlock(&easyav1->video.decoder_thread.mutexes.info);

    for (size_t i = 0; i < queue->count && packets_after_timestamp < VIDEO_FRAMES_TO_PREFETCH; i++) {
        size_t index = (queue->begin + i) % queue->capacity;

        easyav1_packet *packet = &queue->items[index];

        if (packet->type == PACKET_TYPE_VIDEO && packet->packet != NULL && packet->decoded == EASYAV1_FALSE) {
            return packet;
        }

        if (packet->timestamp > timestamp) {
            packets_after_timestamp++;
        }
    }

    return NULL;
}

static easyav1_packet *retrieve_first_packet_from_queue(easyav1_t *easyav1, easyav1_packet_queue *queue)
{
    if (!queue->count) {
        return NULL;
    }

    return &queue->items[queue->begin];
}

static easyav1_packet *retrieve_last_packet_from_queue(easyav1_t *easyav1, easyav1_packet_queue *queue)
{
    if (!queue->count) {
        return NULL;
    }

    return &queue->items[(queue->begin + queue->count - 1) % queue->capacity];
}

static void release_packet_from_queue(easyav1_t *easyav1, easyav1_packet *packet)
{
    if (!packet) {
        return;
    }

    if (packet->packet) {
        nestegg_free_packet(packet->packet);
    }

    easyav1_packet_queue *queue = packet->type == PACKET_TYPE_VIDEO ?
        &easyav1->packets.video_queue : &easyav1->packets.audio_queue;

    // Should never happen
    if (packet != queue->items + queue->begin) {
        log(EASYAV1_LOG_LEVEL_WARNING, "Released packet was not at the beginning of the queue.");
    }

    queue->begin = (queue->begin + 1) % queue->capacity;
    queue->count--;

    // Resync list position
    if (queue->count == 0) {
        queue->begin = 0;
    }

    memset(packet, 0, sizeof(easyav1_packet));

    if (easyav1->packets.all_fetched == EASYAV1_FALSE) {
        easyav1->packets.synced = EASYAV1_FALSE;
    } else {
        if (easyav1->packets.video_queue.count == 0 && easyav1->packets.audio_queue.count == 0) {

            pthread_mutex_lock(&easyav1->video.decoder_thread.mutexes.info);

            easyav1->status = EASYAV1_STATUS_FINISHED;

            pthread_mutex_unlock(&easyav1->video.decoder_thread.mutexes.info);
        }
    }
}

static void release_packets_from_queue(easyav1_t *easyav1, easyav1_packet_queue *queue)
{
    while (queue->count) {
        release_packet_from_queue(easyav1, queue->items + queue->begin);
    }
}

static void destroy_packet_queue(easyav1_t *easyav1, easyav1_packet_queue *queue)
{
    release_packets_from_queue(easyav1, queue);
    free(queue->items);
    memset(queue, 0, sizeof(easyav1_packet_queue));
}

static easyav1_packet *prepare_new_packet(easyav1_t *easyav1)
{
    nestegg_packet *packet = NULL;

    int status = nestegg_read_packet(easyav1->webm.context, &packet);

    if (status < 0) {
        LOG_AND_SET_ERROR(EASYAV1_STATUS_DECODER_ERROR, "Failed to read packet.");
        return NULL;
    }

    if (status == 0) {
        easyav1->packets.all_fetched = EASYAV1_TRUE;
        return NULL;
    }

    unsigned int track;

    if (nestegg_packet_track(packet, &track)) {
        nestegg_free_packet(packet);
        LOG_AND_SET_ERROR(EASYAV1_STATUS_DECODER_ERROR, "Failed to get track number from packet.");
        return NULL;
    }

    int track_type = nestegg_track_type(easyav1->webm.context, track);

    if (track_type == -1) {
        nestegg_free_packet(packet);
        LOG_AND_SET_ERROR(EASYAV1_STATUS_DECODER_ERROR, "Failed to get track type for track %u.", track);
        return NULL;
    } else if (track_type == NESTEGG_TRACK_UNKNOWN) {
        log(EASYAV1_LOG_LEVEL_INFO, "Skipping unknown track %u of type %d.", track, track_type);
        nestegg_free_packet(packet);
        return NULL;
    }

    easyav1_packet_type type = track_type == NESTEGG_TRACK_VIDEO ? PACKET_TYPE_VIDEO : PACKET_TYPE_AUDIO;

    if (type == PACKET_TYPE_VIDEO &&
        (easyav1->video.active == EASYAV1_FALSE || easyav1->video.track != track)) {
        nestegg_free_packet(packet);
        return NULL;
    }

    if (type == PACKET_TYPE_AUDIO &&
        (easyav1->audio.active == EASYAV1_FALSE || easyav1->audio.track != track)) {
        nestegg_free_packet(packet);
        return NULL;
    }

    easyav1_timestamp packet_timestamp;

    if (nestegg_packet_tstamp(packet, &packet_timestamp)) {
        nestegg_free_packet(packet);
        LOG_AND_SET_ERROR(EASYAV1_STATUS_DECODER_ERROR, "Failed to get packet timestamp.");
        return NULL;
    }

    packet_timestamp = internal_timestamp_to_ms(easyav1, packet_timestamp);

    if (type == PACKET_TYPE_AUDIO) {
        if (easyav1->packets.audio_offset < 0 && -easyav1->packets.audio_offset > packet_timestamp) {
            nestegg_free_packet(packet);
            return NULL;
        }

        if (easyav1->packets.audio_offset > 0 &&
            packet_timestamp + easyav1->packets.audio_offset > easyav1->duration) {
            nestegg_free_packet(packet);
            return NULL;
        }

        packet_timestamp += easyav1->packets.audio_offset;
    }

    int has_keyframe = nestegg_packet_has_keyframe(packet);
    if (has_keyframe == -1) {
        nestegg_free_packet(packet);
        LOG_AND_SET_ERROR(EASYAV1_STATUS_DECODER_ERROR, "Failed to get keyframe status.");
        return NULL;
    }

    easyav1_bool should_lock_thread = easyav1->seek.mode == NOT_SEEKING || easyav1->seek.mode == SEEKING_FOR_TIMESTAMP ?
        EASYAV1_TRUE : EASYAV1_FALSE;

    if (type == PACKET_TYPE_VIDEO && should_lock_thread) {
        pthread_mutex_lock(&easyav1->video.decoder_thread.mutexes.io);
    }

    easyav1_packet *new_packet = queue_new_packet(easyav1, type == PACKET_TYPE_VIDEO ?
        &easyav1->packets.video_queue : &easyav1->packets.audio_queue);

    if (!new_packet) {
        if (type == PACKET_TYPE_VIDEO && should_lock_thread) {
            pthread_mutex_unlock(&easyav1->video.decoder_thread.mutexes.io);
        }
        nestegg_free_packet(packet);
        LOG_AND_SET_ERROR(EASYAV1_STATUS_OUT_OF_MEMORY, "Failed to allocate memory for new packet.");
        return NULL;
    }

    new_packet->packet = packet;
    new_packet->timestamp = packet_timestamp;
    new_packet->is_keyframe = has_keyframe == NESTEGG_PACKET_HAS_KEYFRAME_TRUE ? EASYAV1_TRUE : EASYAV1_FALSE;
    new_packet->type = type;
    new_packet->is_seek_packet = easyav1->seek.mode != NOT_SEEKING && packet_timestamp <= easyav1->seek.timestamp;

    if (type == PACKET_TYPE_VIDEO && should_lock_thread) {
        pthread_mutex_unlock(&easyav1->video.decoder_thread.mutexes.io);
        pthread_cond_signal(&easyav1->video.decoder_thread.conditions.has_packets);
    }

    return new_packet;
}


/**
 * @brief Indicates whether we need to fetch at least one packet.
 *
 * @param easyav1 The easyav1 context to check.
 * @return `EASYAV1_TRUE` if we need to fetch at least one packet, `EASYAV1_FALSE` otherwise. 
 */
static inline easyav1_bool must_fetch_one_packet(easyav1_t *easyav1)
{
    return easyav1->packets.video_queue.count == 0 && easyav1->packets.audio_queue.count == 0;
}


/**
 * @brief Indicates whether we need to fetch more video packets.
 * 
 * @param easyav1 The easyav1 context to check.
 *
 * @return `EASYAV1_TRUE` if we need to fetch more video packets, `EASYAV1_FALSE` otherwise.
 */
static easyav1_bool must_fetch_video_packets(easyav1_t *easyav1)
{
    if (easyav1->video.active == EASYAV1_FALSE ||
        (easyav1->seek.mode != NOT_SEEKING && easyav1->seek.mode != SEEKING_FOR_TIMESTAMP)) {
        return EASYAV1_FALSE;
    }

    if (easyav1->packets.video_queue.count > VIDEO_FRAMES_TO_PREFETCH) {
        return EASYAV1_FALSE;
    }

    return EASYAV1_TRUE;
}


/**
 * @brief Indicates whether we need to fetch more audio packets.
 *
 * @param easyav1 The easyav1 context to check.
 * @return `EASYAV1_TRUE` if we need to fetch more audio packets, `EASYAV1_FALSE` otherwise.
 */
static easyav1_bool must_fetch_audio_packets(easyav1_t *easyav1)
{
    // No audio or performing seek - skip
    if (easyav1->audio.active == EASYAV1_FALSE) {
        return EASYAV1_FALSE;
    }

    // No need to prefetch audio - skip
    if (easyav1->packets.audio_offset >= 0) {
        return EASYAV1_FALSE;
    }

    easyav1_packet *audio_packet = retrieve_last_packet_from_queue(easyav1, &easyav1->packets.audio_queue);

    return audio_packet == NULL || audio_packet->timestamp < easyav1->position ? EASYAV1_TRUE : EASYAV1_FALSE;
}

static easyav1_status sync_packet_queues(easyav1_t *easyav1)
{
    if (easyav1->status == EASYAV1_STATUS_FINISHED) {
        return EASYAV1_STATUS_FINISHED;
    }

    if (easyav1->packets.synced == EASYAV1_TRUE || easyav1->packets.all_fetched == EASYAV1_TRUE) {
        return EASYAV1_STATUS_OK;
    }

    while (easyav1->packets.all_fetched == EASYAV1_FALSE &&
        (must_fetch_one_packet(easyav1) || must_fetch_video_packets(easyav1) || must_fetch_audio_packets(easyav1))) {

        if (!prepare_new_packet(easyav1) && EASYAV1_STATUS_IS_ERROR(easyav1->status)) {
            return EASYAV1_STATUS_ERROR;
        }

    }

    easyav1->packets.synced = EASYAV1_TRUE;

    if (easyav1->packets.video_queue.count == 0 && easyav1->packets.audio_queue.count == 0 &&
        easyav1->packets.all_fetched == EASYAV1_TRUE) {
        easyav1->status = EASYAV1_STATUS_FINISHED;
    }

    return EASYAV1_STATUS_OK;
}

static easyav1_packet *get_next_packet(easyav1_t *easyav1)
{
    if (easyav1->status == EASYAV1_STATUS_FINISHED) {
        return NULL;
    }

    if (sync_packet_queues(easyav1) != EASYAV1_STATUS_OK) {
        return NULL;
    }

    easyav1_packet *video_packet = retrieve_first_packet_from_queue(easyav1, &easyav1->packets.video_queue);
    easyav1_packet *audio_packet = retrieve_first_packet_from_queue(easyav1, &easyav1->packets.audio_queue);

    if (!video_packet) {
        return audio_packet;
    }

    if (!audio_packet) {
        return video_packet;
    }

    if (video_packet->timestamp <= audio_packet->timestamp) {
        return video_packet;
    }
    
    return audio_packet;
}


/**
 * Video frame queue functions
 */

static void enqueue_video_frame(easyav1_t *easyav1, Dav1dPicture *pic)
{
    if (easyav1->video.frame_queue.count >= VIDEO_FRAME_QUEUE_SIZE) {
        dequeue_video_frame(easyav1);
    }

    size_t index = (easyav1->video.frame_queue.begin + easyav1->video.frame_queue.count) % VIDEO_FRAME_QUEUE_SIZE;
    memcpy(&easyav1->video.frame_queue.frames[index], pic, sizeof(Dav1dPicture));
    easyav1->video.frame_queue.count++;
}

static Dav1dPicture *get_oldest_video_frame_from_queue(easyav1_t *easyav1)
{
    if (easyav1->video.frame_queue.count == 0) {
        return NULL;
    }

    return &easyav1->video.frame_queue.frames[easyav1->video.frame_queue.begin];
}

static void dequeue_video_frame(easyav1_t *easyav1)
{
    Dav1dPicture *pic = get_oldest_video_frame_from_queue(easyav1);

    if (!pic) {
        return;
    }

    if (pic->frame_hdr) {
        dav1d_picture_unref(pic);
    }

    easyav1->video.frame_queue.count--;

    if (easyav1->video.frame_queue.count == 0) {
        easyav1->video.frame_queue.begin = 0;
    } else {
        easyav1->video.frame_queue.begin = (easyav1->video.frame_queue.begin + 1) % VIDEO_FRAME_QUEUE_SIZE;
    }
}

static void dequeue_all_video_frames(easyav1_t *easyav1)
{
    while (easyav1->video.frame_queue.count > 0) {
        dequeue_video_frame(easyav1);
    }
}


/**
 * Decoding functions
 */

static void callback_video(easyav1_t *easyav1)
{
    if (!easyav1->settings.callbacks.video) {
        return;
    }

    const easyav1_video_frame *frame = easyav1_get_video_frame(easyav1);

    if (!frame) {
        return;
    }

    easyav1->settings.callbacks.video(frame, easyav1->settings.callbacks.userdata);
}

static void callback_audio(easyav1_t *easyav1)
{
    if (!easyav1->settings.callbacks.audio) {
        return;
    }

    const easyav1_audio_frame *frame = easyav1_get_audio_frame(easyav1);

    if (!frame) {
        return;
    }

    easyav1->settings.callbacks.audio(frame, easyav1->settings.callbacks.userdata);
}

static void pause_video_decoder_thread(easyav1_t *easyav1)
{
    pthread_mutex_lock(&easyav1->video.decoder_thread.mutexes.status);

    easyav1->video.decoder_thread.command = THREAD_COMMAND_PAUSE;

    while (easyav1->video.decoder_thread.command == THREAD_COMMAND_PAUSE) {
        // Force the video decoder thread to wake up and check the command
        // This is necessary because the thread may be waiting for a packet
        // and won't check the command until it gets one
        pthread_cond_signal(&easyav1->video.decoder_thread.conditions.has_packets);
        pthread_cond_wait(&easyav1->video.decoder_thread.conditions.has_changed_status,
            &easyav1->video.decoder_thread.mutexes.status);
    }
}

static void resume_video_decoder_thread(easyav1_t *easyav1)
{
    pthread_mutex_unlock(&easyav1->video.decoder_thread.mutexes.status);
    pthread_cond_signal(&easyav1->video.decoder_thread.conditions.has_changed_status);
}

static void stop_video_decoder_thread(easyav1_t *easyav1)
{
    easyav1->video.decoder_thread.command = THREAD_COMMAND_STOP;

    resume_video_decoder_thread(easyav1);

    pthread_join(easyav1->video.decoder_thread.decoder, NULL);
}

static thread_command handle_video_decoder_thread_command(easyav1_t *easyav1)
{
    pthread_mutex_lock(&easyav1->video.decoder_thread.mutexes.status);

    if (easyav1->video.decoder_thread.command == THREAD_COMMAND_PAUSE) {
        easyav1->video.decoder_thread.command = THREAD_COMMAND_NONE;
        pthread_cond_signal(&easyav1->video.decoder_thread.conditions.has_changed_status);
        pthread_cond_wait(&easyav1->video.decoder_thread.conditions.has_changed_status,
            &easyav1->video.decoder_thread.mutexes.status);
    }

    thread_command command = easyav1->video.decoder_thread.command;

    pthread_mutex_unlock(&easyav1->video.decoder_thread.mutexes.status);

    return command;
}

static void *video_decoder_thread(void *arg)
{
    easyav1_t *easyav1 = (easyav1_t *) arg;

    while (!EASYAV1_STATUS_IS_ERROR(easyav1->status)) {

        if (handle_video_decoder_thread_command(easyav1) == THREAD_COMMAND_STOP) {
            break;
        }
    
        pthread_mutex_lock(&easyav1->video.decoder_thread.mutexes.io);

        easyav1_packet *packet = get_video_packet_to_decode(easyav1, &easyav1->packets.video_queue);

        while (packet == NULL) {
            pthread_cond_wait(&easyav1->video.decoder_thread.conditions.has_packets,
                &easyav1->video.decoder_thread.mutexes.io);

            pthread_mutex_unlock(&easyav1->video.decoder_thread.mutexes.io);

            if (handle_video_decoder_thread_command(easyav1) == THREAD_COMMAND_STOP) {
                return 0;
            }

            pthread_mutex_lock(&easyav1->video.decoder_thread.mutexes.io);

            packet = get_video_packet_to_decode(easyav1, &easyav1->packets.video_queue);
        }

        pthread_mutex_lock(&easyav1->video.decoder_thread.mutexes.decoder);
        pthread_mutex_unlock(&easyav1->video.decoder_thread.mutexes.io);

        Dav1dPicture pic = { 0 };

        easyav1_status status = decode_video(easyav1, packet, &pic);

        if (EASYAV1_STATUS_IS_ERROR(status)) {
            pthread_mutex_unlock(&easyav1->video.decoder_thread.mutexes.decoder);
            log(EASYAV1_LOG_LEVEL_ERROR, "Failed to decode video packet.");
            return 0;
        }

        pthread_mutex_unlock(&easyav1->video.decoder_thread.mutexes.decoder);

        pthread_mutex_lock(&easyav1->video.decoder_thread.mutexes.io);

        if (packet->is_seek_packet == EASYAV1_TRUE) {
            dequeue_video_frame(easyav1);
        }

        enqueue_video_frame(easyav1, &pic);

        packet->decoded = EASYAV1_TRUE;

        pthread_mutex_unlock(&easyav1->video.decoder_thread.mutexes.io);

        pthread_cond_signal(&easyav1->video.decoder_thread.conditions.has_frames_to_display);
    }

    log(EASYAV1_LOG_LEVEL_INFO, "Video decoder thread exiting.");

    return 0;
}

static easyav1_status seek_sequence_header(easyav1_t *easyav1, easyav1_packet *packet, uint8_t *data, size_t size)
{
    Dav1dSequenceHeader sequence_header;

    int result = dav1d_parse_sequence_header(&sequence_header, data, size);

    if (result < 0 && result != DAV1D_ERR(ENOENT)) {
        LOG_AND_SET_ERROR(EASYAV1_STATUS_DECODER_ERROR, "Failed to parse sequence header");
        return EASYAV1_STATUS_ERROR;
    }

    if (result == 0) {
        easyav1->seek.mode = SEEKING_FOR_KEYFRAME;
    }

    return EASYAV1_STATUS_OK;
}

/**
 * Dummy function that is passed to the AV1 decoder to free the data buffer.
 * This is a no-op function that does nothing.
 */
static void free_nothing(const uint8_t *data, void *cookie)
{}

static easyav1_status decode_video(easyav1_t *easyav1, easyav1_packet *packet, Dav1dPicture *pic)
{
    unsigned int chunks;

    if (nestegg_packet_count(packet->packet, &chunks)) {
        LOG_AND_SET_ERROR(EASYAV1_STATUS_DECODER_ERROR, "Failed to get packet count");
        return EASYAV1_STATUS_ERROR;
    }

    easyav1_bool has_picture = EASYAV1_FALSE;

    for (unsigned int chunk = 0; chunk < chunks; chunk++) {
        unsigned char *data;
        size_t size;

        if (nestegg_packet_data(packet->packet, chunk, &data, &size)) {
            LOG_AND_SET_ERROR(EASYAV1_STATUS_DECODER_ERROR, "Failed to get data from packet");
            return EASYAV1_STATUS_ERROR;
        }

        Dav1dData buf = { 0 };
        int result = dav1d_data_wrap(&buf, data, size, free_nothing, 0);

        if (result < 0) {
            LOG_AND_SET_ERROR(EASYAV1_STATUS_DECODER_ERROR, "Failed to create data buffer");
            return EASYAV1_STATUS_ERROR;
        }

        do {

            result = dav1d_send_data(easyav1->video.context, &buf);
            if (result < 0 && result != DAV1D_ERR(EAGAIN)) {
                LOG_AND_SET_ERROR(EASYAV1_STATUS_DECODER_ERROR, "Failed to send data to AV1 decoder");
                dav1d_data_unref(&buf);
                return EASYAV1_STATUS_ERROR;
            }

            Dav1dPicture temp_pic = { 0 };

            result = dav1d_get_picture(easyav1->video.context, &temp_pic);

            // For some reason, sometimes we need to insist on getting the picture without sending more data
            if (result == DAV1D_ERR(EAGAIN)) {
                result = dav1d_get_picture(easyav1->video.context, &temp_pic);
            }

            if (result < 0) {
                if (result == DAV1D_ERR(EAGAIN)) {
                    continue;
                }
                LOG_AND_SET_ERROR(EASYAV1_STATUS_DECODER_ERROR, "Failed to get picture from AV1 decoder");
                dav1d_data_unref(&buf);
                return EASYAV1_STATUS_ERROR;
            }

            easyav1->video.processed_frames++;

            if (has_picture == EASYAV1_FALSE) {
                *pic = temp_pic;
                pic->m.timestamp = packet->timestamp;
                has_picture = EASYAV1_TRUE;
            } else {
                log(EASYAV1_LOG_LEVEL_WARNING, "Multiple pictures in single packet are not supported.");
                dav1d_picture_unref(&temp_pic);
            }

        } while (buf.sz > 0);

        dav1d_data_unref(&buf);
    }

    return EASYAV1_STATUS_OK;
}

static easyav1_status decode_audio(easyav1_t *easyav1, easyav1_packet *packet, uint8_t *data, size_t size)
{
    ogg_packet audio_packet = {
        .packet = data,
        .bytes = size
    };

    if (easyav1->audio.has_samples_in_buffer == EASYAV1_FALSE) {
        easyav1->audio.frame.samples = 0;
    }

    if (easyav1->seek.mode != NOT_SEEKING) {
        if (vorbis_synthesis_trackonly(&easyav1->audio.vorbis.block, &audio_packet) ||
            vorbis_synthesis_blockin(&easyav1->audio.vorbis.dsp, &easyav1->audio.vorbis.block)) {
            LOG_AND_SET_ERROR(EASYAV1_STATUS_DECODER_ERROR, "Failed to process audio packet.");
            return EASYAV1_STATUS_ERROR;
        }

        int decoded_samples = vorbis_synthesis_pcmout(&easyav1->audio.vorbis.dsp, 0);
        vorbis_synthesis_read(&easyav1->audio.vorbis.dsp, decoded_samples);
        return EASYAV1_STATUS_OK;
    }

    if (vorbis_synthesis(&easyav1->audio.vorbis.block, &audio_packet) ||
        vorbis_synthesis_blockin(&easyav1->audio.vorbis.dsp, &easyav1->audio.vorbis.block)) {
        LOG_AND_SET_ERROR(EASYAV1_STATUS_DECODER_ERROR, "Failed to process audio packet.");
        return EASYAV1_STATUS_ERROR;
    }

    float **pcm;
    easyav1_audio_frame *frame = &easyav1->audio.frame;
    float *buffer = easyav1->audio.buffer;

    int decoded_samples = vorbis_synthesis_pcmout(&easyav1->audio.vorbis.dsp, &pcm);
    unsigned int pcm_offset = prepare_audio_buffer_for_new_samples(easyav1, decoded_samples);
    
    while (decoded_samples > 0) {
        for (unsigned int sample = 0; sample < decoded_samples; sample++) {
            for (unsigned int channel = 0; channel < easyav1->audio.channels; channel++) {
                if (easyav1->settings.interlace_audio) {
                    buffer[(frame->samples + sample) * easyav1->audio.channels + channel] =
                        pcm[channel][sample + pcm_offset];
                } else {
                    buffer[channel * AUDIO_BUFFER_SIZE + frame->samples + sample] = pcm[channel][sample + pcm_offset];
                }
            }
        }

        vorbis_synthesis_read(&easyav1->audio.vorbis.dsp, decoded_samples);

        frame->samples += decoded_samples;
        if (frame->samples > AUDIO_BUFFER_SIZE) {
            frame->samples = AUDIO_BUFFER_SIZE;
        }

        decoded_samples = vorbis_synthesis_pcmout(&easyav1->audio.vorbis.dsp, &pcm);
        pcm_offset = prepare_audio_buffer_for_new_samples(easyav1, decoded_samples);
    }

    if (frame->samples > 0) {
        easyav1->audio.has_samples_in_buffer = EASYAV1_TRUE;
    }

    return EASYAV1_STATUS_OK;
}

static unsigned int prepare_audio_buffer_for_new_samples(easyav1_t *easyav1, int decoded_samples)
{
    // If we have too many samples, we need to discard some
    if (decoded_samples > AUDIO_BUFFER_SIZE) {
        easyav1->audio.frame.samples = 0;
        return decoded_samples - AUDIO_BUFFER_SIZE;
    }

    if (decoded_samples + easyav1->audio.frame.samples <= AUDIO_BUFFER_SIZE) {
        return 0;
    }

    // If the audio buffer is too small for the incoming samples, we need to move the stored samples to fit the new ones
    unsigned int samples_to_move = decoded_samples + easyav1->audio.frame.samples - AUDIO_BUFFER_SIZE;

    log(EASYAV1_LOG_LEVEL_INFO, "Audio buffer full, moving %u samples to fit new samples.", samples_to_move);

    if (easyav1->settings.interlace_audio) {
        memmove(easyav1->audio.buffer, easyav1->audio.buffer + samples_to_move * easyav1->audio.channels,
            (AUDIO_BUFFER_SIZE - samples_to_move) * easyav1->audio.channels * sizeof(float));
    } else {
        for (unsigned int channel = 0; channel < easyav1->audio.channels; channel++) {
            memmove(easyav1->audio.buffer + channel * AUDIO_BUFFER_SIZE,
                easyav1->audio.buffer + channel * AUDIO_BUFFER_SIZE + samples_to_move,
                (AUDIO_BUFFER_SIZE - samples_to_move) * sizeof(float));
        }
    }

    easyav1->audio.frame.samples = AUDIO_BUFFER_SIZE - decoded_samples;

    return 0;
}

static easyav1_status send_packet_data_to_decoder(easyav1_t *easyav1, easyav1_packet *packet, decoder_function decode)
{
    unsigned int chunks;

    if (nestegg_packet_count(packet->packet, &chunks)) {
        LOG_AND_SET_ERROR(EASYAV1_STATUS_DECODER_ERROR, "Failed to get packet count");
        return EASYAV1_STATUS_ERROR;
    }

    for (unsigned int chunk = 0; chunk < chunks; chunk++) {
        unsigned char *data;
        size_t size;

        if (nestegg_packet_data(packet->packet, chunk, &data, &size)) {
            LOG_AND_SET_ERROR(EASYAV1_STATUS_DECODER_ERROR, "Failed to get data from packet");
            return EASYAV1_STATUS_ERROR;
        }

        if (decode(easyav1, packet, data, size) == EASYAV1_STATUS_ERROR) {
            return EASYAV1_STATUS_ERROR;
        }
    }

    return EASYAV1_STATUS_OK;
}

static easyav1_status decode_packet(easyav1_t *easyav1, easyav1_packet *packet)
{
    if (packet->type == PACKET_TYPE_AUDIO) {
        return send_packet_data_to_decoder(easyav1, packet, decode_audio);
    }

    // Decoding video: use multithreaded decoder
    if (easyav1->seek.mode == NOT_SEEKING || easyav1->seek.mode == SEEKING_FOR_TIMESTAMP) {

        pthread_mutex_lock(&easyav1->video.decoder_thread.mutexes.io);

        while (packet->decoded == EASYAV1_FALSE) {

            // On some occasions, some video packets may have been fetched after the last signal to the decoder thread.
            // In this case, we need to signal the decoder thread again to process the new packets.
            if (get_video_packet_to_decode(easyav1, &easyav1->packets.video_queue)) {
                pthread_cond_signal(&easyav1->video.decoder_thread.conditions.has_packets);
            }

            log(EASYAV1_LOG_LEVEL_INFO, "Waiting for video frame to be decoded.");
            pthread_cond_wait(&easyav1->video.decoder_thread.conditions.has_frames_to_display,
                &easyav1->video.decoder_thread.mutexes.io);

            if (EASYAV1_STATUS_IS_ERROR(easyav1->status)) {
                pthread_mutex_unlock(&easyav1->video.decoder_thread.mutexes.io);
                return EASYAV1_STATUS_ERROR;
            }
        }

        pthread_mutex_unlock(&easyav1->video.decoder_thread.mutexes.io);

        if (EASYAV1_STATUS_IS_ERROR(easyav1->status)) {
            return EASYAV1_STATUS_ERROR;
        }

        if (easyav1->settings.skip_unprocessed_frames == EASYAV1_FALSE) {
            callback_video(easyav1);
        }

        return EASYAV1_STATUS_OK;
    }

    // Seeking, do not decode video frames
    if (easyav1->seek.mode == SEEKING_FOR_SQHDR) {
        easyav1_status status = send_packet_data_to_decoder(easyav1, packet, seek_sequence_header);

        if (EASYAV1_STATUS_IS_ERROR(easyav1->status)) {
            return EASYAV1_STATUS_ERROR;
        }
    }

    if (easyav1->seek.mode == SEEKING_FOR_KEYFRAME && packet->is_keyframe == EASYAV1_TRUE) {
        easyav1->seek.mode = SEEKING_FOUND_KEYFRAME;
    }

    return EASYAV1_STATUS_OK;
}

easyav1_status easyav1_decode_next(easyav1_t *easyav1)
{
    if (!easyav1) {
        log(EASYAV1_LOG_LEVEL_INFO, "Handle is NULL");
        return EASYAV1_STATUS_ERROR;
    }

    if (EASYAV1_STATUS_IS_ERROR(easyav1->status)) {
        return EASYAV1_STATUS_ERROR;
    }

    if (easyav1->seek.mode != NOT_SEEKING) {
        log(EASYAV1_LOG_LEVEL_INFO, "Cannot decode while seeking.");
        return EASYAV1_STATUS_OK;
    }

    if (easyav1->playback.active == EASYAV1_TRUE) {
        log(EASYAV1_LOG_LEVEL_INFO, "Cannot manually decode when using playback functions.");
        return EASYAV1_STATUS_OK;
    }

    easyav1_packet *packet = get_next_packet(easyav1);

    if (easyav1->status == EASYAV1_STATUS_FINISHED) {
        return EASYAV1_STATUS_FINISHED;
    }

    if (!packet) {
        return EASYAV1_STATUS_ERROR;
    }

    pthread_mutex_lock(&easyav1->video.decoder_thread.mutexes.info);

    easyav1->position = packet->timestamp;
    easyav1_packet_type packet_type = packet->type;

    pthread_mutex_unlock(&easyav1->video.decoder_thread.mutexes.info);

    easyav1_status status = decode_packet(easyav1, packet);

    if (packet_type == PACKET_TYPE_VIDEO) {
        pthread_mutex_lock(&easyav1->video.decoder_thread.mutexes.io);
    }

    release_packet_from_queue(easyav1, packet);

    if (packet_type == PACKET_TYPE_VIDEO) {
        pthread_mutex_unlock(&easyav1->video.decoder_thread.mutexes.io);
    }

    if (status == EASYAV1_STATUS_OK) {
        callback_video(easyav1);
        callback_audio(easyav1);
    }

    return status;
}

static easyav1_status do_decode_until(easyav1_t *easyav1, easyav1_timestamp timestamp)
{
    if (EASYAV1_STATUS_IS_ERROR(easyav1->status)) {
        return EASYAV1_STATUS_ERROR;
    }

    if (easyav1->status == EASYAV1_STATUS_FINISHED) {
        return EASYAV1_STATUS_FINISHED;
    }

    if (timestamp <= easyav1->position) {
        return EASYAV1_STATUS_OK;
    }

    if (easyav1->seek.mode != NOT_SEEKING) {
        log(EASYAV1_LOG_LEVEL_INFO, "Cannot decode while seeking.");
        return EASYAV1_STATUS_OK;
    }

    // Skip to timestamp if too far behind and at different cue points
    if (easyav1->settings.skip_unprocessed_frames == EASYAV1_TRUE &&
        timestamp - easyav1->position > DECODE_UNTIL_SKIP_MS &&
        get_closest_cue_point(easyav1, easyav1->position) < get_closest_cue_point(easyav1, timestamp)) {
        log(EASYAV1_LOG_LEVEL_INFO, "Decoder too far behind at %llu, skipping to requested timestamp %llu.",
            easyav1->position, timestamp);

        easyav1_bool use_fast_seeking = easyav1->settings.use_fast_seeking;
        easyav1->settings.use_fast_seeking = EASYAV1_TRUE;

        // Use fast seeking to prevent constantly seeking to the next seek point
        easyav1_status status = do_seek_to_timestamp(easyav1, timestamp);

        easyav1->settings.use_fast_seeking = use_fast_seeking;

        if (EASYAV1_STATUS_IS_ERROR(status)) {
            return EASYAV1_STATUS_ERROR;
        }
    }

    easyav1_status status = easyav1->status;

    while (status == EASYAV1_STATUS_OK) {
        easyav1_packet *packet = get_next_packet(easyav1);

        if (easyav1->status == EASYAV1_STATUS_FINISHED) {
            status = EASYAV1_STATUS_FINISHED;
            break;
        }

        if (!packet) {
            return EASYAV1_STATUS_ERROR;
        }

        if (easyav1->playback.do_pause == EASYAV1_TRUE || easyav1->playback.seek.requested == EASYAV1_TRUE) {
            return status;
        }

        if (packet->timestamp >= timestamp) {
            break;
        }

        pthread_mutex_lock(&easyav1->video.decoder_thread.mutexes.info);

        easyav1->position = packet->timestamp;
        easyav1_packet_type packet_type = packet->type;

        pthread_mutex_unlock(&easyav1->video.decoder_thread.mutexes.info);

        status = decode_packet(easyav1, packet);


        if (packet_type == PACKET_TYPE_VIDEO) {
            pthread_mutex_lock(&easyav1->video.decoder_thread.mutexes.io);
        }
    
        release_packet_from_queue(easyav1, packet);

        if (packet_type == PACKET_TYPE_VIDEO) {
            pthread_mutex_unlock(&easyav1->video.decoder_thread.mutexes.io);
        }
    }

    if (status == EASYAV1_STATUS_OK) {
        pthread_mutex_lock(&easyav1->video.decoder_thread.mutexes.info);

        easyav1->position = timestamp;

        pthread_mutex_unlock(&easyav1->video.decoder_thread.mutexes.info);
    }

    if (status != EASYAV1_STATUS_ERROR) {
        callback_video(easyav1);
        callback_audio(easyav1);
    }

    return status;
}

easyav1_status easyav1_decode_until(easyav1_t *easyav1, easyav1_timestamp timestamp)
{
    if (!easyav1) {
        log(EASYAV1_LOG_LEVEL_INFO, "Handle is NULL");
        return EASYAV1_STATUS_ERROR;
    }

    if (easyav1->playback.active == EASYAV1_TRUE) {
        log(EASYAV1_LOG_LEVEL_INFO, "Cannot manually decode when using playback functions.");
        return EASYAV1_STATUS_OK;
    }

    return do_decode_until(easyav1, timestamp);
}

easyav1_status easyav1_decode_for(easyav1_t *easyav1, easyav1_timestamp time)
{
    return easyav1_decode_until(easyav1, easyav1->position + time);
}


/**
 * High-level decoding functions
 */

 /**
  * @brief The thread function that handles video playback.
  * 
  * @param arg The easyav1 instance.
  * @return Always returns `NULL`.
  */
static void *easyav1_playback_thread(void *arg)
{
    easyav1_t *easyav1 = (easyav1_t *) arg;

    easyav1_timestamp last_timestamp = easyav1_get_ticks();
    easyav1_timestamp current_timestamp = last_timestamp;

    pthread_mutex_lock(&easyav1->playback.mutex);

    while (easyav1->playback.active == EASYAV1_TRUE && easyav1->playback.do_pause == EASYAV1_FALSE &&
        do_decode_until(easyav1, easyav1->position + (current_timestamp - last_timestamp)) != EASYAV1_STATUS_ERROR) {

        pthread_mutex_unlock(&easyav1->playback.mutex);

        // Prevent busy waiting
        if (current_timestamp == last_timestamp) {
            easyav1_sleep(1);
        }

        // Update timestamp
        last_timestamp = current_timestamp;
        current_timestamp = easyav1_get_ticks();

        pthread_mutex_lock(&easyav1->video.decoder_thread.mutexes.info);

        easyav1_bool should_seek = easyav1->playback.seek.requested;
        easyav1_timestamp seek_timestamp = easyav1->playback.seek.timestamp;

        easyav1->playback.seek.requested = EASYAV1_FALSE;
        easyav1->playback.seek.timestamp = 0;

        pthread_mutex_unlock(&easyav1->video.decoder_thread.mutexes.info);

        if (should_seek == EASYAV1_TRUE) {
            if (do_seek_to_timestamp(easyav1, seek_timestamp) == EASYAV1_STATUS_ERROR) {
                break;
            }

            current_timestamp = easyav1_get_ticks();
            last_timestamp = current_timestamp;
        }

        pthread_mutex_lock(&easyav1->playback.mutex);
    }

    pthread_mutex_unlock(&easyav1->playback.mutex);

    return NULL;
}

easyav1_status easyav1_play(easyav1_t *easyav1)
{
    if (!easyav1) {
        log(EASYAV1_LOG_LEVEL_INFO, "Handle is NULL");
        return EASYAV1_STATUS_ERROR;
    }

    if (EASYAV1_STATUS_IS_ERROR(easyav1->status)) {
        return EASYAV1_STATUS_ERROR;
    }

    if (easyav1->status == EASYAV1_STATUS_FINISHED) {
        return EASYAV1_STATUS_FINISHED;
    }

    if (easyav1->playback.active == EASYAV1_TRUE) {
        log(EASYAV1_LOG_LEVEL_INFO, "Playback is already active.");
        return EASYAV1_STATUS_OK;
    }

    if (pthread_mutex_init(&easyav1->playback.mutex, NULL)) {
        log(EASYAV1_LOG_LEVEL_ERROR, "Failed to initialize playback mutex.");
        return EASYAV1_STATUS_ERROR;
    }

    easyav1->playback.do_pause = EASYAV1_FALSE;
    easyav1->playback.active = EASYAV1_TRUE;

    easyav1->playback.seek.requested = EASYAV1_FALSE;
    easyav1->playback.seek.timestamp = 0;

    if (pthread_create(&easyav1->playback.thread, NULL, easyav1_playback_thread, easyav1)) {
        log(EASYAV1_LOG_LEVEL_ERROR, "Failed to create playback thread.");
        easyav1->playback.active = EASYAV1_FALSE;
        return EASYAV1_STATUS_ERROR;
    }

    return EASYAV1_STATUS_OK;
}

void easyav1_stop(easyav1_t *easyav1)
{
    if (!easyav1) {
        log(EASYAV1_LOG_LEVEL_INFO, "Handle is NULL");
        return;
    }

    if (EASYAV1_STATUS_IS_ERROR(easyav1->status)) {
        return;
    }

    if (easyav1->playback.active == EASYAV1_FALSE) {
        log(EASYAV1_LOG_LEVEL_INFO, "Playback is not active.");
        return;
    }

    easyav1->playback.do_pause = EASYAV1_TRUE;

    pthread_join(easyav1->playback.thread, NULL);

    easyav1->playback.active = EASYAV1_FALSE;
    easyav1->playback.do_pause = EASYAV1_FALSE;

    easyav1->playback.seek.requested = EASYAV1_FALSE;
    easyav1->playback.seek.timestamp = 0;

    pthread_mutex_destroy(&easyav1->playback.mutex);
}


/**
 * Seeking functions
 */

static easyav1_timestamp get_closest_cue_point(const easyav1_t *easyav1, easyav1_timestamp timestamp)
{
    if (!nestegg_has_cues(easyav1->webm.context)) {
        return 0;
    }

    unsigned int cluster = 0;
    int64_t start_pos;
    int64_t end_pos;
    easyav1_timestamp cue_timestamp;
    easyav1_timestamp closest = 0;

    do {
        if (nestegg_get_cue_point(easyav1->webm.context, cluster, -1, &start_pos, &end_pos, &cue_timestamp)) {
            log(EASYAV1_LOG_LEVEL_WARNING, "Failed to get cue point %u.", cluster);
            return closest;
        }

        cue_timestamp = internal_timestamp_to_ms(easyav1, cue_timestamp);

        if (cue_timestamp >= timestamp) {
            break;
        }

        closest = cue_timestamp;

        cluster++;
    } while (end_pos != -1);

    return closest;
}

static easyav1_status do_seek_to_timestamp(easyav1_t *easyav1, easyav1_timestamp timestamp)
{
    pthread_mutex_lock(&easyav1->video.decoder_thread.mutexes.info);

    easyav1_status status = easyav1->status;
    easyav1_timestamp position = easyav1->position;
    easyav1_timestamp duration = easyav1_get_duration(easyav1);

    pthread_mutex_unlock(&easyav1->video.decoder_thread.mutexes.info);

    if (EASYAV1_STATUS_IS_ERROR(status)) {
        return EASYAV1_STATUS_ERROR;
    }

    if (timestamp == position) {
        return EASYAV1_STATUS_OK;
    }

    if (duration && timestamp >= duration) {
        log(EASYAV1_LOG_LEVEL_INFO, "Requested timestamp is beyond the end of the stream.");
        timestamp = duration;

        if (status == EASYAV1_STATUS_FINISHED) {
            return EASYAV1_STATUS_OK;
        }
    }

    easyav1_timestamp original_timestamp = easyav1->position;
    easyav1_timestamp corrected_timestamp = get_closest_cue_point(easyav1, timestamp);

    unsigned int track = 0;

    if (easyav1->video.active == EASYAV1_TRUE) {
        track = easyav1->video.track;
    } else {
        track = easyav1->audio.track;
    }

    /**
     * Two-pass seek when there's video:
     *
     * - The first pass finds the closest keyframe to the requested timestamp without decoding anything
     * - The second pass skips all video decoding until that keyframe
     */
    easyav1_timestamp last_keyframe_timestamp = 0;

    easyav1_bool audio_is_active = easyav1->audio.active;

    pause_video_decoder_thread(easyav1);

    pthread_mutex_lock(&easyav1->video.decoder_thread.mutexes.info);

    easyav1->seek.mode = STARTING_SEEKING;
    easyav1->status = EASYAV1_STATUS_OK;

    pthread_mutex_unlock(&easyav1->video.decoder_thread.mutexes.info);

    for (int pass = 0; pass < 2; pass++) {

        pthread_mutex_lock(&easyav1->video.decoder_thread.mutexes.info);

        easyav1->position = corrected_timestamp;

        pthread_mutex_unlock(&easyav1->video.decoder_thread.mutexes.info);

        if (nestegg_track_seek(easyav1->webm.context, track, ms_to_internal_timestmap(easyav1, corrected_timestamp))) {
            LOG_AND_SET_ERROR(EASYAV1_STATUS_IO_ERROR, "Failed to seek to requested timestamp %u.", easyav1->position);
            resume_video_decoder_thread(easyav1);

            if (easyav1->playback.active == EASYAV1_TRUE) {
                pthread_mutex_unlock(&easyav1->playback.mutex);
            }

            return EASYAV1_STATUS_ERROR;
        }

        release_packets_from_queue(easyav1, &easyav1->packets.video_queue);
        release_packets_from_queue(easyav1, &easyav1->packets.audio_queue);

        easyav1->packets.synced = EASYAV1_FALSE;
        easyav1->packets.all_fetched = EASYAV1_FALSE;

        pthread_mutex_lock(&easyav1->video.decoder_thread.mutexes.info);

        easyav1->status = EASYAV1_STATUS_OK;

        pthread_mutex_unlock(&easyav1->video.decoder_thread.mutexes.info);

        easyav1->seek.timestamp = timestamp;

        if (easyav1->video.active == EASYAV1_TRUE) {

            pthread_mutex_lock(&easyav1->video.decoder_thread.mutexes.io);

            dequeue_all_video_frames(easyav1);

            pthread_mutex_unlock(&easyav1->video.decoder_thread.mutexes.io);

            dav1d_flush(easyav1->video.context);
            easyav1->seek.mode = SEEKING_FOR_SQHDR;
        }

        if (audio_is_active) {
            if (easyav1->seek.mode == STARTING_SEEKING) {
                easyav1->seek.mode = SEEKING_FOR_TIMESTAMP;

                // No need for a second pass for audio only
                pass = 1;
            }

            // No need to decode audio at all on first pass
            if (pass == 0) {
                easyav1->audio.active = EASYAV1_FALSE;
            } else {
                easyav1->audio.active = EASYAV1_TRUE;

                vorbis_synthesis_restart(&easyav1->audio.vorbis.dsp);
                easyav1->audio.has_samples_in_buffer = EASYAV1_FALSE;
            }
        }

        while (1) {
            easyav1_packet *packet = get_next_packet(easyav1);

            if (EASYAV1_STATUS_IS_ERROR(easyav1->status)) {
                resume_video_decoder_thread(easyav1);

                if (easyav1->playback.active == EASYAV1_TRUE) {
                    pthread_mutex_unlock(&easyav1->playback.mutex);
                }

                return EASYAV1_STATUS_ERROR;
            }

            if (packet) {

                pthread_mutex_lock(&easyav1->video.decoder_thread.mutexes.info);

                easyav1->position = packet->timestamp;

                pthread_mutex_unlock(&easyav1->video.decoder_thread.mutexes.info);

                if (pass == 1) {
                    if (packet->timestamp >= last_keyframe_timestamp) {

                        // If fast seeking is enabled, we don't decode until we find the correct timestamp, rather we
                        // resume playing immediately after we get to the last keyframe before the requested timestamp
                        if (easyav1->settings.use_fast_seeking == EASYAV1_TRUE) {
                            easyav1->seek.mode = NOT_SEEKING;
                            resume_video_decoder_thread(easyav1);
                            break;
                        } else if (easyav1->seek.mode != SEEKING_FOR_TIMESTAMP) {
                            easyav1->seek.mode = SEEKING_FOR_TIMESTAMP;
                            resume_video_decoder_thread(easyav1);
                        }
                    } else if (easyav1->seek.mode == SEEKING_FOR_TIMESTAMP) {
                        easyav1->seek.mode = SEEKING_FOUND_KEYFRAME;
                    }
                }
            }

            if (easyav1->position >= timestamp || easyav1->status == EASYAV1_STATUS_FINISHED) {

                // If we couldn't find a keyframe on the first pass, we need to seek again,
                // starting on the previous cue point
                if (pass == 0 && last_keyframe_timestamp < corrected_timestamp) {

                    // No keyframe found from the beginning of the file - this shouldn't happen, but just in case
                    if (corrected_timestamp == 0) {
                        LOG_AND_SET_ERROR(EASYAV1_STATUS_DECODER_ERROR,
                            "Unable to seek, no sequence header or keyframes found. Aborting.");
                        release_packet_from_queue(easyav1, packet);
                        resume_video_decoder_thread(easyav1);

                        if (easyav1->playback.active == EASYAV1_TRUE) {
                            pthread_mutex_unlock(&easyav1->playback.mutex);
                        }

                        return EASYAV1_STATUS_ERROR;
                    }

                    corrected_timestamp = get_closest_cue_point(easyav1, corrected_timestamp);
                    last_keyframe_timestamp = 0;
                    pass = -1;
                }

                if (easyav1->seek.mode == SEEKING_FOR_TIMESTAMP) {
                    pthread_mutex_lock(&easyav1->video.decoder_thread.mutexes.info);
                }

                easyav1->position = timestamp;

                if (easyav1->seek.mode == SEEKING_FOR_TIMESTAMP) {
                    pthread_mutex_unlock(&easyav1->video.decoder_thread.mutexes.info);
                }

                easyav1->seek.mode = STARTING_SEEKING;
                break;
            }

            seeking_mode last_seek_mode = easyav1->seek.mode;

            if (decode_packet(easyav1, packet) == EASYAV1_STATUS_ERROR) {
                if (easyav1->seek.mode != SEEKING_FOR_TIMESTAMP) {
                    resume_video_decoder_thread(easyav1);
                }

                if (easyav1->playback.active == EASYAV1_TRUE) {
                    pthread_mutex_unlock(&easyav1->playback.mutex);
                }

                log(EASYAV1_LOG_LEVEL_ERROR, "Failed to decode packet when seeking.");
                return EASYAV1_STATUS_ERROR;
            }

            if (pass == 0 && easyav1->seek.mode == SEEKING_FOUND_KEYFRAME) {
                last_keyframe_timestamp = packet->timestamp;
                easyav1->seek.mode = SEEKING_FOR_KEYFRAME;
            }

            // If we found a sequence header, we may need to decode the video frame from this packet.
            // To do that, we need to keep the packet so it's fetched again and properly decoded.
            if (pass == 0 || (pass == 1 && (last_seek_mode != SEEKING_FOR_SQHDR ||
                last_seek_mode == easyav1->seek.mode || packet->timestamp < last_keyframe_timestamp))) {

                if (easyav1->seek.mode == SEEKING_FOR_TIMESTAMP) {
                    pthread_mutex_lock(&easyav1->video.decoder_thread.mutexes.io);
                }

                release_packet_from_queue(easyav1, packet);

                if (easyav1->seek.mode == SEEKING_FOR_TIMESTAMP) {
                    pthread_mutex_unlock(&easyav1->video.decoder_thread.mutexes.io);
                }
            }
        }
    }

    easyav1->seek.timestamp = 0;
    easyav1->seek.mode = NOT_SEEKING;

    log(EASYAV1_LOG_LEVEL_INFO, "Seeked to timestamp %llu from timestamp %llu.", easyav1->position,
        original_timestamp);

    return EASYAV1_STATUS_OK;
}

static void request_seek_to_timestamp(easyav1_t *easyav1, easyav1_timestamp timestamp)
{
    pthread_mutex_lock(&easyav1->video.decoder_thread.mutexes.info);

    easyav1->playback.seek.requested = EASYAV1_TRUE;
    easyav1->playback.seek.timestamp = timestamp;

    pthread_mutex_unlock(&easyav1->video.decoder_thread.mutexes.info);
}

easyav1_status easyav1_seek_to_timestamp(easyav1_t *easyav1, easyav1_timestamp timestamp)
{
    if (!easyav1) {
        log(EASYAV1_LOG_LEVEL_INFO, "Handle is NULL");
        return EASYAV1_STATUS_ERROR;
    }

    if (easyav1->seek.mode != NOT_SEEKING) {
        log(EASYAV1_LOG_LEVEL_INFO, "Trying to seek while already seeking.");
        return EASYAV1_STATUS_OK;
    }

    if (easyav1->playback.active == EASYAV1_TRUE) {
        request_seek_to_timestamp(easyav1, timestamp);
        return EASYAV1_STATUS_OK;
    }

    return do_seek_to_timestamp(easyav1, timestamp);
}

easyav1_status easyav1_seek_forward(easyav1_t *easyav1, easyav1_timestamp time)
{
    if (!easyav1) {
        log(EASYAV1_LOG_LEVEL_INFO, "Handle is NULL");
        return EASYAV1_STATUS_ERROR;
    }

    return easyav1_seek_to_timestamp(easyav1, easyav1->position + time);
}

easyav1_status easyav1_seek_backward(easyav1_t *easyav1, easyav1_timestamp time)
{
    if (!easyav1) {
        log(EASYAV1_LOG_LEVEL_INFO, "Handle is NULL");
        return EASYAV1_STATUS_ERROR;
    }

    if (time > easyav1->position) {
        time = easyav1->position;
    }

    return easyav1_seek_to_timestamp(easyav1, easyav1->position - time);
}


/**
 * Output functions
 */

easyav1_bool easyav1_has_video_frame(easyav1_t *easyav1)
{
    if (!easyav1) {
        log(EASYAV1_LOG_LEVEL_WARNING, "Handle is NULL");
        return EASYAV1_FALSE;
    }

    if (easyav1->seek.mode != NOT_SEEKING) {
        return EASYAV1_FALSE;
    }

    pthread_mutex_lock(&easyav1->video.decoder_thread.mutexes.io);

    Dav1dPicture *pic = get_oldest_video_frame_from_queue(easyav1);

    pthread_mutex_unlock(&easyav1->video.decoder_thread.mutexes.io);

    return pic == NULL || pic->m.timestamp > easyav1->position ? EASYAV1_FALSE : EASYAV1_TRUE;
}

static easyav1_bool update_frame_picture_type(easyav1_t *easyav1, easyav1_video_frame *frame, Dav1dSequenceHeader *sqhdr)
{
    if (sqhdr == NULL || easyav1->video.sqhdr == sqhdr) {
        return easyav1->video.sqhdr == NULL ? EASYAV1_FALSE : EASYAV1_TRUE;
    }

    easyav1->video.sqhdr = sqhdr;

    switch (sqhdr->layout) {
        case DAV1D_PIXEL_LAYOUT_I400:
            frame->pixel_layout = EASYAV1_PIXEL_LAYOUT_YUV400;
            break;
        case DAV1D_PIXEL_LAYOUT_I420:
            frame->pixel_layout = EASYAV1_PIXEL_LAYOUT_YUV420;
            break;
        case DAV1D_PIXEL_LAYOUT_I422:
            frame->pixel_layout = EASYAV1_PIXEL_LAYOUT_YUV422;
            break;
        case DAV1D_PIXEL_LAYOUT_I444:
            frame->pixel_layout = EASYAV1_PIXEL_LAYOUT_YUV444;
            break;
        default:
            frame->pixel_layout = EASYAV1_PIXEL_LAYOUT_UNKNOWN;
            log(EASYAV1_LOG_LEVEL_WARNING, "Unsupported pixel layout.");
            return EASYAV1_FALSE;
    }

    switch (sqhdr->hbd) {
        case 0:
            frame->bits_per_color = EASYAV1_BITS_PER_COLOR_8;
            break;
        case 1:
            frame->bits_per_color = EASYAV1_BITS_PER_COLOR_10;
            break;
        case 2:
            frame->bits_per_color = EASYAV1_BITS_PER_COLOR_12;
            break;
        default:
            frame->bits_per_color = EASYAV1_BITS_PER_COLOR_UNKNOWN;
            log(EASYAV1_LOG_LEVEL_WARNING, "Unsupported bit depth.");
            return EASYAV1_FALSE;
    }

    switch (sqhdr->color_range) {
        case 0:
            frame->color_space = EASYAV1_COLOR_SPACE_LIMITED;
            break;
        case 1:
            frame->color_space = EASYAV1_COLOR_SPACE_FULL;
            break;
        default:
            frame->color_space = EASYAV1_COLOR_SPACE_UNKNOWN;
            log(EASYAV1_LOG_LEVEL_WARNING, "Unsupported color space.");
            return EASYAV1_FALSE;
    }

    switch (sqhdr->pri) {
        case DAV1D_COLOR_PRI_BT709:
            frame->color_primaries = EASYAV1_COLOR_PRIMARIES_BT709;
            break;
        case DAV1D_COLOR_PRI_UNKNOWN:
            frame->color_primaries = EASYAV1_COLOR_PRIMARIES_UNKNOWN;
            break;
        case DAV1D_COLOR_PRI_BT470M:
            frame->color_primaries = EASYAV1_COLOR_PRIMARIES_BT470M;
            break;
        case DAV1D_COLOR_PRI_BT470BG:
            frame->color_primaries = EASYAV1_COLOR_PRIMARIES_BT470BG;
            break;
        case DAV1D_COLOR_PRI_BT601:
            frame->color_primaries = EASYAV1_COLOR_PRIMARIES_BT601;
            break;
        case DAV1D_COLOR_PRI_SMPTE240:
            frame->color_primaries = EASYAV1_COLOR_PRIMARIES_SMPTE240;
            break;
        case DAV1D_COLOR_PRI_FILM:
            frame->color_primaries = EASYAV1_COLOR_PRIMARIES_FILM;
            break;
        case DAV1D_COLOR_PRI_BT2020:
            frame->color_primaries = EASYAV1_COLOR_PRIMARIES_BT2020;
            break;
        case DAV1D_COLOR_PRI_XYZ:
            frame->color_primaries = EASYAV1_COLOR_PRIMARIES_XYZ;
            break;
        case DAV1D_COLOR_PRI_SMPTE431:
            frame->color_primaries = EASYAV1_COLOR_PRIMARIES_SMPTE431;
            break;
        case DAV1D_COLOR_PRI_SMPTE432:
            frame->color_primaries = EASYAV1_COLOR_PRIMARIES_SMPTE432;
            break;
        case DAV1D_COLOR_PRI_EBU3213:
            frame->color_primaries = EASYAV1_COLOR_PRIMARIES_EBU3213;
            break;
        default:
            frame->color_primaries = EASYAV1_COLOR_PRIMARIES_UNSPECIFIED;
            log(EASYAV1_LOG_LEVEL_WARNING, "Unsupported color primaries.");
            return EASYAV1_FALSE;
    }

    switch (sqhdr->trc) {
        case DAV1D_TRC_BT709:
            frame->transfer_characteristics = EASYAV1_TRANSFER_CHARACTERISTICS_BT709;
            break;
        case DAV1D_TRC_UNKNOWN:
            frame->transfer_characteristics = EASYAV1_TRANSFER_CHARACTERISTICS_UNKNOWN;
            break;
        case DAV1D_TRC_BT470M:
            frame->transfer_characteristics = EASYAV1_TRANSFER_CHARACTERISTICS_BT470M;
            break;
        case DAV1D_TRC_BT470BG:
            frame->transfer_characteristics = EASYAV1_TRANSFER_CHARACTERISTICS_BT470BG;
            break;
        case DAV1D_TRC_BT601:
            frame->transfer_characteristics = EASYAV1_TRANSFER_CHARACTERISTICS_BT601;
            break;
        case DAV1D_TRC_SMPTE240:
            frame->transfer_characteristics = EASYAV1_TRANSFER_CHARACTERISTICS_SMPTE240;
            break;
        case DAV1D_TRC_LINEAR:
            frame->transfer_characteristics = EASYAV1_TRANSFER_CHARACTERISTICS_LINEAR;
            break;
        case DAV1D_TRC_LOG100:
            frame->transfer_characteristics = EASYAV1_TRANSFER_CHARACTERISTICS_LOG_100;
            break;
        case DAV1D_TRC_LOG100_SQRT10:
            frame->transfer_characteristics = EASYAV1_TRANSFER_CHARACTERISTICS_LOG_100_SQRT;
            break;
        case DAV1D_TRC_IEC61966:
            frame->transfer_characteristics = EASYAV1_TRANSFER_CHARACTERISTICS_IEC61966;
            break;
        case DAV1D_TRC_BT1361:
            frame->transfer_characteristics = EASYAV1_TRANSFER_CHARACTERISTICS_BT1361;
            break;
        case DAV1D_TRC_SRGB:
            frame->transfer_characteristics = EASYAV1_TRANSFER_CHARACTERISTICS_SRGB;
            break;
        case DAV1D_TRC_BT2020_10BIT:
            frame->transfer_characteristics = EASYAV1_TRANSFER_CHARACTERISTICS_BT2020_10;
            break;
        case DAV1D_TRC_BT2020_12BIT:
            frame->transfer_characteristics = EASYAV1_TRANSFER_CHARACTERISTICS_BT2020_12;
            break;
        case DAV1D_TRC_SMPTE2084:
            frame->transfer_characteristics = EASYAV1_TRANSFER_CHARACTERISTICS_SMPTE2084;
            break;
        case DAV1D_TRC_SMPTE428:
            frame->transfer_characteristics = EASYAV1_TRANSFER_CHARACTERISTICS_SMPTE428;
            break;
        case DAV1D_TRC_HLG:
            frame->transfer_characteristics = EASYAV1_TRANSFER_CHARACTERISTICS_HLG;
            break;
        default:
            frame->transfer_characteristics = EASYAV1_TRANSFER_CHARACTERISTICS_UNSPECIFIED;
            log(EASYAV1_LOG_LEVEL_WARNING, "Unsupported transfer characteristics.");
            return EASYAV1_FALSE;
    }

    switch (sqhdr->mtrx) {
        case DAV1D_MC_IDENTITY:
            frame->matrix_coefficients = EASYAV1_MATRIX_COEFFICIENTS_IDENTITY;
            break;
        case DAV1D_MC_BT709:
            frame->matrix_coefficients = EASYAV1_MATRIX_COEFFICIENTS_BT709;
            break;
        case DAV1D_MC_UNKNOWN:
            frame->matrix_coefficients = EASYAV1_MATRIX_COEFFICIENTS_UNKONWN;
            break;
        case DAV1D_MC_FCC:
            frame->matrix_coefficients = EASYAV1_MATRIX_COEFFICIENTS_FCC;
            break;
        case DAV1D_MC_BT470BG:
            frame->matrix_coefficients = EASYAV1_MATRIX_COEFFICIENTS_BT470BG;
            break;
        case DAV1D_MC_BT601:
            frame->matrix_coefficients = EASYAV1_MATRIX_COEFFICIENTS_BT601;
            break;
        case DAV1D_MC_SMPTE240:
            frame->matrix_coefficients = EASYAV1_MATRIX_COEFFICIENTS_SMPTE240;
            break;
        case DAV1D_MC_SMPTE_YCGCO:
            frame->matrix_coefficients = EASYAV1_MATRIX_COEFFICIENTS_SMPTE_YCGCO;
            break;
        case DAV1D_MC_BT2020_NCL:
            frame->matrix_coefficients = EASYAV1_MATRIX_COEFFICIENTS_BT2020_NCL;
            break;
        case DAV1D_MC_BT2020_CL:
            frame->matrix_coefficients = EASYAV1_MATRIX_COEFFICIENTS_BT2020_CL;
            break;
        case DAV1D_MC_SMPTE2085:
            frame->matrix_coefficients = EASYAV1_MATRIX_COEFFICIENTS_SMPTE2085;
            break;
        case DAV1D_MC_CHROMAT_NCL:
            frame->matrix_coefficients = EASYAV1_MATRIX_COEFFICIENTS_CHROMATICITY_NCL;
            break;
        case DAV1D_MC_CHROMAT_CL:
            frame->matrix_coefficients = EASYAV1_MATRIX_COEFFICIENTS_CHROMATICITY_CL;
            break;
        case DAV1D_MC_ICTCP:
            frame->matrix_coefficients = EASYAV1_MATRIX_COEFFICIENTS_ICTCP;
            break;
        default:
            frame->matrix_coefficients = EASYAV1_MATRIX_COEFFICIENTS_UNSPECIFIED;
            log(EASYAV1_LOG_LEVEL_WARNING, "Unsupported matrix coefficients.");
            return EASYAV1_FALSE;
    }

    switch (sqhdr->chr) {
        case EASYAV1_CHROMA_SAMPLE_POSITION_VERTICAL:
            frame->chroma_sample_position = EASYAV1_CHROMA_SAMPLE_POSITION_VERTICAL;
                break;
        case EASYAV1_CHROMA_SAMPLE_POSITION_COLOCATED:
            frame->chroma_sample_position = EASYAV1_CHROMA_SAMPLE_POSITION_COLOCATED;
                break;
        default:
            frame->chroma_sample_position = EASYAV1_CHROMA_SAMPLE_POSITION_UNKNOWN;
            log(EASYAV1_LOG_LEVEL_WARNING, "Unsupported chroma sample position.");
            return EASYAV1_FALSE;
    }

    return EASYAV1_TRUE;
}

const easyav1_video_frame *easyav1_get_video_frame(easyav1_t *easyav1)
{
    if (!easyav1) {
        log(EASYAV1_LOG_LEVEL_WARNING, "Handle is NULL");
        return NULL;
    }

    if (easyav1->seek.mode != NOT_SEEKING) {
        return NULL;
    }

    // Remove the old picture being displayed if it exists
    if (easyav1->video.picture.frame_hdr) {
        dav1d_picture_unref(&easyav1->video.picture);
    }

    pthread_mutex_lock(&easyav1->video.decoder_thread.mutexes.info);

    easyav1_timestamp timestamp = easyav1->position;

    pthread_mutex_unlock(&easyav1->video.decoder_thread.mutexes.info);

    pthread_mutex_lock(&easyav1->video.decoder_thread.mutexes.io);

    Dav1dPicture *pic = get_oldest_video_frame_from_queue(easyav1);

    // No pending new image to display - don't return anything
    if (!pic || pic->m.timestamp > timestamp) {
        pthread_mutex_unlock(&easyav1->video.decoder_thread.mutexes.io);
        return NULL;
    }

    // Copy the image to the output frame
    memcpy(&easyav1->video.picture, pic, sizeof(Dav1dPicture));

    // Remove the reference to the image from the frame queue and free the slot
    memset(pic, 0, sizeof(Dav1dPicture));
    dequeue_video_frame(easyav1);

    pthread_mutex_unlock(&easyav1->video.decoder_thread.mutexes.io);

    pic = &easyav1->video.picture;
    easyav1_video_frame *frame = &easyav1->video.frame;

    if (update_frame_picture_type(easyav1, frame, pic->seq_hdr) == EASYAV1_FALSE) {
        return NULL;
    }

    frame->data[0] = pic->data[0];
    frame->data[1] = pic->data[1];
    frame->data[2] = pic->data[2];

    frame->stride[0] = pic->stride[0];
    frame->stride[1] = pic->stride[1];
    frame->stride[2] = pic->stride[1];

    frame->width = (unsigned int) pic->p.w;
    frame->height = (unsigned int) pic->p.h;

    frame->timestamp = (uint64_t) pic->m.timestamp;

    return frame;
}

uint64_t easyav1_get_total_video_frames_processed(easyav1_t *easyav1)
{
    if (!easyav1) {
        log(EASYAV1_LOG_LEVEL_WARNING, "Handle is NULL");
        return 0;
    }

    pthread_mutex_lock(&easyav1->video.decoder_thread.mutexes.decoder);

    uint64_t result = easyav1->video.processed_frames;

    pthread_mutex_unlock(&easyav1->video.decoder_thread.mutexes.decoder);

    return result;
}

easyav1_bool easyav1_is_audio_buffer_filled(const easyav1_t *easyav1)
{
    if (!easyav1) {
        log(EASYAV1_LOG_LEVEL_WARNING, "Handle is NULL");
        return EASYAV1_FALSE;
    }

    if (easyav1->seek.mode != NOT_SEEKING) {
        return EASYAV1_FALSE;
    }

    return easyav1->audio.has_samples_in_buffer && easyav1->audio.frame.samples == AUDIO_BUFFER_SIZE ?
        EASYAV1_TRUE : EASYAV1_FALSE;
}

const easyav1_audio_frame *easyav1_get_audio_frame(easyav1_t *easyav1)
{
    if (!easyav1) {
        log(EASYAV1_LOG_LEVEL_WARNING, "Handle is NULL");
        return NULL;
    }

    if (easyav1->seek.mode != NOT_SEEKING || !easyav1->audio.has_samples_in_buffer) {
        return NULL;
    }

    easyav1->audio.has_samples_in_buffer = EASYAV1_FALSE;

    easyav1_audio_frame *frame = &easyav1->audio.frame;

    if (frame->samples == 0) {
        return NULL;
    }

    frame->bytes = frame->samples * sizeof(float);

    if (easyav1->settings.interlace_audio) {
        frame->bytes *= easyav1->audio.channels;
    }

    return frame;
}


/**
 * Information functions
 */

easyav1_settings easyav1_default_settings(void)
{
    return DEFAULT_SETTINGS;
}

easyav1_status easyav1_get_status(easyav1_t *easyav1)
{
    if (!easyav1) {
        log(EASYAV1_LOG_LEVEL_WARNING, "Handle is NULL");
        return EASYAV1_STATUS_ERROR;
    }

    pthread_mutex_lock(&easyav1->video.decoder_thread.mutexes.info);

    easyav1_status status = easyav1->status;

    pthread_mutex_unlock(&easyav1->video.decoder_thread.mutexes.info);

    return EASYAV1_STATUS_IS_ERROR(status) ? EASYAV1_STATUS_ERROR : status;
}

easyav1_timestamp easyav1_get_current_timestamp(easyav1_t *easyav1)
{
    if (!easyav1) {
        log(EASYAV1_LOG_LEVEL_WARNING, "Handle is NULL");
        return 0;
    }

    pthread_mutex_lock(&easyav1->video.decoder_thread.mutexes.info);

    easyav1_timestamp timestamp = easyav1->position;

    pthread_mutex_unlock(&easyav1->video.decoder_thread.mutexes.info);

    return timestamp;
}

easyav1_bool easyav1_has_video_track(const easyav1_t *easyav1)
{
    if (!easyav1 || !easyav1->webm.context) {
        log(EASYAV1_LOG_LEVEL_WARNING, "Handle is NULL");
        return EASYAV1_FALSE;
    }

    return easyav1->video.active;
}

easyav1_bool easyav1_has_audio_track(const easyav1_t *easyav1)
{
    if (!easyav1 || !easyav1->webm.context) {
        log(EASYAV1_LOG_LEVEL_WARNING, "Handle is NULL");
        return EASYAV1_FALSE;
    }

    return easyav1->audio.active;
}

unsigned int easyav1_get_video_width(const easyav1_t *easyav1)
{
    if (!easyav1 || !easyav1->webm.context) {
        log(EASYAV1_LOG_LEVEL_WARNING, "Handle is NULL");
        return 0;
    }

    return easyav1->video.active == EASYAV1_TRUE ? easyav1->video.width : 0;
}

unsigned int easyav1_get_video_height(const easyav1_t *easyav1)
{
    if (!easyav1 || !easyav1->webm.context) {
        log(EASYAV1_LOG_LEVEL_WARNING, "Handle is NULL");
        return 0;
    }

    return easyav1->video.active == EASYAV1_TRUE ? easyav1->video.height : 0;
}

unsigned int easyav1_get_video_fps(const easyav1_t *easyav1)
{
    if (!easyav1 || !easyav1->webm.context) {
        log(EASYAV1_LOG_LEVEL_WARNING, "Handle is NULL");
        return 0;
    }

    return easyav1->video.active == EASYAV1_TRUE ? easyav1->video.fps : 0;
}

unsigned int easyav1_get_audio_channels(const easyav1_t *easyav1)
{
    if (!easyav1 || !easyav1->webm.context) {
        log(EASYAV1_LOG_LEVEL_WARNING, "Handle is NULL");
        return 0;
    }

    return easyav1->audio.active == EASYAV1_TRUE ? easyav1->audio.channels : 0;
}

unsigned int easyav1_get_audio_sample_rate(const easyav1_t *easyav1)
{
    if (!easyav1 || !easyav1->webm.context) {
        log(EASYAV1_LOG_LEVEL_WARNING, "Handle is NULL");
        return 0;
    }

    return easyav1->audio.active == EASYAV1_TRUE ? easyav1->audio.sample_rate : 0;
}

easyav1_timestamp easyav1_get_duration(const easyav1_t *easyav1)
{
    if (!easyav1 || !easyav1->webm.context) {
        log(EASYAV1_LOG_LEVEL_WARNING, "Handle is NULL");
        return 0;
    }

    return easyav1->duration;
}

unsigned int easyav1_get_total_video_tracks(const easyav1_t *easyav1)
{
    return easyav1 ? easyav1->webm.video_tracks : 0;
}

unsigned int easyav1_get_total_audio_tracks(const easyav1_t *easyav1)
{
    return easyav1 ? easyav1->webm.audio_tracks : 0;
}

easyav1_bool easyav1_is_finished(easyav1_t *easyav1)
{
    if (!easyav1) {
        log(EASYAV1_LOG_LEVEL_WARNING, "Handle is NULL");
        return EASYAV1_FALSE;
    }

    pthread_mutex_lock(&easyav1->video.decoder_thread.mutexes.info);

    easyav1_status status = easyav1->status;

    pthread_mutex_unlock(&easyav1->video.decoder_thread.mutexes.info);

    return status == EASYAV1_STATUS_FINISHED ? EASYAV1_TRUE : EASYAV1_FALSE;
}


/**
 * Settings functions
 */

easyav1_settings easyav1_get_current_settings(const easyav1_t *easyav1)
{
    if (!easyav1) {
        log(EASYAV1_LOG_LEVEL_WARNING, "Handle is NULL");
        return DEFAULT_SETTINGS;
    }

    return easyav1->settings;
}

static easyav1_status change_track(easyav1_t *easyav1, easyav1_packet_type type, unsigned int track_id)
{
    unsigned int current_track = 0;

    for (unsigned int track = 0; track < easyav1->webm.num_tracks; track++) {

        int codec = nestegg_track_codec_id(easyav1->webm.context, track);

        if (codec == -1) {
            LOG_AND_SET_ERROR(EASYAV1_STATUS_DECODER_ERROR, "Failed to get codec for track %u.", track);
            return EASYAV1_STATUS_ERROR;
        }

        int track_type = nestegg_track_type(easyav1->webm.context, track);

        if (track_type == -1) {
            LOG_AND_SET_ERROR(EASYAV1_STATUS_DECODER_ERROR, "Failed to get track type.");
            return EASYAV1_STATUS_ERROR;
        }

        if (track_type == NESTEGG_TRACK_VIDEO && type == PACKET_TYPE_VIDEO) {
            if (current_track != track_id) {
                current_track++;
                continue;
            }

            log(EASYAV1_LOG_LEVEL_INFO, "Found requested video track %u at webm track %u.", current_track, track);

            if (codec != NESTEGG_CODEC_AV1) {
                log(EASYAV1_LOG_LEVEL_WARNING,
                    "Unsupported video codec found. Only AV1 codec is supported. Not displaying video.");
                return EASYAV1_STATUS_OK;
            }

            return init_video(easyav1, track);
        }

        if (track_type == NESTEGG_TRACK_AUDIO && type == PACKET_TYPE_AUDIO) {
            if (current_track != track_id) {
                current_track++;
                continue;
            }

            log(EASYAV1_LOG_LEVEL_INFO, "Found requested audio track %u at webm track %u.", current_track, track);

            if (codec != NESTEGG_CODEC_VORBIS) {
                log(EASYAV1_LOG_LEVEL_WARNING,
                    "Unsupported audio codec found. Only vorbis codec is supported. Not playing audio.");
                continue;
            }

            return init_audio(easyav1, track);
        }
    }

    log(EASYAV1_LOG_LEVEL_WARNING, "Track was not found, disabling %s.", type == PACKET_TYPE_VIDEO ? "video" : "audio");
    return EASYAV1_STATUS_OK;
}

easyav1_status easyav1_update_settings(easyav1_t *easyav1, const easyav1_settings *settings)
{
    if (!easyav1) {
        log(EASYAV1_LOG_LEVEL_WARNING, "Handle is NULL");
        return EASYAV1_STATUS_ERROR;
    }

    if (!settings) {
        log(EASYAV1_LOG_LEVEL_WARNING, "Settings are NULL");
        return EASYAV1_STATUS_ERROR;
    }

    easyav1_settings old_settings = easyav1->settings;
    easyav1->settings = *settings;

    easyav1_bool must_seek = EASYAV1_FALSE;

    easyav1_status status = EASYAV1_STATUS_OK;

    if (settings->enable_audio != old_settings.enable_audio || settings->audio_track != old_settings.audio_track) {

        must_seek = EASYAV1_TRUE;

        if (old_settings.enable_audio == EASYAV1_TRUE) {
            destroy_audio(easyav1);
            easyav1->audio.active = EASYAV1_FALSE;
            easyav1->audio.has_samples_in_buffer = EASYAV1_FALSE;
        }

        if (settings->enable_audio == EASYAV1_TRUE && settings->audio_track != old_settings.audio_track) {
            status = change_track(easyav1, PACKET_TYPE_AUDIO, settings->audio_track);
        }

    } else if (settings->enable_audio == EASYAV1_TRUE && settings->interlace_audio != old_settings.interlace_audio) {

        if (old_settings.interlace_audio == EASYAV1_TRUE) {
            free(easyav1->audio.frame.pcm.deinterlaced);
            easyav1->audio.frame.pcm.deinterlaced = NULL;
        }

        free(easyav1->audio.buffer);
        easyav1->audio.buffer = NULL;
        status = prepare_audio_buffer(easyav1);

        must_seek = EASYAV1_TRUE;

    } else if (settings->enable_audio == EASYAV1_TRUE && settings->audio_offset_time != old_settings.audio_offset_time) {

        nestegg_audio_params params;

        if (nestegg_track_audio_params(easyav1->webm.context, easyav1->audio.track, &params)) {
            LOG_AND_SET_ERROR(EASYAV1_STATUS_DECODER_ERROR, "Failed to get audio track parameters.");
            return EASYAV1_STATUS_ERROR;
        }

        easyav1->packets.audio_offset = easyav1->settings.audio_offset_time +
            internal_timestamp_to_ms(easyav1, params.codec_delay);

        must_seek = EASYAV1_TRUE;

    }

    if (settings->enable_video != old_settings.enable_video ||
        settings->video_track != old_settings.video_track) {
        must_seek = EASYAV1_TRUE;

        if (old_settings.enable_video == EASYAV1_TRUE) {
            destroy_video(easyav1);
            easyav1->video.active = EASYAV1_FALSE;
        }

        if (settings->enable_video == EASYAV1_TRUE && settings->video_track != old_settings.video_track) {
            status = change_track(easyav1, PACKET_TYPE_VIDEO, settings->video_track);
        }
    }

    if (status != EASYAV1_STATUS_OK) {
        return status;
    }

    if (must_seek == EASYAV1_TRUE) {
        log(EASYAV1_LOG_LEVEL_INFO, "Settings changed, seeking to timestamp %llu.", easyav1->position);

        // Force slow seeking to keep the video at the current position
        easyav1_bool use_fast_seeking = easyav1->settings.use_fast_seeking;
        easyav1->settings.use_fast_seeking = EASYAV1_FALSE;

        // Change the timestamp to force seeking
        easyav1->position++;
        status = easyav1_seek_to_timestamp(easyav1, easyav1->position - 1);

        easyav1->settings.use_fast_seeking = use_fast_seeking;
    }

    return status;
}


/**
 * Destruction functions
 */

static void destroy_video(easyav1_t *easyav1)
{
    pause_video_decoder_thread(easyav1);

    if (easyav1->video.picture.frame_hdr) {
        dav1d_picture_unref(&easyav1->video.picture);
    }

    dequeue_all_video_frames(easyav1);

    stop_video_decoder_thread(easyav1);

    if (easyav1->video.context) {
        dav1d_close(&easyav1->video.context);
        easyav1->video.context = NULL;
    }
}

static void destroy_audio(easyav1_t *easyav1)
{
    vorbis_block_clear(&easyav1->audio.vorbis.block);
    vorbis_dsp_clear(&easyav1->audio.vorbis.dsp);
    vorbis_info_clear(&easyav1->audio.vorbis.info);
    memset(&easyav1->audio.vorbis, 0, sizeof(easyav1->audio.vorbis));

    free(easyav1->audio.buffer);
    easyav1->audio.buffer = NULL;

    if (!easyav1->settings.interlace_audio) {
        free(easyav1->audio.frame.pcm.deinterlaced);
        easyav1->audio.frame.pcm.deinterlaced = NULL;
    }
}

void easyav1_destroy(easyav1_t **handle)
{
    easyav1_t *easyav1 = NULL;

    if (!handle) {
        log(EASYAV1_LOG_LEVEL_INFO, "Handle is NULL");
        return;
    }

    easyav1 = *handle;

    if (!easyav1) {
        log(EASYAV1_LOG_LEVEL_INFO, "Handle is NULL");
        return;
    }

    destroy_video(easyav1);
    destroy_audio(easyav1);

    destroy_packet_queue(easyav1, &easyav1->packets.video_queue);
    destroy_packet_queue(easyav1, &easyav1->packets.audio_queue);

    if (easyav1->webm.context) {
        nestegg_destroy(easyav1->webm.context);
    }

    pthread_mutex_destroy(&easyav1->video.decoder_thread.mutexes.io);
    pthread_mutex_destroy(&easyav1->video.decoder_thread.mutexes.decoder);
    pthread_mutex_destroy(&easyav1->video.decoder_thread.mutexes.info);
    pthread_mutex_destroy(&easyav1->video.decoder_thread.mutexes.status);
    pthread_cond_destroy(&easyav1->video.decoder_thread.conditions.has_packets);
    pthread_cond_destroy(&easyav1->video.decoder_thread.conditions.has_frames_to_display);

    if (easyav1->stream.data) {
        switch (easyav1->stream.type) {
            case STREAM_TYPE_FILE:
                fclose(easyav1->stream.data);
                break;
            case STREAM_TYPE_MEMORY:
                free(easyav1->stream.data);
                break;
            default:
                log(EASYAV1_LOG_LEVEL_WARNING, "Unknown stream type");
                break;
        }
    }

    free(easyav1);
    *handle = NULL;
}
