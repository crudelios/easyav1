#include "easyav1.h"

#define OGG_IMPL
#define VORBIS_IMPL

#include "nestegg/nestegg.h"
#include "dav1d/dav1d.h"
#include "minivorbis/minivorbis.h"

#include <stdlib.h>
#include <string.h>

#define VORBIS_HEADERS_COUNT 3
#define DECODE_UNTIL_SKIP_MS 1000

typedef enum {
    STREAM_TYPE_NONE = 0,
    STREAM_TYPE_FILE,
    STREAM_TYPE_MEMORY
} stream_type;

typedef enum {
    NOT_SEEKING = 0,
    SEEKING_FOR_SQHDR = 1,
    SEEKING_FOR_KEYFRAME = 2,
    SEEKING_FOR_TIMESTAMP = 3
} seeking_mode;

typedef struct easyav1_t {
    struct {
        nestegg *context;
        nestegg_packet *packet;
        unsigned int num_tracks;
    } webm;
    struct {
        struct {
            Dav1dContext *context;
            Dav1dPicture pic;
        } av1;

        int has_picture_to_output;

        unsigned int active;
        unsigned int track;

        unsigned int width;
        unsigned int height;

        easyav1_video_frame frame;
    } video;
    struct {
        struct {
            vorbis_info info;
            vorbis_block block;
            vorbis_dsp_state dsp;
        } vorbis;

        unsigned int active;
        unsigned int track;

        unsigned int channels;
        unsigned int sample_rate;

        float *buffer;

        int has_samples_in_buffer;

        easyav1_audio_frame frame;
    } audio;
    struct {
        void *data;
        stream_type type;
    } stream;
    easyav1_settings settings;
    easyav1_timestamp timestamp;
    seeking_mode seek;
    int is_finished;
} easyav1_t;

typedef struct {
    uint8_t *data;
    int64_t offset;
    size_t size;
} easyav1_memory;

static const easyav1_settings DEFAULT_SETTINGS = {
    .enable_video = 1,
    .enable_audio = 1,
    .skip_unprocessed_frames = 1,
    .interlace_audio = 1,
    .video_conversion = EASYAV1_VIDEO_CONVERSION_NONE,
    .close_handle_on_destroy = 0,
    .callbacks = {
        .video = 0,
        .audio = 0,
        .userdata = 0
    },
    .video_track = 0,
    .audio_track = 0,
    .max_audio_samples = 4096,
    .log_level = EASYAV1_LOG_LEVEL_WARNING
};

typedef int (*decoder_function)(easyav1_t *, uint8_t *, size_t);


/*
 * Utility functions
 */
static inline easyav1_timestamp ns_to_ms(easyav1_timestamp ns)
{
    return ns / 1000000;
}

static inline easyav1_timestamp ms_to_ns(easyav1_timestamp ms)
{
    return ms * 1000000;
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
    return fseek(f, offset, origin);
}

