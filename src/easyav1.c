#include "easyav1.h"

#define OGG_IMPL
#define VORBIS_IMPL

#include "nestegg/nestegg.h"
#include "dav1d/dav1d.h"
#include "minivorbis/minivorbis.h"
#include "tinycthread/tinycthread.h"

#include <stdlib.h>
#include <string.h>

#define AUDIO_BUFFER_SIZE 4096
#define PACKET_QUEUE_BASE_CAPACITY 16
#define VIDEO_FRAME_QUEUE_SIZE 20

#define VORBIS_HEADERS_COUNT 3

#define DECODE_UNTIL_SKIP_MS 1000

#define INVALID_TIMESTAMP ((easyav1_timestamp) -1)

#define EASYAV1_STATUS_IS_ERROR(status) ((status) <= EASYAV1_STATUS_ERROR)

typedef enum {
    STREAM_TYPE_NONE = 0,
    STREAM_TYPE_FILE,
    STREAM_TYPE_MEMORY
} stream_type;

typedef enum {
    NOT_SEEKING = 0,
    SEEKING_FOR_SQHDR = 1,
    SEEKING_FOR_KEYFRAME = 2,
    SEEKING_FOUND_KEYFRAME = 3,
    SEEKING_FOR_TIMESTAMP = 4
} seeking_mode;

typedef enum {
    PACKET_TYPE_VIDEO = 0,
    PACKET_TYPE_AUDIO = 1
} easyav1_packet_type;

typedef struct {
    easyav1_timestamp timestamp;
    nestegg_packet *packet;
    easyav1_bool is_keyframe;
    easyav1_packet_type type;
    Dav1dPicture *video_frame;
} easyav1_packet;

typedef struct {
    easyav1_packet *items;
    size_t count;
    size_t capacity;
    size_t begin;
} easyav1_packet_queue;

typedef struct {
    Dav1dPicture pic;
    easyav1_bool displayed;
} queued_video_frame_t;

struct easyav1_t {

    struct {
        nestegg *context;
        unsigned int num_tracks;
        unsigned int video_tracks;
        unsigned int audio_tracks;
    } webm;

    struct {

        Dav1dContext *context;

        easyav1_bool active;
        unsigned int track;

        unsigned int width;
        unsigned int height;

        unsigned int fps;

        uint64_t processed_frames;

        struct {
            queued_video_frame_t frames[VIDEO_FRAME_QUEUE_SIZE];

            size_t count;
            size_t begin;
        } frame_queue;

        easyav1_video_frame frame;

        struct {

            struct {
                mtx_t input;
                mtx_t decoder;
                mtx_t output;
            } mutexes;

            cnd_t has_packets;
            cnd_t has_frames;
            thrd_t decoder;
            easyav1_bool exiting;

        } decoder_thread;

    } video;

    struct {

        struct {
            vorbis_info info;
            vorbis_block block;
            vorbis_dsp_state dsp;
        } vorbis;

        easyav1_bool active;
        unsigned int track;

        unsigned int channels;
        unsigned int sample_rate;

        float *buffer;

        easyav1_bool has_samples_in_buffer;

        easyav1_audio_frame frame;

    } audio;

    struct {

        easyav1_bool synced;
        easyav1_bool all_fetched;
        easyav1_bool started_fetching;

        easyav1_packet_queue video_queue;
        easyav1_packet_queue audio_queue;

        int64_t audio_offset;

    } packets;

    struct {
        void *data;
        stream_type type;
    } stream;

    easyav1_settings settings;

    easyav1_timestamp position;
    easyav1_timestamp duration;
    easyav1_timestamp time_scale;

    seeking_mode seek;

    easyav1_status status;

};

typedef struct {
    uint8_t *data;
    int64_t offset;
    size_t size;
} easyav1_memory;

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

typedef easyav1_status (*decoder_function)(easyav1_t *, easyav1_packet *, uint8_t *, size_t);


/*
 * Utility functions
 */
static inline easyav1_timestamp internal_timestamp_to_ms(const easyav1_t *easyav1, easyav1_timestamp ns)
{
    return ns / easyav1->time_scale;
}

static inline easyav1_timestamp ms_to_internal_timestmap(const easyav1_t *easyav1, easyav1_timestamp ms)
{
    return ms * easyav1->time_scale;
}


/*
 * Logging functions
 */

#define log(type, ...) \
if ((!easyav1 && DEFAULT_SETTINGS.log_level >= type) || (easyav1 && easyav1->settings.log_level >= type)) { \
    log_internal(type, __LINE__, __func__, __VA_ARGS__); \
}

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

static void log_from_dav1d(void *userdata, const char *format, va_list args)
{
    fprintf(stderr, "(dav1d): ");
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");

    va_end(args);

    fflush(stderr);
}

