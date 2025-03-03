#ifndef EASYAV1_H
#define EASYAV1_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EASYAV1_VIDEO_CONVERSION_NONE = 0,
    EASYAV1_VIDEO_CONVERSION_RGBA,
    EASYAV1_VIDEO_CONVERSION_ARGB
} easyav1_video_conversion;

typedef enum {
    EASYAV1_LOG_LEVEL_ERROR,
    EASYAV1_LOG_LEVEL_WARNING,
    EASYAV1_LOG_LEVEL_INFO
} easyav1_log_level_t;

typedef int(*easyav1_read_func)(void *buffer, size_t size, void *userdata);
typedef int(*easyav1_seek_func)(int64_t offset, int origin, void *userdata);
typedef int64_t(*easyav1_tell_func)(void *userdata);

typedef uint64_t easyav1_timestamp;
typedef struct easyav1_t easyav1_t;

typedef struct {
    easyav1_read_func read_func;
    easyav1_seek_func seek_func;
    easyav1_tell_func tell_func;

    void *userdata;
} easyav1_stream;

typedef struct {
    const void *data[3];
    size_t stride[3];
} easyav1_video_frame;

typedef struct {
    unsigned int channels;
    unsigned int samples;
    size_t bytes;
    union {
        const float **deinterlaced;
        const float *interlaced;
    } pcm;
} easyav1_audio_frame;

typedef void(*easyav1_video_callback)(const easyav1_video_frame *frame, void *userdata);
typedef void(*easyav1_audio_callback)(const easyav1_audio_frame *frame, void *userdata);

typedef struct {
    int enable_video;
    int enable_audio;
    int skip_unprocessed_frames;
    int interlace_audio;
    easyav1_video_conversion video_conversion;
    int close_handle_on_destroy;
    struct {
        easyav1_video_callback video;
        easyav1_audio_callback audio;
        void *userdata;
    } callbacks;
    unsigned int video_track;
    unsigned int audio_track;
    unsigned int max_audio_samples;
    easyav1_log_level_t log_level;
} easyav1_settings;

easyav1_settings easyav1_default_settings(void);
easyav1_t *easyav1_init_from_filename(const char *filename, const easyav1_settings *settings);
easyav1_t *easyav1_init_from_file(FILE *f, const easyav1_settings *settings);
easyav1_t *easyav1_init_from_memory(const void *data, size_t size, const easyav1_settings *settings);
easyav1_t *easyav1_init_from_custom_stream(const easyav1_stream *stream, const easyav1_settings *settings);

int easyav1_decode_next(easyav1_t *easyav1);
int easyav1_decode_until(easyav1_t *easyav1, easyav1_timestamp timestamp);
int easyav1_decode_for(easyav1_t *easyav1, easyav1_timestamp time);

int easyav1_seek_forward(easyav1_t *easyav1, easyav1_timestamp time);
int easyav1_seek_backward(easyav1_t *easyav1, easyav1_timestamp time);
int easyav1_seek_to_timestamp(easyav1_t *easyav1, easyav1_timestamp timestamp);

easyav1_timestamp easyav1_get_current_timestamp(const easyav1_t *easyav1);

int easyav1_has_video_track(const easyav1_t *easyav1);
int easyav1_has_audio_track(const easyav1_t *easyav1);

unsigned int easyav1_get_video_width(const easyav1_t *easyav1);
unsigned int easyav1_get_video_height(const easyav1_t *easyav1);

unsigned int easyav1_get_audio_channels(const easyav1_t *easyav1);
unsigned int easyav1_get_audio_sample_rate(const easyav1_t *easyav1);


/*
 * Indicates whether there is a video frame ready to be displayed.
 *
 * @param easyav1 The easyav1 instance.
 *
 * @return 1 if a video frame is available, 0 otherwise.
 */
int easyav1_has_video_frame(const easyav1_t *easyav1);

/*
 * Gets the current video frame, is one is available.
 *
 * The frame is only valid until the next call to `easyav1_decode_next` or `easyav1_decode_until`.
 * Calling this function will mark the frame as displayed, so you will only receive a decoded frame once.
 *
 * @param easyav1 The easyav1 instance.
 *
 * @return A pointer to the video frame, or NULL if no frame is available.
 */
const easyav1_video_frame *easyav1_get_video_frame(easyav1_t *easyav1);

/*
 * Indicates whether the audio buffer is filled (i.e. the queued samples equal the `max_audio_samples` setting).
 *
 * @param easyav1 The easyav1 instance.
 *
 * @return 1 if the audio buffer is filled, 0 otherwise.
 */
int easyav1_is_audio_buffer_filled(const easyav1_t *easyav1);

/*
 * Gets the current audio frame, if there are available samples.
 *
 * The frame is only valid until the next call to `easyav1_decode_next` or `easyav1_decode_until`.
 * Calling this function will mark all the samples in the frame as played, so you will only receive the samples once.
 *
 * @param easyav1 The easyav1 instance.
 *
 * @return A pointer to the audio frame, or NULL if there are no samples to process.
 */
const easyav1_audio_frame *easyav1_get_audio_frame(easyav1_t *easyav1);

easyav1_timestamp easyav1_get_duration(const easyav1_t *easyav1);
int easyav1_is_finished(const easyav1_t *easyav1);

void easyav1_destroy(easyav1_t **easyav1);

#ifdef __cplusplus
}


/*
 * C++ wrapper
 */
class EasyAV1 {
    public:

        Easyav1::Easyav1(const char *filename, const easyav1_settings *settings)
        {
            this->m_stream = nullptr;
            this->m_easyav1 = easyav1_init_from_filename(filename, settings);
        }

        Easyav1::Easyav1(FILE *f, const easyav1_settings *settings)
        {
            this->m_stream = nullptr;
            this->m_easyav1 = easyav1_init_from_file(f, settings);
        }

        Easyav1::Easyav1(const void *data, size_t size, const easyav1_settings *settings)
        {
            this->m_stream = nullptr;
            this->m_easyav1 = easyav1_init_from_memory(data, size, settings);
        }

        Easyav1::Easyav1(const easyav1_stream *stream, const easyav1_settings *settings)
        {
            this->m_stream = nullptr;
            this->m_easyav1 = easyav1_init_from_custom_stream(stream, settings);
        }

        Easyav1::Easyav1(EasyAV1IO &io, const easyav1_settings *settings)
        {
            this->m_easyav1 = easyav1_init_from_custom_stream(&stream, settings);
        }

        ~Easyav1() { easyav1_destroy(&this->m_easyav1); }

        // TODO all remaining functions

        bool has_valid_instance() {
            return this->m_easyav1 != nullptr;
        }

    private:
        easyav1_t *m_easyav1;

        Easyav1::Easyav1() : m_easyav1(nullptr) {}
};
#endif

#endif // EASYAV1_H