static int64_t file_tell(void *userdata)
{
    FILE *f = (FILE *) userdata;
    if (!f) {
        return -1;
    }
    return ftell(f);
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

static int init_video(easyav1_t *easyav1, unsigned int track)
{
    nestegg_video_params params;

    if (nestegg_track_video_params(easyav1->webm.context, track, &params)) {
        log(EASYAV1_LOG_LEVEL_ERROR, "Failed to get video track parameters.");
        return 0;
    }

    Dav1dSettings dav1d_settings;
    dav1d_default_settings(&dav1d_settings);
    dav1d_settings.logger = (Dav1dLogger) { .cookie = 0, .callback = log_from_dav1d };

    if (dav1d_open(&easyav1->video.av1.context, &dav1d_settings) < 0) {
        log(EASYAV1_LOG_LEVEL_ERROR, "Failed to initialize AV1 decoder.");
        return 0;
    }

    easyav1->video.active = 1;
    easyav1->video.track = track;

    easyav1->video.width = params.width;
    easyav1->video.height = params.height;

    log(EASYAV1_LOG_LEVEL_INFO, "Video initialized. Size: %ux%u.", easyav1->video.width, easyav1->video.height);

    return 1;
}

static unsigned int prepare_audio_buffer(easyav1_t *easyav1)
{
    unsigned max_samples = easyav1->settings.max_audio_samples * easyav1->audio.channels;

    easyav1->audio.buffer = malloc(max_samples * sizeof(float));

    if (!easyav1->audio.buffer) {
        log(EASYAV1_LOG_LEVEL_ERROR, "Failed to allocate audio buffer.");
        return 0;
    }

    memset(easyav1->audio.buffer, 0, max_samples * sizeof(float));

    easyav1->audio.frame.channels = easyav1->audio.channels;

    if (easyav1->settings.interlace_audio) {
        easyav1->audio.frame.pcm.interlaced = easyav1->audio.buffer;
        return 1;
    }

    easyav1->audio.frame.pcm.deinterlaced = malloc(easyav1->audio.channels * sizeof(float *));
    if (!easyav1->audio.frame.pcm.deinterlaced) {
        log(EASYAV1_LOG_LEVEL_ERROR, "Failed to allocate deinterlaced audio buffer.");
        return 0;
    }
    for (unsigned int i = 0; i < easyav1->audio.channels; i++) {
        easyav1->audio.frame.pcm.deinterlaced[i] = easyav1->audio.buffer + i * easyav1->settings.max_audio_samples;
    }

    return 1;
}

static int init_audio(easyav1_t *easyav1, unsigned int track)
{
    unsigned int headers;

    if (nestegg_track_codec_data_count(easyav1->webm.context, track, &headers)) {
        log(EASYAV1_LOG_LEVEL_ERROR, "Failed to get audio codec header count.");
        return 0;
    }

    // Vorbis data should always have 3 headers
    if (headers != VORBIS_HEADERS_COUNT) {
        log(EASYAV1_LOG_LEVEL_ERROR, "Vorbis data should always have 3 headers.");
        return 0;
    }

    vorbis_comment comment;

    vorbis_info_init(&easyav1->audio.vorbis.info);
    vorbis_comment_init(&comment);

    for (int header = 0; header < VORBIS_HEADERS_COUNT; header++) {
        unsigned char *header_data;
        size_t header_size;

        if (nestegg_track_codec_data(easyav1->webm.context, track, header, &header_data, &header_size)) {
            log(EASYAV1_LOG_LEVEL_ERROR, "Failed to get audio codec header data for header %u.", header);
            return 0;
        }

        ogg_packet packet;
        packet.packet = header_data;
        packet.bytes = header_size;
        packet.b_o_s = header == 0;
        packet.e_o_s = header == VORBIS_HEADERS_COUNT - 1;
        packet.granulepos = 0;
        packet.packetno = header;

        if (vorbis_synthesis_headerin(&easyav1->audio.vorbis.info, &comment, &packet)) {
            log(EASYAV1_LOG_LEVEL_ERROR, "Failed to process audio codec header %u.", header);
            return 0;
        }
    }

    vorbis_comment_clear(&comment);

    if (vorbis_synthesis_init(&easyav1->audio.vorbis.dsp, &easyav1->audio.vorbis.info)) {
        log(EASYAV1_LOG_LEVEL_ERROR, "Failed to initialize vorbis synthesis.");
        return 0;
    }

    if (vorbis_block_init(&easyav1->audio.vorbis.dsp, &easyav1->audio.vorbis.block)) {
        log(EASYAV1_LOG_LEVEL_ERROR, "Failed to initialize vorbis block.");
        return 0;
    }

    nestegg_audio_params params;
    if (nestegg_track_audio_params(easyav1->webm.context, track, &params)) {
        log(EASYAV1_LOG_LEVEL_ERROR, "Failed to get audio track parameters.");
        return 0;
    }

    easyav1->audio.active = 1;
    easyav1->audio.track = track;

    easyav1->audio.channels = params.channels;
    easyav1->audio.sample_rate = params.rate;

    if (!prepare_audio_buffer(easyav1)) {
        return 0;
    }

    log(EASYAV1_LOG_LEVEL_INFO, "Audio initialized. Channels: %u, sample rate: %uhz.",
        easyav1->audio.channels, easyav1->audio.sample_rate);

    return 1;
}

static int init_webm_tracks(easyav1_t *easyav1)
{
    if (nestegg_track_count(easyav1->webm.context, &easyav1->webm.num_tracks)) {
        log(EASYAV1_LOG_LEVEL_ERROR, "Failed to get track count");
        return 0;
    }

    unsigned int current_video_track = 0;
    unsigned int current_audio_track = 0;

    for (unsigned int track = 0; track < easyav1->webm.num_tracks; track++) {

        int type = nestegg_track_type(easyav1->webm.context, track);
    
        if (type == -1) {
            log(EASYAV1_LOG_LEVEL_ERROR, "Failed to get track type.");
            return 0;
        }

        if (type == NESTEGG_TRACK_UNKNOWN) {
            log(EASYAV1_LOG_LEVEL_WARNING, "Unknown track type found, ignoring.");
            continue;
        }

        int codec = nestegg_track_codec_id(easyav1->webm.context, track);

        if (codec == -1) {
            log(EASYAV1_LOG_LEVEL_ERROR, "Failed to get codec for track %u.", track);
            return 0;
        }

        if (type == NESTEGG_TRACK_VIDEO) {

            // Video already found or disabled - skip
            if (!easyav1->settings.enable_video || easyav1->video.active) {
                continue;
            }

            if (current_video_track != easyav1->settings.video_track) {
                current_video_track++;
                continue;
            }

            log(EASYAV1_LOG_LEVEL_INFO, "Found requested video track %u at webm track %u.", current_video_track, track);

            if (codec != NESTEGG_CODEC_AV1) {
                log(EASYAV1_LOG_LEVEL_WARNING, "Unsupported video codec found. Only AV1 codec is supported. Not displaying video.");
                continue;
            }

            if (!init_video(easyav1, track)) {
                return 0;
            }
        }

        if (type == NESTEGG_TRACK_AUDIO) {

            // Audio already found or disabled - skip
            if (!easyav1->settings.enable_audio || easyav1->audio.active) {
                continue;
            }

            if (current_audio_track != easyav1->settings.audio_track) {
                current_audio_track++;
                continue;
            }

            log(EASYAV1_LOG_LEVEL_INFO, "Found requested audio track %u at webm track %u.", current_audio_track, track);

            if (codec != NESTEGG_CODEC_VORBIS) {
                log(EASYAV1_LOG_LEVEL_WARNING, "Unsupported audio codec found. Only vorbis codec is supported. Not playing audio.");
                continue;
            }

            if (!init_audio(easyav1, track)) {
                return 0;
            }
        }
    }
    return 1;
}

easyav1_t *easyav1_init_from_custom_stream(const easyav1_stream *stream, const easyav1_settings *settings)
{
    easyav1_t *easyav1 = 0;

    if (!stream || !stream->read_func || !stream->seek_func || !stream->tell_func) {
        log(EASYAV1_LOG_LEVEL_ERROR, "Stream is NULL or missing read, seek or tell functions");
        return 0;
    }

    easyav1 = malloc(sizeof(easyav1_t));

    if (!easyav1) {
        log(EASYAV1_LOG_LEVEL_ERROR, "Failed to allocate memory for the easyav1 handle");
        return 0;
    }

    memset(easyav1, 0, sizeof(easyav1_t));

    if (settings) {
        easyav1->settings = *settings;
    } else {
        easyav1->settings = DEFAULT_SETTINGS;
        log(EASYAV1_LOG_LEVEL_WARNING, "No settings provided, using default settings!");
        log(EASYAV1_LOG_LEVEL_WARNING, "By default neither video nor audio are processed.");
    }

    nestegg_io io = {
        .read = stream->read_func,
        .seek = stream->seek_func,
        .tell = stream->tell_func,
        .userdata = stream->userdata
    };

    if (nestegg_init(&easyav1->webm.context, io, log_from_nestegg, -1)) {
        log(EASYAV1_LOG_LEVEL_ERROR, "Failed to initialize webm context");
        easyav1_destroy(&easyav1);
        return 0;
    }

    if (!init_webm_tracks(easyav1)) {
        easyav1_destroy(&easyav1);
        return 0;
    }

    return easyav1;
}

easyav1_t *easyav1_init_from_memory(const void *data, size_t size, const easyav1_settings *settings)
{
    easyav1_t *easyav1 = 0;

    if (!data || !size) {
        log(EASYAV1_LOG_LEVEL_ERROR, "Data is NULL or size is 0");
        return 0;
    }

    easyav1_memory *mem = malloc(sizeof(easyav1_memory));

    if (!mem) {
        log(EASYAV1_LOG_LEVEL_ERROR, "Failed to allocate memory for memory stream");
        return 0;
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
        return 0;
    }

    easyav1->stream.type = STREAM_TYPE_MEMORY;
    easyav1->stream.data = mem;

    return easyav1;
}

easyav1_t *easyav1_init_from_file(FILE *f, const easyav1_settings *settings)
{
    easyav1_t *easyav1 = 0;

    if (!f) {
        log(EASYAV1_LOG_LEVEL_ERROR, "File handle is NULL");
        return 0;
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
        return 0;
    }

    if (easyav1->settings.close_handle_on_destroy) {
        easyav1->stream.type = STREAM_TYPE_FILE;
        easyav1->stream.data = f;
    }

    return easyav1;
}

easyav1_t *easyav1_init_from_filename(const char *filename, const easyav1_settings *settings)
{
    easyav1_t *easyav1 = 0;

    if (!filename) {
        log(EASYAV1_LOG_LEVEL_ERROR, "Filename is NULL");
        return 0;
    }

    FILE *f = fopen(filename, "rb");

    if (!f) {
        log(EASYAV1_LOG_LEVEL_ERROR, "Failed to open file %s", filename);
        return 0;
    }

    easyav1 = easyav1_init_from_file(f, settings);

    if (!easyav1) {
        log(EASYAV1_LOG_LEVEL_ERROR, "Failed to create easyav1 structure from file %s", filename);
        fclose(f);
        return 0;
    }

    easyav1->stream.type = STREAM_TYPE_FILE;
    easyav1->stream.data = f;

    return easyav1;
}


/*
 * Decoding functions
 */

static int get_next_packet(easyav1_t *easyav1)
{
    if (easyav1->webm.packet) {
        return 1;
    }

    int result = nestegg_read_packet(easyav1->webm.context, &easyav1->webm.packet);

    if (result < 0) {
        log(EASYAV1_LOG_LEVEL_ERROR, "Failed to read packet from webm context");
        return 0;
    } else if (result == 0) {
        easyav1->is_finished = 1;
        log(EASYAV1_LOG_LEVEL_INFO, "End of stream");
    }

    return result;
}

static void free_nothing(const uint8_t *data, void *cookie)
{}

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

    dav1d_picture_unref(&easyav1->video.av1.pic);
}