#define LOG_AND_SET_ERROR(error_type, message, ...) \
    log(EASYAV1_LOG_LEVEL_ERROR, message, ##__VA_ARGS__); \
    if (easyav1) { \
        easyav1->status = error_type; \
    } \

/*
 * I/O functions
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

static int64_t memory_tell(void *userdata)
{
    easyav1_memory *mem = (easyav1_memory *) userdata;
    if (!mem || !mem->data) {
        return -1;
    }
    return mem->offset;
}


/*
 * Initialization functions
 */

static easyav1_packet *get_unencoded_video_packet(easyav1_t *easyav1, easyav1_packet_queue *queue);

static queued_video_frame_t *enqueue_video_frame(easyav1_t *easyav1);

static void free_nothing(const uint8_t *data, void *cookie)
{}

static easyav1_status decode_video(easyav1_t *easyav1, easyav1_packet *packet)
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

            Dav1dPicture pic = { 0 };

            result = dav1d_get_picture(easyav1->video.context, &pic);

            // For some reason, sometimes we need to insist on getting the picture without sending more data
            if (result == DAV1D_ERR(EAGAIN)) {
                result = dav1d_get_picture(easyav1->video.context, &pic);
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

            if (easyav1->seek == SEEKING_FOR_TIMESTAMP) {
                dav1d_picture_unref(&pic);
                static Dav1dPicture empty_pic = { 0 };
                packet->video_frame = &empty_pic;
                continue;
            }

            if (!packet->video_frame) {
                mtx_lock(&easyav1->video.decoder_thread.mutexes.output);

                packet->video_frame = &enqueue_video_frame(easyav1)->pic;

                mtx_unlock(&easyav1->video.decoder_thread.mutexes.output);
            } else {
                dav1d_picture_unref(packet->video_frame);
            }

            *packet->video_frame = pic;

            packet->video_frame->m.timestamp = packet->timestamp;

        } while (buf.sz > 0);

        dav1d_data_unref(&buf);
    }

    return EASYAV1_STATUS_OK;
}

static int video_decoder_thread(void *arg)
{
    easyav1_t *easyav1 = (easyav1_t *) arg;

    while (easyav1->video.decoder_thread.exiting == EASYAV1_FALSE && !EASYAV1_STATUS_IS_ERROR(easyav1->status)) {

        mtx_lock(&easyav1->video.decoder_thread.mutexes.input);

        easyav1_packet *packet = get_unencoded_video_packet(easyav1, &easyav1->packets.video_queue);

        while(packet == NULL) {
            cnd_wait(&easyav1->video.decoder_thread.has_packets, &easyav1->video.decoder_thread.mutexes.input);

            if (easyav1->video.decoder_thread.exiting == EASYAV1_TRUE) {
                mtx_unlock(&easyav1->video.decoder_thread.mutexes.input);
                return 0;
            }

            packet = get_unencoded_video_packet(easyav1, &easyav1->packets.video_queue);
        }

        mtx_lock(&easyav1->video.decoder_thread.mutexes.decoder);
        mtx_unlock(&easyav1->video.decoder_thread.mutexes.input);

        easyav1_status status = decode_video(easyav1, packet);

        mtx_unlock(&easyav1->video.decoder_thread.mutexes.decoder);

        cnd_signal(&easyav1->video.decoder_thread.has_frames);
    }

    return 0;
}