static int decode_video(easyav1_t *easyav1, uint8_t *data, size_t size)
{
    // Handle seeking
    if (easyav1->seek == SEEKING_FOR_SQHDR) {
        log(EASYAV1_LOG_LEVEL_INFO, "Seeking to keyframe");

        Dav1dSequenceHeader seq_hdr;

        int result = dav1d_parse_sequence_header(&seq_hdr, data, size);

        if (result < 0 && result != DAV1D_ERR(ENOENT)) {
            log(EASYAV1_LOG_LEVEL_ERROR, "Failed to parse sequence header");
            return 0;
        }

        if (result == 0) {
            easyav1->seek = SEEKING_FOR_KEYFRAME;
        } else {
            return 1;
        }
    }

    if (easyav1->seek == SEEKING_FOR_KEYFRAME) {
        int result = nestegg_packet_has_keyframe(easyav1->webm.packet);

        if (result == -1) {
            log(EASYAV1_LOG_LEVEL_ERROR, "Failed to get keyframe status.");
            return 0;
        }

        if (result == NESTEGG_PACKET_HAS_KEYFRAME_TRUE) {
            easyav1->seek = SEEKING_FOR_TIMESTAMP;
        } else {
            return 1;
        }
    }

    Dav1dData buf = { 0 };
    int result = dav1d_data_wrap(&buf, data, size, free_nothing, 0);

    if (result < 0) {
        log(EASYAV1_LOG_LEVEL_ERROR, "Failed to create data buffer");
        return 0;
    }

    do {

        result = dav1d_send_data(easyav1->video.av1.context, &buf);
        if (result < 0 && result != DAV1D_ERR(EAGAIN)) {
            log(EASYAV1_LOG_LEVEL_ERROR, "Failed to send data to AV1 decoder");
            dav1d_data_unref(&buf);
            return 0;
        }

        Dav1dPicture pic = { 0 };
        result = dav1d_get_picture(easyav1->video.av1.context, &pic);

        // For some reason, sometimes we need to insist on getting the picture without sending more data
        if (result == DAV1D_ERR(EAGAIN)) {
            result = dav1d_get_picture(easyav1->video.av1.context, &pic);
        }

        if (result < 0) {
            if (result == DAV1D_ERR(EAGAIN)) {
                continue;
            }
            log(EASYAV1_LOG_LEVEL_ERROR, "Failed to get picture from AV1 decoder");
            dav1d_data_unref(&buf);
            return 0;
        }

        if (easyav1->seek == SEEKING_FOR_TIMESTAMP) {
            dav1d_picture_unref(&pic);
            continue;
        }

        if (pic.p.layout != DAV1D_PIXEL_LAYOUT_I420) {
            log(EASYAV1_LOG_LEVEL_WARNING, "Unsupported pixel layou, ignoring frame");
            dav1d_picture_unref(&pic);
            continue;
        }

        if (easyav1->video.has_picture_to_output && easyav1->settings.skip_unprocessed_frames) {
            log(EASYAV1_LOG_LEVEL_INFO, "Undisplayed frame still pending, discarding it");
        }

        dav1d_picture_unref(&easyav1->video.av1.pic);

        easyav1->video.av1.pic = pic;
        easyav1->video.has_picture_to_output = 1;

        if (!easyav1->settings.skip_unprocessed_frames) {
            callback_video(easyav1);
        }

    } while (buf.sz > 0);

    dav1d_data_unref(&buf);

    return 1;
}