static easyav1_status init_decoder_thread(easyav1_t *easyav1)
{
    if (easyav1->video.decoder_thread.exiting == EASYAV1_TRUE) {
        return EASYAV1_STATUS_ERROR;
    }

    if (mtx_init(&easyav1->video.decoder_thread.mutexes.input, mtx_plain) != thrd_success ||
        mtx_init(&easyav1->video.decoder_thread.mutexes.decoder, mtx_plain) != thrd_success ||
        mtx_init(&easyav1->video.decoder_thread.mutexes.output, mtx_plain) != thrd_success) {
        LOG_AND_SET_ERROR(EASYAV1_STATUS_DECODER_ERROR, "Failed to create decoder thread mutexes.");
        return EASYAV1_STATUS_ERROR;
    }

    if (cnd_init(&easyav1->video.decoder_thread.has_packets) != thrd_success ||
        cnd_init(&easyav1->video.decoder_thread.has_frames) != thrd_success) {
        LOG_AND_SET_ERROR(EASYAV1_STATUS_DECODER_ERROR, "Failed to create decoder thread condition variable.");
        return EASYAV1_STATUS_ERROR;
    }

    if (thrd_create(&easyav1->video.decoder_thread.decoder, video_decoder_thread, easyav1) != thrd_success) {
        LOG_AND_SET_ERROR(EASYAV1_STATUS_DECODER_ERROR, "Failed to create decoder thread.");
        return EASYAV1_STATUS_ERROR;
    }

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

    if (init_decoder_thread(easyav1) == EASYAV1_STATUS_ERROR) {
        return EASYAV1_STATUS_ERROR;
    }

    log(EASYAV1_LOG_LEVEL_INFO, "Video initialized. Size: %ux%u, %u FPS.", easyav1->video.width, easyav1->video.height,
        easyav1->video.fps);

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

static easyav1_status sync_queues(easyav1_t *easyav1);

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

    if (init_webm_tracks(easyav1) == EASYAV1_STATUS_ERROR || sync_queues(easyav1) != EASYAV1_STATUS_OK) {
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


/*
 * Packet handling functions
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
    mtx_lock(&easyav1->video.decoder_thread.mutexes.decoder);

    free(queue->items);

    queue->items = new_queue;
    queue->capacity = new_capacity;
    queue->begin = 0;

    mtx_unlock(&easyav1->video.decoder_thread.mutexes.decoder);

    return EASYAV1_STATUS_OK;
}

static easyav1_packet *get_unencoded_video_packet(easyav1_t *easyav1, easyav1_packet_queue *queue)
{
    if (!queue->count) {
        return NULL;
    }

    for (size_t i = 0; i < queue->count; i++) {
        size_t index = (queue->begin + i) % queue->capacity;

        easyav1_packet *packet = &queue->items[index];

        if (packet->type == PACKET_TYPE_VIDEO && packet->packet != NULL && packet->video_frame == NULL) {
            return packet;
        }
    }

    return NULL;
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

static easyav1_packet *retrieve_packet_from_queue(easyav1_t *easyav1, easyav1_packet_queue *queue)
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

    if (type == PACKET_TYPE_VIDEO && (easyav1->seek == NOT_SEEKING || easyav1->seek == SEEKING_FOR_TIMESTAMP)) {
        mtx_lock(&easyav1->video.decoder_thread.mutexes.input);
    }

    easyav1_packet *new_packet = queue_new_packet(easyav1, type == PACKET_TYPE_VIDEO ?
        &easyav1->packets.video_queue : &easyav1->packets.audio_queue);

    if (!new_packet) {
        if (type == PACKET_TYPE_VIDEO && (easyav1->seek == NOT_SEEKING || easyav1->seek == SEEKING_FOR_TIMESTAMP)) {
            mtx_unlock(&easyav1->video.decoder_thread.mutexes.input);
        }
        nestegg_free_packet(packet);
        LOG_AND_SET_ERROR(EASYAV1_STATUS_OUT_OF_MEMORY, "Failed to allocate memory for new packet.");
        return NULL;
    }

    new_packet->packet = packet;
    new_packet->timestamp = packet_timestamp;
    new_packet->is_keyframe = has_keyframe == NESTEGG_PACKET_HAS_KEYFRAME_TRUE ? EASYAV1_TRUE : EASYAV1_FALSE;
    new_packet->type = type;

    if (type == PACKET_TYPE_VIDEO && (easyav1->seek == NOT_SEEKING || easyav1->seek == SEEKING_FOR_TIMESTAMP)) {
        mtx_unlock(&easyav1->video.decoder_thread.mutexes.input);
        cnd_signal(&easyav1->video.decoder_thread.has_packets);
    }

    return new_packet;
}

static easyav1_bool must_fetch_video(easyav1_t *easyav1)
{
    if (easyav1->video.active == EASYAV1_FALSE ||
        (easyav1->seek != NOT_SEEKING && easyav1->seek != SEEKING_FOR_TIMESTAMP)) {
        return EASYAV1_FALSE;
    }

    if (easyav1->packets.video_queue.count >= VIDEO_FRAME_QUEUE_SIZE / 2) {
        return EASYAV1_FALSE;
    }

    return EASYAV1_TRUE;
}

static easyav1_status sync_queues(easyav1_t *easyav1)
{
    if (easyav1->status == EASYAV1_STATUS_FINISHED) {
        return EASYAV1_STATUS_FINISHED;
    }

    if (easyav1->packets.synced == EASYAV1_TRUE || easyav1->packets.all_fetched == EASYAV1_TRUE) {
        return EASYAV1_STATUS_OK;
    }

    easyav1->packets.started_fetching = EASYAV1_TRUE;

    // Complex packet fetching, with time syncing between audio and video:
    // First we check which packet types are decoded earlier than the video timeline
    // As an example, let's assume audio.offset is -100ms. This means we must fetch audio packets up to 100ms later
    // than the video packets. This is because audio packets are decoded earlier than video packets.
    easyav1_packet_type early_packet_type = easyav1->audio.active == EASYAV1_TRUE && easyav1->packets.audio_offset < 0 ?
        PACKET_TYPE_AUDIO : PACKET_TYPE_VIDEO;

    easyav1_packet_queue *early_queue = early_packet_type == PACKET_TYPE_VIDEO ?
        &easyav1->packets.video_queue : &easyav1->packets.audio_queue;
    easyav1_packet_queue *late_queue = early_packet_type == PACKET_TYPE_VIDEO ?
        &easyav1->packets.audio_queue : &easyav1->packets.video_queue;

    // Then we check which packet types are decoded later than the video timeline. Following the above example,
    // `early_packet` would be an audio packet and `late_packet` would be a video packet.
    easyav1_packet *early_packet = retrieve_last_packet_from_queue(easyav1, early_queue);
    easyav1_packet *late_packet = retrieve_packet_from_queue(easyav1, late_queue);

    easyav1_timestamp start_timestamp = early_packet ? early_packet->timestamp : INVALID_TIMESTAMP;

    easyav1_timestamp abs_audio_offset = easyav1->packets.audio_offset < 0 ?
        -easyav1->packets.audio_offset : easyav1->packets.audio_offset;

    if (easyav1->audio.active == EASYAV1_FALSE) {
        abs_audio_offset = 0;
    }

    // Keeping with the example, we need to keep fetching audio packets until we reach an audio packet that is at least
    // 100ms after the video packet.
    // Alternatively, if we don't find any video packets for the next 100ms, we should stop fetching audio packets,
    // since we're sure we won't need them.
    while (easyav1->packets.all_fetched == EASYAV1_FALSE &&
        (must_fetch_video(easyav1) || !early_packet ||
        ((!late_packet || late_packet->timestamp < early_packet->timestamp) &&
        early_packet->timestamp - start_timestamp <= abs_audio_offset) &&
        early_packet->timestamp <= easyav1->position)) {

        easyav1_packet *packet = prepare_new_packet(easyav1);

        if (!packet) {
            if (EASYAV1_STATUS_IS_ERROR(easyav1->status)) {
                return EASYAV1_STATUS_ERROR;
            } else {
                continue;
            }
        }

        if (packet->type == early_packet_type) {
            early_packet = packet;
            if (start_timestamp == INVALID_TIMESTAMP) {
                start_timestamp = packet->timestamp;
            }
        } else {
            late_packet = retrieve_packet_from_queue(easyav1, late_queue);
        }
    }

    easyav1->packets.synced = EASYAV1_TRUE;

    if (easyav1->packets.video_queue.count == 0 && easyav1->packets.audio_queue.count == 0 &&
        easyav1->packets.all_fetched == EASYAV1_TRUE) {
        easyav1->status = EASYAV1_STATUS_FINISHED;
    }

    return EASYAV1_STATUS_OK;
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
            easyav1->status = EASYAV1_STATUS_FINISHED;
        }
    }
}

static void release_packets_from_queue(easyav1_t *easyav1, easyav1_packet_queue *queue)
{
    while (queue->count) {
        release_packet_from_queue(easyav1, queue->items + queue->begin);
    }
}

static void release_packet_queue(easyav1_t *easyav1, easyav1_packet_queue *queue)
{
    release_packets_from_queue(easyav1, queue);
    free(queue->items);
    memset(queue, 0, sizeof(easyav1_packet_queue));
}

static easyav1_packet *get_next_packet(easyav1_t *easyav1)
{
    if (easyav1->status == EASYAV1_STATUS_FINISHED) {
        return NULL;
    }

    if (sync_queues(easyav1) != EASYAV1_STATUS_OK) {
        return NULL;
    }

    easyav1_packet *video_packet = retrieve_packet_from_queue(easyav1, &easyav1->packets.video_queue);
    easyav1_packet *audio_packet = retrieve_packet_from_queue(easyav1, &easyav1->packets.audio_queue);

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


/*
 * Decoding functions
 */

static void dequeue_used_video_frames(easyav1_t *easyav1);

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

    dequeue_used_video_frames(easyav1);
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
        easyav1->seek = SEEKING_FOR_KEYFRAME;
    }
    
    return EASYAV1_STATUS_OK;
}