static unsigned int move_audio_buffer(easyav1_t *easyav1, int decoded_samples)
{
    // If we have too many samples, we need to discard some
    if (decoded_samples > easyav1->settings.max_audio_samples) {
        easyav1->audio.frame.samples = 0;
        return decoded_samples - easyav1->settings.max_audio_samples;
    }

    if (decoded_samples + easyav1->audio.frame.samples <= easyav1->settings.max_audio_samples) {
        return 0;
    }

    // If the audio buffer is too small for the incoming samples, we need to move the stored samples to fit the new ones
    unsigned int samples_to_move = decoded_samples + easyav1->audio.frame.samples - easyav1->settings.max_audio_samples;

    log(EASYAV1_LOG_LEVEL_INFO, "Audio buffer full, moving %u samples to fit new samples.", samples_to_move);

    if (easyav1->settings.interlace_audio) {
        memmove(easyav1->audio.buffer, easyav1->audio.buffer + samples_to_move * easyav1->audio.channels,
            (easyav1->settings.max_audio_samples - samples_to_move) * easyav1->audio.channels * sizeof(float));
    } else {
        for (unsigned int channel = 0; channel < easyav1->audio.channels; channel++) {
            memmove(easyav1->audio.buffer + channel * easyav1->settings.max_audio_samples,
                easyav1->audio.buffer + channel * easyav1->settings.max_audio_samples + samples_to_move,
                (easyav1->settings.max_audio_samples - samples_to_move) * sizeof(float));
        }
    }

    easyav1->audio.frame.samples = easyav1->settings.max_audio_samples - decoded_samples;

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

static int decode_audio(easyav1_t *easyav1, uint8_t *data, size_t size)
{
    ogg_packet packet = {
        .packet = data,
        .bytes = size
    };

    if (vorbis_synthesis(&easyav1->audio.vorbis.block, &packet) ||
        vorbis_synthesis_blockin(&easyav1->audio.vorbis.dsp, &easyav1->audio.vorbis.block)) {
        log(EASYAV1_LOG_LEVEL_ERROR, "Failed to process audio packet.");
        return 0;
    }

    if (!easyav1->audio.has_samples_in_buffer) {
        easyav1->audio.frame.samples = 0;
    }

    if (easyav1->seek != NOT_SEEKING) {
        int decoded_samples = vorbis_synthesis_pcmout(&easyav1->audio.vorbis.dsp, 0);
        vorbis_synthesis_read(&easyav1->audio.vorbis.dsp, decoded_samples);
        return 1;
    }

    float **pcm;
    easyav1_audio_frame *frame = &easyav1->audio.frame;
    unsigned int max_samples = easyav1->settings.max_audio_samples;
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
                    buffer[channel * max_samples + frame->samples + sample] = pcm[channel][sample + pcm_offset];
                }
            }
        }

        vorbis_synthesis_read(&easyav1->audio.vorbis.dsp, decoded_samples);

        frame->samples += decoded_samples;
        if (frame->samples > max_samples) {
            frame->samples = max_samples;
        }

        decoded_samples = vorbis_synthesis_pcmout(&easyav1->audio.vorbis.dsp, &pcm);
        pcm_offset = move_audio_buffer(easyav1, decoded_samples);
    }

    if (frame->samples > 0) {
        easyav1->audio.has_samples_in_buffer = 1;
    }

    return 1;
}

static decoder_function get_decoder_function(easyav1_t *easyav1, unsigned int track, int type)
{
    if (type == NESTEGG_TRACK_VIDEO) {
        if (!easyav1->video.active || track != easyav1->video.track) {
            return 0;
        }
        return decode_video;
    }

    if (type == NESTEGG_TRACK_AUDIO) {
        if (!easyav1->audio.active || track != easyav1->audio.track) {
            return 0;
        }
        return decode_audio;
    }

    log(EASYAV1_LOG_LEVEL_INFO, "Skipping unknown track %u of type %d.", track, type);

    return 0;
}

static int decode_packet(easyav1_t *easyav1)
{
    unsigned int track;

    if (nestegg_packet_track(easyav1->webm.packet, &track)) {
        log(EASYAV1_LOG_LEVEL_ERROR, "Failed to get track number from packet.");
        return 0;
    }

    int track_type = nestegg_track_type(easyav1->webm.context, track);

    if (track_type == -1) {
        log(EASYAV1_LOG_LEVEL_ERROR, "Failed to get track type for track %u.", track);
        return 0;
    }

    decoder_function decode_track = get_decoder_function(easyav1, track, track_type);

    if (!decode_track) {
        return 1;
    }

    int chunks;

    if (nestegg_packet_count(easyav1->webm.packet, &chunks)) {
        log(EASYAV1_LOG_LEVEL_ERROR, "Failed to get packet count");
        return 0;
    }

    for (int chunk = 0; chunk < chunks; chunk++) {
        unsigned char *data;
        size_t size;

        if (nestegg_packet_data(easyav1->webm.packet, chunk, &data, &size)) {
            log(EASYAV1_LOG_LEVEL_ERROR, "Failed to get data from packet");
            return 0;
        }

        if (!decode_track(easyav1, data, size)) {
            return 0;
        }
    }

    return 1;
}