static unsigned int move_audio_buffer(easyav1_t *easyav1, int decoded_samples)
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

    return;
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

    if (easyav1->seek != NOT_SEEKING) {
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
    unsigned int pcm_offset = move_audio_buffer(easyav1, decoded_samples);
    
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
        pcm_offset = move_audio_buffer(easyav1, decoded_samples);
    }

    if (frame->samples > 0) {
        easyav1->audio.has_samples_in_buffer = EASYAV1_TRUE;
    }

    return EASYAV1_STATUS_OK;
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
    if (easyav1->seek == NOT_SEEKING || easyav1->seek == SEEKING_FOR_TIMESTAMP) {

        mtx_lock(&easyav1->video.decoder_thread.mutexes.decoder);

        while (!packet->video_frame) {
            log(EASYAV1_LOG_LEVEL_INFO, "Waiting for video frame to be decoded.");
            cnd_wait(&easyav1->video.decoder_thread.has_frames, &easyav1->video.decoder_thread.mutexes.decoder);
        }

        mtx_unlock(&easyav1->video.decoder_thread.mutexes.decoder);

        if (EASYAV1_STATUS_IS_ERROR(easyav1->status)) {
            return EASYAV1_STATUS_ERROR;
        }

        if (easyav1->settings.skip_unprocessed_frames == EASYAV1_FALSE) {
            callback_video(easyav1);
        }

        return EASYAV1_STATUS_OK;
    }

    // Seeking, do not decode video frames
    if (easyav1->seek == SEEKING_FOR_SQHDR) {
        easyav1_status status = send_packet_data_to_decoder(easyav1, packet, seek_sequence_header);

        if (EASYAV1_STATUS_IS_ERROR(easyav1->status)) {
            return EASYAV1_STATUS_ERROR;
        }
    }

    if (easyav1->seek == SEEKING_FOR_KEYFRAME && packet->is_keyframe == EASYAV1_TRUE) {
        easyav1->seek = SEEKING_FOUND_KEYFRAME;
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

    mtx_lock(&easyav1->video.decoder_thread.mutexes.output);

    dequeue_used_video_frames(easyav1);

    mtx_unlock(&easyav1->video.decoder_thread.mutexes.output);

    easyav1_packet *packet = get_next_packet(easyav1);

    if (easyav1->status == EASYAV1_STATUS_FINISHED) {
        return EASYAV1_STATUS_FINISHED;
    }

    if (!packet) {
        return EASYAV1_STATUS_ERROR;
    }

    easyav1->position = packet->timestamp;

    easyav1_status status = decode_packet(easyav1, packet);

    if (packet->type == PACKET_TYPE_VIDEO) {
        mtx_lock(&easyav1->video.decoder_thread.mutexes.input);
    }

    release_packet_from_queue(easyav1, packet);

    if (packet->type == PACKET_TYPE_VIDEO) {
        mtx_unlock(&easyav1->video.decoder_thread.mutexes.input);
    }

    if (status == EASYAV1_STATUS_OK) {
        callback_video(easyav1);
        callback_audio(easyav1);
    }

    return status;
}

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