static void release_packet(easyav1_t *easyav1)
{
    if (!easyav1->webm.packet) {
        return;
    }
    nestegg_free_packet(easyav1->webm.packet);
    easyav1->webm.packet = 0;
}

static int decode_cycle(easyav1_t *easyav1)
{
    if (!get_next_packet(easyav1)) {
        return 0;
    }

    int packet_decoded = decode_packet(easyav1);

    easyav1_timestamp current;

    if (nestegg_packet_tstamp(easyav1->webm.packet, &current)) {
        log(EASYAV1_LOG_LEVEL_ERROR, "Failed to get timestamp from packet.");
        return 0;
    }

    easyav1->timestamp = ns_to_ms(current);

    release_packet(easyav1);

    return packet_decoded;
}

int easyav1_decode_next(easyav1_t *easyav1)
{
    if (!easyav1) {
        log(EASYAV1_LOG_LEVEL_INFO, "Handle is NULL");
        return 0;
    }

    if (!decode_cycle(easyav1)) {
        return 0;
    }

    callback_video(easyav1);
    callback_audio(easyav1);

    return 1;
}

int easyav1_decode_until(easyav1_t *easyav1, easyav1_timestamp timestamp)
{
    if (!easyav1) {
        log(EASYAV1_LOG_LEVEL_INFO, "Handle is NULL");
        return 0;
    }

    if (timestamp <= easyav1->timestamp) {
        return 1;
    }

    if (!get_next_packet(easyav1)) {
        return 0;
    }

    easyav1_timestamp current;

    if (nestegg_packet_tstamp(easyav1->webm.packet, &current)) {
        log(EASYAV1_LOG_LEVEL_ERROR, "Failed to get timestamp from packet.");
        return 0;
    }

    // Just update the timestamp if the packet is not yet at the requested timestamp
    if (ns_to_ms(current) > easyav1->timestamp) {
        easyav1->timestamp = timestamp;
        return 1;
    }

    // Skip to timestamp if too far behind
    if (timestamp - easyav1->timestamp > DECODE_UNTIL_SKIP_MS) {
        log(EASYAV1_LOG_LEVEL_INFO, "Decoder too far behind at %llu, skipping to requested timestamp %llu.",
            easyav1->timestamp, timestamp);

        // Leave 30ms difference to get something actually decoded on this run
        easyav1_seek_to_timestamp(easyav1, timestamp - 30);
    }

    while (easyav1->timestamp <= timestamp) {
        if (!decode_cycle(easyav1)) {
            return 0;
        }
    }

    easyav1->timestamp = timestamp;

    callback_video(easyav1);
    callback_audio(easyav1);

    return 1;
}

int easyav1_decode_for(easyav1_t *easyav1, easyav1_timestamp time)
{
    while (time > DECODE_UNTIL_SKIP_MS) {
        if (!easyav1_decode_until(easyav1, easyav1->timestamp + DECODE_UNTIL_SKIP_MS)) {
            return 0;
        }
        time -= DECODE_UNTIL_SKIP_MS;
    }

    return easyav1_decode_until(easyav1, easyav1->timestamp + time);
}


/*
 * Seeking functions
 */

static easyav1_timestamp get_closest_cue_point(const easyav1_t *easyav1, easyav1_timestamp timestamp)
{
    if (!nestegg_has_cues(easyav1->webm.context)) {
        return timestamp;
    }

    unsigned int cluster = 0;
    int64_t start_pos;
    int64_t end_pos;
    easyav1_timestamp cue_timestamp;
    easyav1_timestamp closest = 0;

    do {
        if (nestegg_get_cue_point(easyav1->webm.context, cluster, -1, &start_pos, &end_pos, &cue_timestamp)) {
            log(EASYAV1_LOG_LEVEL_WARNING, "Failed to get cue point %u.", cluster);
            return timestamp;
        }

        if (cue_timestamp >= timestamp) {
            break;
        }

        closest = cue_timestamp;

        cluster++;
    } while (end_pos != -1);

    return closest;
}

int easyav1_seek_to_timestamp(easyav1_t *easyav1, easyav1_timestamp timestamp)
{
    if (!easyav1) {
        log(EASYAV1_LOG_LEVEL_INFO, "Handle is NULL");
        return 0;
    }

    if (timestamp == easyav1->timestamp) {
        return 1;
    }

    easyav1_timestamp duration = easyav1_get_duration(easyav1);

    if (duration && timestamp >= easyav1_get_duration(easyav1)) {
        log(EASYAV1_LOG_LEVEL_INFO, "Requested timestamp is beyond the end of the stream.");
        timestamp = duration;
    }

    easyav1_timestamp old_timestamp = easyav1->timestamp;
    easyav1_timestamp corrected_timestamp = get_closest_cue_point(easyav1, ms_to_ns(timestamp));

    // Only seek the file if the timestamp is at a different cluster or before the current timestamp
    if (corrected_timestamp > easyav1->timestamp || timestamp < easyav1->timestamp) {
        easyav1->timestamp = ns_to_ms(corrected_timestamp);

        unsigned int track = 0;

        if (easyav1->video.active) {
            track = easyav1->video.track;
        } else {
            track = easyav1->audio.track;
        }

        if (nestegg_track_seek(easyav1->webm.context, track, corrected_timestamp)) {
            log(EASYAV1_LOG_LEVEL_ERROR, "Failed to seek to requested timestamp %u.", easyav1->timestamp);
            return 0;
        }
    }

    release_packet(easyav1);

    if (easyav1->video.active) {
        dav1d_picture_unref(&easyav1->video.av1.pic);
        dav1d_flush(easyav1->video.av1.context);
        easyav1->seek = SEEKING_FOR_SQHDR;
        easyav1->video.has_picture_to_output = 0;
    }

    if (easyav1->audio.active) {
        if (easyav1->seek == NOT_SEEKING) {
            easyav1->seek = SEEKING_FOR_KEYFRAME;
        }

        vorbis_synthesis_restart(&easyav1->audio.vorbis.dsp);
        easyav1->audio.has_samples_in_buffer = 0;
    }

    while (easyav1->seek != NOT_SEEKING) {
        if (!decode_cycle(easyav1)) {
            return 0;
        }

        if (easyav1->seek == SEEKING_FOR_TIMESTAMP && easyav1->timestamp >= timestamp) {
            easyav1->seek = NOT_SEEKING;
        }
    }

    log(EASYAV1_LOG_LEVEL_INFO, "Seeked to timestamp %llu from timestamp %llu.", easyav1->timestamp, old_timestamp);

    easyav1->is_finished = 0;

    return 1;
}

int easyav1_seek_forward(easyav1_t *easyav1, easyav1_timestamp time)
{
    if (!easyav1) {
        log(EASYAV1_LOG_LEVEL_INFO, "Handle is NULL");
        return 0;
    }

    return easyav1_seek_to_timestamp(easyav1, easyav1->timestamp + time);
}

int easyav1_seek_backward(easyav1_t *easyav1, easyav1_timestamp time)
{
    if (!easyav1) {
        log(EASYAV1_LOG_LEVEL_INFO, "Handle is NULL");
        return 0;
    }

    if (time > easyav1->timestamp) {
        time = easyav1->timestamp;
    }

    return easyav1_seek_to_timestamp(easyav1, easyav1->timestamp - time);
}


/*
 * Output functions
 */

int easyav1_has_video_frame(const easyav1_t *easyav1)
{
    if (!easyav1) {
        log(EASYAV1_LOG_LEVEL_WARNING, "Handle is NULL");
        return 0;
    }

    return easyav1->video.has_picture_to_output;
}

const easyav1_video_frame *easyav1_get_video_frame(easyav1_t *easyav1)
{
    if (!easyav1) {
        log(EASYAV1_LOG_LEVEL_WARNING, "Handle is NULL");
        return 0;
    }

    if (!easyav1->video.has_picture_to_output) {
        return 0;
    }

    easyav1->video.has_picture_to_output = 0;
    Dav1dPicture *pic = &easyav1->video.av1.pic;

    if (pic->p.layout != DAV1D_PIXEL_LAYOUT_I420) {
        // TODO convert on request or on unsupported type
        dav1d_picture_unref(pic);
        log(EASYAV1_LOG_LEVEL_WARNING, "Unsupported pixel layout");
        return 0;
    }

    easyav1_video_frame frame = {
        .data = {
            pic->data[0],
            pic->data[1],
            pic->data[2]
        },
        .stride = {
            pic->stride[0],
            pic->stride[1],
            pic->stride[1]
        }
    };

    easyav1->video.frame = frame;

    easyav1->video.has_picture_to_output = 0;
    return &easyav1->video.frame;
}

int easyav1_is_audio_buffer_filled(const easyav1_t *easyav1)
{
    if (!easyav1) {
        log(EASYAV1_LOG_LEVEL_WARNING, "Handle is NULL");
        return 0;
    }

    return easyav1->audio.has_samples_in_buffer && easyav1->audio.frame.samples == easyav1->settings.max_audio_samples;
}