easyav1_status easyav1_decode_until(easyav1_t *easyav1, easyav1_timestamp timestamp)
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

    if (timestamp <= easyav1->position) {
        return EASYAV1_STATUS_OK;
    }

    mtx_lock(&easyav1->video.decoder_thread.mutexes.output);

    dequeue_used_video_frames(easyav1);

    mtx_unlock(&easyav1->video.decoder_thread.mutexes.output);

    // Skip to timestamp if too far behind and at different cue points
    if (easyav1->settings.skip_unprocessed_frames == EASYAV1_TRUE &&
        timestamp - easyav1->position > DECODE_UNTIL_SKIP_MS &&
        get_closest_cue_point(easyav1, easyav1->position) < get_closest_cue_point(easyav1, timestamp - 30)) {
        log(EASYAV1_LOG_LEVEL_INFO, "Decoder too far behind at %llu, skipping to requested timestamp %llu.",
            easyav1->position, timestamp);

        // Leave 30ms difference to get something actually decoded on this run
        easyav1_seek_to_timestamp(easyav1, timestamp - 30);
    }

    easyav1_status status = easyav1->status;

    while (status == EASYAV1_STATUS_OK) {
        easyav1_packet *packet = get_next_packet(easyav1);

        if (easyav1->status == EASYAV1_STATUS_FINISHED) {
            status = EASYAV1_STATUS_FINISHED;
            break;
        }

        if (packet->timestamp >= timestamp) {
            break;
        }

        if (!packet) {
            return EASYAV1_STATUS_ERROR;
        }

        easyav1->position = packet->timestamp;

        easyav1_status status = decode_packet(easyav1, packet);

        if (packet->type == PACKET_TYPE_VIDEO) {
            mtx_lock(&easyav1->video.decoder_thread.mutexes.input);
        }
    
        release_packet_from_queue(easyav1, packet);

        if (packet->type == PACKET_TYPE_VIDEO) {
            mtx_unlock(&easyav1->video.decoder_thread.mutexes.input);
        }
    }

    if (status == EASYAV1_STATUS_OK) {
        easyav1->position = timestamp;
    }

    if (status != EASYAV1_STATUS_ERROR) {
        callback_video(easyav1);
        callback_audio(easyav1);
    }

    return status;
}

easyav1_status easyav1_decode_for(easyav1_t *easyav1, easyav1_timestamp time)
{
    return easyav1_decode_until(easyav1, easyav1->position + time);
}


/*
 * Seeking functions
 */

static void dequeue_all_video_frames(easyav1_t *easyav1);

static void pause_video_decoder(easyav1_t *easyav1)
{
    mtx_lock(&easyav1->video.decoder_thread.mutexes.input);
    mtx_lock(&easyav1->video.decoder_thread.mutexes.decoder);

    // Wait for the video decoder to finish processing pending frame
    if (get_unencoded_video_packet(easyav1, &easyav1->packets.video_queue) == NULL) {
        mtx_unlock(&easyav1->video.decoder_thread.mutexes.decoder);
        return;
    }

    mtx_unlock(&easyav1->video.decoder_thread.mutexes.decoder);

    unsigned int pending_frames = easyav1->video.frame_queue.count;
    unsigned int queue_begin = easyav1->video.frame_queue.begin;

    while (easyav1->status != EASYAV1_STATUS_FINISHED && pending_frames == easyav1->video.frame_queue.count &&
        queue_begin == easyav1->video.frame_queue.begin) {
        log(EASYAV1_LOG_LEVEL_INFO, "Waiting for video decoder to finish processing pending frames.");
        cnd_wait(&easyav1->video.decoder_thread.has_frames, &easyav1->video.decoder_thread.mutexes.input);
    }
}