const easyav1_audio_frame *easyav1_get_audio_frame(easyav1_t *easyav1)
{
    if (!easyav1) {
        log(EASYAV1_LOG_LEVEL_WARNING, "Handle is NULL");
        return 0;
    }

    if (!easyav1->audio.has_samples_in_buffer) {
        return 0;
    }

    easyav1->audio.has_samples_in_buffer = 0;

    easyav1_audio_frame *frame = &easyav1->audio.frame;

    if (frame->samples == 0) {
        return 0;
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

easyav1_timestamp easyav1_get_current_timestamp(const easyav1_t *easyav1)
{
    if (!easyav1) {
        log(EASYAV1_LOG_LEVEL_WARNING, "Handle is NULL");
        return 0;
    }

    return easyav1->timestamp;
}

int easyav1_has_video_track(const easyav1_t *easyav1)
{
    if (!easyav1 || !easyav1->webm.context) {
        log(EASYAV1_LOG_LEVEL_WARNING, "Handle is NULL");
        return 0;
    }

    return easyav1->video.active;
}

int easyav1_has_audio_track(const easyav1_t *easyav1)
{
    if (!easyav1 || !easyav1->webm.context) {
        log(EASYAV1_LOG_LEVEL_WARNING, "Handle is NULL");
        return 0;
    }

    return easyav1->audio.active;
}

unsigned int easyav1_get_video_width(const easyav1_t *easyav1)
{
    if (!easyav1 || !easyav1->webm.context) {
        log(EASYAV1_LOG_LEVEL_WARNING, "Handle is NULL");
        return 0;
    }

    return easyav1->video.active ? easyav1->video.width : 0;
}

unsigned int easyav1_get_video_height(const easyav1_t *easyav1)
{
    if (!easyav1 || !easyav1->webm.context) {
        log(EASYAV1_LOG_LEVEL_WARNING, "Handle is NULL");
        return 0;
    }

    return easyav1->video.active ? easyav1->video.height : 0;
}

unsigned int easyav1_get_audio_channels(const easyav1_t *easyav1)
{
    if (!easyav1 || !easyav1->webm.context) {
        log(EASYAV1_LOG_LEVEL_WARNING, "Handle is NULL");
        return 0;
    }

    return easyav1->audio.active ? easyav1->audio.channels : 0;
}

unsigned int easyav1_get_audio_sample_rate(const easyav1_t *easyav1)
{
    if (!easyav1 || !easyav1->webm.context) {
        log(EASYAV1_LOG_LEVEL_WARNING, "Handle is NULL");
        return 0;
    }

    return easyav1->audio.active ? easyav1->audio.sample_rate : 0;
}

easyav1_timestamp easyav1_get_duration(const easyav1_t *easyav1)
{
    if (!easyav1 || !easyav1->webm.context) {
        log(EASYAV1_LOG_LEVEL_WARNING, "Handle is NULL");
        return 0;
    }

    easyav1_timestamp duration;

    if (nestegg_duration(easyav1->webm.context, &duration)) {
        log(EASYAV1_LOG_LEVEL_ERROR, "Failed to get duration");
        return 0;
    }

    return ns_to_ms(duration);
}

int easyav1_is_finished(const easyav1_t *easyav1)
{
    if (!easyav1) {
        log(EASYAV1_LOG_LEVEL_WARNING, "Handle is NULL");
        return 0;
    }

    return easyav1->is_finished;
}


/*
 * Destruction functions
 */

void easyav1_destroy(easyav1_t **handle)
{
    easyav1_t *easyav1 = 0;
    if (!handle) {
        log(EASYAV1_LOG_LEVEL_INFO, "Handle is NULL");
        return;
    }
    easyav1 = *handle;
    if (!easyav1) {
        log(EASYAV1_LOG_LEVEL_INFO, "Handle is NULL");
        return;
    }

    if (easyav1->video.has_picture_to_output) {
        dav1d_picture_unref(&easyav1->video.av1.pic);
    }
    if (easyav1->video.av1.context) {
        dav1d_close(&easyav1->video.av1.context);
    }

    vorbis_block_clear(&easyav1->audio.vorbis.block);
    vorbis_dsp_clear(&easyav1->audio.vorbis.dsp);
    vorbis_info_clear(&easyav1->audio.vorbis.info);
    free(easyav1->audio.buffer);
    if (!easyav1->settings.interlace_audio) {
        free(easyav1->audio.frame.pcm.deinterlaced);
    }

    if (easyav1->webm.context) {
        nestegg_destroy(easyav1->webm.context);
    }

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
    *handle = 0;
}