easyav1_status easyav1_seek_to_timestamp(easyav1_t *easyav1, easyav1_timestamp timestamp)
{
    if (!easyav1) {
        log(EASYAV1_LOG_LEVEL_INFO, "Handle is NULL");
        return EASYAV1_STATUS_ERROR;
    }

    if (EASYAV1_STATUS_IS_ERROR(easyav1->status)) {
        return EASYAV1_STATUS_ERROR;
    }

    if (timestamp == easyav1->position) {
        return EASYAV1_STATUS_OK;
    }

    easyav1_timestamp duration = easyav1_get_duration(easyav1);

    if (duration && timestamp >= easyav1_get_duration(easyav1)) {
        log(EASYAV1_LOG_LEVEL_INFO, "Requested timestamp is beyond the end of the stream.");
        timestamp = duration;

        if (easyav1->status == EASYAV1_STATUS_FINISHED) {
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

    /*
     * Two-pass seek when there's video:
     *
     * - The first pass finds the closest keyframe to the requested timestamp without decoding anything
     * - The second pass skips all video decoding until that keyframe
     */
    easyav1_timestamp last_keyframe_timestamp = 0;

    easyav1_bool audio_is_active = easyav1->audio.active;

    pause_video_decoder(easyav1);

    easyav1->status = EASYAV1_STATUS_OK;

    for (int pass = 0; pass < 2; pass++) {

        easyav1->position = corrected_timestamp;

        if (nestegg_track_seek(easyav1->webm.context, track, ms_to_internal_timestmap(easyav1, corrected_timestamp))) {
            LOG_AND_SET_ERROR(EASYAV1_STATUS_IO_ERROR, "Failed to seek to requested timestamp %u.", easyav1->position);
            mtx_unlock(&easyav1->video.decoder_thread.mutexes.input);
            return EASYAV1_STATUS_ERROR;
        }

        release_packets_from_queue(easyav1, &easyav1->packets.video_queue);
        release_packets_from_queue(easyav1, &easyav1->packets.audio_queue);

        easyav1->packets.synced = EASYAV1_FALSE;
        easyav1->packets.all_fetched = EASYAV1_FALSE;
        easyav1->status = EASYAV1_STATUS_OK;

        if (easyav1->video.active == EASYAV1_TRUE) {
            dequeue_all_video_frames(easyav1);
            dav1d_flush(easyav1->video.context);
            easyav1->seek = SEEKING_FOR_SQHDR;
        }

        if (audio_is_active) {
            if (easyav1->seek == NOT_SEEKING) {
                easyav1->seek = SEEKING_FOR_TIMESTAMP;

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
                mtx_unlock(&easyav1->video.decoder_thread.mutexes.input);
                return EASYAV1_STATUS_ERROR;
            }

            if (packet) {
                easyav1->position = packet->timestamp;

                if (pass == 1) {
                    if (packet->timestamp >= last_keyframe_timestamp) {

                        // If fast seeking is enabled, we don't decode until we find the correct timestamp, rather we
                        // resume playing immediately after we get to the last keyframe before the requested timestamp
                        if (easyav1->settings.use_fast_seeking == EASYAV1_TRUE) {
                            easyav1->seek = NOT_SEEKING;
                            mtx_unlock(&easyav1->video.decoder_thread.mutexes.input);
                            cnd_signal(&easyav1->video.decoder_thread.has_packets);

                            break;
                        } else if (easyav1->seek != SEEKING_FOR_TIMESTAMP) {
                            easyav1->seek = SEEKING_FOR_TIMESTAMP;
                            mtx_unlock(&easyav1->video.decoder_thread.mutexes.input);
                            cnd_signal(&easyav1->video.decoder_thread.has_packets);
                        }
                    } else if (easyav1->seek == SEEKING_FOR_TIMESTAMP) {
                        easyav1->seek = SEEKING_FOUND_KEYFRAME;
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
                        mtx_unlock(&easyav1->video.decoder_thread.mutexes.input);
                        return EASYAV1_STATUS_ERROR;
                    }

                    corrected_timestamp = get_closest_cue_point(easyav1, corrected_timestamp);
                    last_keyframe_timestamp = 0;
                    pass = -1;
                }

                easyav1->seek = NOT_SEEKING;
                break;
            }

            seeking_mode last_seek_mode = easyav1->seek;

            if (decode_packet(easyav1, packet) == EASYAV1_STATUS_ERROR) {
                if (easyav1->seek != SEEKING_FOR_TIMESTAMP) {
                    mtx_unlock(&easyav1->video.decoder_thread.mutexes.input);
                }
                log(EASYAV1_LOG_LEVEL_ERROR, "Failed to decode packet when seeking.");
                return EASYAV1_STATUS_ERROR;
            }

            if (pass == 0 && easyav1->seek == SEEKING_FOUND_KEYFRAME) {
                last_keyframe_timestamp = packet->timestamp;
                easyav1->seek = SEEKING_FOR_KEYFRAME;
            }

            // If we found a sequence header, we may need to decode the video frame from this packet.
            // To do that, we need to keep the packet so it's fetched again and properly decoded.
            if (pass == 0 || (pass == 1 && (last_seek_mode != SEEKING_FOR_SQHDR || last_seek_mode == easyav1->seek ||
                packet->timestamp < last_keyframe_timestamp))) {

                if (easyav1->seek == SEEKING_FOR_TIMESTAMP) {
                    mtx_lock(&easyav1->video.decoder_thread.mutexes.input);
                }

                release_packet_from_queue(easyav1, packet);

                if (easyav1->seek == SEEKING_FOR_TIMESTAMP) {
                    mtx_unlock(&easyav1->video.decoder_thread.mutexes.input);
                }
            }
        }
    }

    log(EASYAV1_LOG_LEVEL_INFO, "Seeked to timestamp %llu from timestamp %llu.", easyav1->position,
        original_timestamp);

    return EASYAV1_STATUS_OK;
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


/*
 * Output functions
 */

static queued_video_frame_t *retrieve_undisplayed_video_frame(easyav1_t *easyav1)
{
    queued_video_frame_t *frame = NULL;

    unsigned int frame_count = 0;

    for (size_t i = 0; i < easyav1->video.frame_queue.count; i++) {
        size_t index = (easyav1->video.frame_queue.begin + i) % VIDEO_FRAME_QUEUE_SIZE;

        if (easyav1->video.frame_queue.frames[index].displayed == EASYAV1_FALSE &&
            easyav1->video.frame_queue.frames[index].pic.m.timestamp <= easyav1->position) {
            if (frame) {
                frame_count++;
            }
            frame = &easyav1->video.frame_queue.frames[index];
        } else if (easyav1->video.frame_queue.frames[index].pic.m.timestamp > easyav1->position) {
            break;
        }
    }

    if (frame_count > 0) {
        log(EASYAV1_LOG_LEVEL_INFO, "Skipping %u video frames.", frame_count);
    }

    return frame;
}

static void dequeue_video_frame(easyav1_t *easyav1)
{
    queued_video_frame_t *frame = &easyav1->video.frame_queue.frames[easyav1->video.frame_queue.begin];

    dav1d_picture_unref(&frame->pic);
    memset(frame, 0, sizeof(queued_video_frame_t));
    easyav1->video.frame_queue.count--;

    if (easyav1->video.frame_queue.count == 0) {
        easyav1->video.frame_queue.begin = 0;
    } else {
        easyav1->video.frame_queue.begin = (easyav1->video.frame_queue.begin + 1) % VIDEO_FRAME_QUEUE_SIZE;
    }
}

static void dequeue_used_video_frames(easyav1_t *easyav1)
{
    queued_video_frame_t *displayed_frame = NULL;

    for (size_t i = 0; i < easyav1->video.frame_queue.count; i++) {
        size_t index = (easyav1->video.frame_queue.begin + i) % VIDEO_FRAME_QUEUE_SIZE;

        if (easyav1->video.frame_queue.frames[index].displayed == EASYAV1_TRUE) {
            displayed_frame = &easyav1->video.frame_queue.frames[index];
        }
    }

    if (displayed_frame == NULL) {
        return;
    }

    queued_video_frame_t *frame = &easyav1->video.frame_queue.frames[easyav1->video.frame_queue.begin];

    while (frame != displayed_frame) {
        dequeue_video_frame(easyav1);
        frame = &easyav1->video.frame_queue.frames[easyav1->video.frame_queue.begin];
    }

    dequeue_video_frame(easyav1);

    if (easyav1->video.frame_queue.count == 0) {
        easyav1->video.frame_queue.begin = 0;
    }
}

static void dequeue_all_video_frames(easyav1_t *easyav1)
{
    while (easyav1->video.frame_queue.count > 0) {
        dequeue_video_frame(easyav1);
    }
}

static queued_video_frame_t *enqueue_video_frame(easyav1_t *easyav1)
{
    if (easyav1->video.frame_queue.count >= VIDEO_FRAME_QUEUE_SIZE) {
        log(EASYAV1_LOG_LEVEL_INFO, "Video frame queue is full, discarding oldest frame.");
        dequeue_video_frame(easyav1);
    }

    size_t index = (easyav1->video.frame_queue.begin + easyav1->video.frame_queue.count) % VIDEO_FRAME_QUEUE_SIZE;
    queued_video_frame_t *frame = &easyav1->video.frame_queue.frames[index];
    frame->displayed = EASYAV1_FALSE;

    easyav1->video.frame_queue.count++;

    return frame;
}

easyav1_bool easyav1_has_video_frame(easyav1_t *easyav1)
{
    if (!easyav1) {
        log(EASYAV1_LOG_LEVEL_WARNING, "Handle is NULL");
        return EASYAV1_FALSE;
    }

    easyav1_bool has_frame = retrieve_undisplayed_video_frame(easyav1) == NULL ? EASYAV1_FALSE : EASYAV1_TRUE;

    return has_frame;
}

const easyav1_video_frame *easyav1_get_video_frame(easyav1_t *easyav1)
{
    if (!easyav1) {
        log(EASYAV1_LOG_LEVEL_WARNING, "Handle is NULL");
        return NULL;
    }

    mtx_lock(&easyav1->video.decoder_thread.mutexes.output);

    queued_video_frame_t *queued_frame = retrieve_undisplayed_video_frame(easyav1);

    if (!queued_frame) {
        mtx_unlock(&easyav1->video.decoder_thread.mutexes.output);
        return NULL;
    }

    queued_frame->displayed = EASYAV1_TRUE;

    easyav1_picture_type type = EASYAV1_PICTURE_TYPE_UNKNOWN;

    Dav1dPicture *pic = &queued_frame->pic;

    switch (pic->p.layout) {
        case DAV1D_PIXEL_LAYOUT_I400:
            type = EASYAV1_PICTURE_TYPE_YUV400_8BPC;
            break;
        case DAV1D_PIXEL_LAYOUT_I420:
            type = EASYAV1_PICTURE_TYPE_YUV420_8BPC;
            break;
        case DAV1D_PIXEL_LAYOUT_I422:
            type = EASYAV1_PICTURE_TYPE_YUV422_8BPC;
            break;
        case DAV1D_PIXEL_LAYOUT_I444:
            type = EASYAV1_PICTURE_TYPE_YUV444_8BPC;
            break;
        default:
            mtx_unlock(&easyav1->video.decoder_thread.mutexes.output);
            log(EASYAV1_LOG_LEVEL_WARNING, "Unsupported pixel layout.");
            return NULL;
    }

    if (pic->p.bpc == 10) {
        type += EASYAV1_PICTURE_TYPE_10BPC_OFFSET;
    } else if (pic->p.bpc != 8) {
        mtx_unlock(&easyav1->video.decoder_thread.mutexes.output);
        log(EASYAV1_LOG_LEVEL_WARNING, "Unsupported bit depth: %d.", pic->p.bpc);
        return NULL;
    }

    easyav1_video_frame *frame = &easyav1->video.frame;

    frame->picture_type = type;

    frame->data[0] = pic->data[0];
    frame->data[1] = pic->data[1];
    frame->data[2] = pic->data[2];

    frame->stride[0] = pic->stride[0];
    frame->stride[1] = pic->stride[1];
    frame->stride[2] = pic->stride[1];

    frame->width = (unsigned int) pic->p.w;
    frame->height = (unsigned int) pic->p.h;

    frame->timestamp = (uint64_t) pic->m.timestamp;

    mtx_unlock(&easyav1->video.decoder_thread.mutexes.output);

    return frame;
}

uint64_t easyav1_get_total_video_frames_processed(const easyav1_t *easyav1)
{
    if (!easyav1) {
        log(EASYAV1_LOG_LEVEL_WARNING, "Handle is NULL");
        return 0;
    }

    return easyav1->video.processed_frames;
}

easyav1_bool easyav1_is_audio_buffer_filled(const easyav1_t *easyav1)
{
    if (!easyav1) {
        log(EASYAV1_LOG_LEVEL_WARNING, "Handle is NULL");
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

    if (!easyav1->audio.has_samples_in_buffer) {
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


/*
 * Information functions
 */

easyav1_settings easyav1_default_settings(void)
{
    return DEFAULT_SETTINGS;
}

easyav1_status easyav1_get_status(const easyav1_t *easyav1)
{
    if (!easyav1) {
        log(EASYAV1_LOG_LEVEL_WARNING, "Handle is NULL");
        return EASYAV1_STATUS_ERROR;
    }

    if (EASYAV1_STATUS_IS_ERROR(easyav1->status)) {
        return EASYAV1_STATUS_ERROR;
    }

    return easyav1->status;
}

easyav1_timestamp easyav1_get_current_timestamp(const easyav1_t *easyav1)
{
    if (!easyav1) {
        log(EASYAV1_LOG_LEVEL_WARNING, "Handle is NULL");
        return 0;
    }

    return easyav1->position;
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

easyav1_bool easyav1_is_finished(const easyav1_t *easyav1)
{
    if (!easyav1) {
        log(EASYAV1_LOG_LEVEL_WARNING, "Handle is NULL");
        return EASYAV1_FALSE;
    }

    return easyav1->status == EASYAV1_STATUS_FINISHED ? EASYAV1_TRUE : EASYAV1_FALSE;
}


/*
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

static void destroy_video(easyav1_t *easyav1);
static void destroy_audio(easyav1_t *easyav1);

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

    if (must_seek == EASYAV1_TRUE &&
        easyav1->packets.started_fetching == EASYAV1_TRUE && easyav1_is_finished(easyav1) == EASYAV1_FALSE) {
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


/*
 * Destruction functions
 */

static void destroy_video(easyav1_t *easyav1)
{
    pause_video_decoder(easyav1);
    
    dequeue_all_video_frames(easyav1);

    easyav1->video.decoder_thread.exiting = EASYAV1_TRUE;

    mtx_unlock(&easyav1->video.decoder_thread.mutexes.input);

    // Force the decoder thread to exit
    cnd_signal(&easyav1->video.decoder_thread.has_packets);

    thrd_join(easyav1->video.decoder_thread.decoder, NULL);

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

    release_packet_queue(easyav1, &easyav1->packets.video_queue);
    release_packet_queue(easyav1, &easyav1->packets.audio_queue);

    if (easyav1->webm.context) {
        nestegg_destroy(easyav1->webm.context);
    }

    mtx_destroy(&easyav1->video.decoder_thread.mutexes.input);
    mtx_destroy(&easyav1->video.decoder_thread.mutexes.decoder);
    mtx_destroy(&easyav1->video.decoder_thread.mutexes.output);
    cnd_destroy(&easyav1->video.decoder_thread.has_packets);
    cnd_destroy(&easyav1->video.decoder_thread.has_frames);

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
