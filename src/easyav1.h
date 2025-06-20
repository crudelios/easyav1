/**
 * easyav1 - A simple, easy-to-use AV1 decoder.
 *
 * Created by José Cadete (crudelios) and licensed under the BSD 3-Clause License. See LICENSE for details.
 *
 * This library provides a simple interface for decoding AV1 video and Vorbis audio from WebM files.
 *
 * Internally it uses the nestegg library for WebM parsing, dav1d for AV1 decoding, and minivorbis for Vorbis decoding.
 *
 * The library is designed to be easy to use and to provide a simple interface for decoding AV1 video and Vorbis audio.
 *
 * Basic example usage:
 *
 * @code{.c}
 * #include <easyav1.h>
 *
 * int main(int argc, char **argv)
 * {
 *     easyav1_t *easyav1 = easyav1_init_from_filename("video.webm", NULL);
 *
 *     if (!easyav1) {
 *         printf("Failed to initialize easyav1.\n");
 *         return 1;
 *     }
 *
 *     if (easyav1_has_video_track(easyav1) == EASYAV1_TRUE) {
 *         printf("Video size: %ux%u\n", easyav1_get_video_width(easyav1), easyav1_get_video_height(easyav1));
 *     }
 *
 *     if (easyav1_has_audio_track(easyav1) == EASYAV1_TRUE) {
 *         printf("Audio sample rate: %u\n", easyav1_get_audio_sample_rate(easyav1));
 *         printf("Audio channels: %u\n", easyav1_get_audio_channels(easyav1));
 *     }
 *
 *     while (easyav1_decode_next(easyav1) == EASYAV1_STATUS_OK) {
 *          if (easyav1_has_video_frame(easyav1) == EASYAV1_TRUE) {
 *             const easyav1_video_frame *video_frame = easyav1_get_video_frame(easyav1);
 *             // Do something with the video frame.
 *         }
 *
 *         if (easyav1_has_audio_frame(easyav1) == EASYAV1_TRUE) {
 *             const easyav1_audio_frame *audio_frame = easyav1_get_audio_frame(easyav1);
 *             // Do something with the audio frame.
 *         }
 *     }
 *
 *     if (easyav1_is_finished(easyav1) == EASYAV1_TRUE) {
 *         printf("Finished decoding.\n");
 *     } else {
 *         printf("Failed to decode.\n");
 *     }
 *
 *     easyav1_destroy(&easyav1);
 *
 *     return 0;
 * }
 * @endcode
 */

#ifndef EASYAV1_H
#define EASYAV1_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * easyav1 instance.
 *
 * This is the main structure that holds the state of the easyav1 instance. This is an opaque pointer.
 */
typedef struct easyav1_t easyav1_t;


/**
 * Timestamp type. This is used for all timestamp related operations.
 */
typedef uint64_t easyav1_timestamp;


/**
 * Boolean enumeration.
 */
typedef enum {
    EASYAV1_FALSE = 0,
    EASYAV1_TRUE = 1
} easyav1_bool;


/**
 * Decoder status, either returned by specific functions or by `easyav1_get_status`.
 */
typedef enum {
    // The values below are returned by functions, with `EASYAV1_STATUS_FINISHED` being returned only by `easyav1_decode_*`.
    EASYAV1_STATUS_ERROR = 0,
    EASYAV1_STATUS_OK = 1,
    EASYAV1_STATUS_FINISHED = 2,

    // The values below are only returned by `easyav1_get_status`
    EASYAV1_STATUS_INVALID_ARGUMENT = -1,
    EASYAV1_STATUS_OUT_OF_MEMORY = -2,
    EASYAV1_STATUS_IO_ERROR = -3,
    EASYAV1_STATUS_DECODER_ERROR = -4,
    EASYAV1_STATUS_NOT_IMPLEMENTED = -5,
    EASYAV1_STATUS_INVALID_STATE = -6,
    EASYAV1_STATUS_INVALID_DATA = -7,
    EASYAV1_STATUS_UNSUPPORTED = -8
} easyav1_status;


/**
 * @brief A function that reads data from a source.
 *
 * @param buffer The buffer to read the data into.
 * @param size The size of the buffer.
 * @param userdata Custom optional user-defined data.
 *
 * @return `1` if `size` bytes were read, `0` if the end of the stream was reached, `-1` if there was an error.
 */
typedef int(*easyav1_read_func)(void *buffer, size_t size, void *userdata);


/**
 * @brief A function that seeks to a specific position in a source.
 *
 * @param offset The offset to seek to.
 * @param origin The origin of the seek operation:
 *
 *   - `SEEK_SET` sets the pointer to `offset` bytes after the start of the buffer.
 *
 *   - `SEEK_CUR` sets the pointer to `offset` bytes after the current position.
 *
 *   - `SEEK_END` sets the pointer to `offset` bytes before the end of the buffer.
 *
 * @param userdata Custom optional user-defined data.
 *
 * @return `0` if the seek was successful, another value if there was an error.
 */
typedef int(*easyav1_seek_func)(int64_t offset, int origin, void *userdata);


/**
 * @brief A function that returns the current position in a source.
 *
 * @param userdata Custom optional user-defined data.
 *
 * @return The current position in the source, or `-1` if there was an error.
 */
typedef int64_t(*easyav1_tell_func)(void *userdata);


/**
 * @brief Custom stream structure for loading data from a source.
 *
 * You'll need to provide the following functions:
 *
 * - `read_func`: A function that reads data from the stream.
 *
 * - `seek_func`: A function that seeks to a specific position in the stream.
 *
 * - `tell_func`: A function that returns the current position in the stream.
 *
 * @note The `userdata` field is a pointer to an optional user-defined data that will be passed to the read, seek,
 * and tell functions.
 */
typedef struct {
    easyav1_read_func read_func;
    easyav1_seek_func seek_func;
    easyav1_tell_func tell_func;

    void *userdata;
} easyav1_stream;


/**
 * Video pixel layouts.
 */
typedef enum {
    EASYAV1_PIXEL_LAYOUT_UNKNOWN = 0,
    EASYAV1_PIXEL_LAYOUT_YUV400 = 1,
    EASYAV1_PIXEL_LAYOUT_YUV420 = 2,
    EASYAV1_PIXEL_LAYOUT_YUV422 = 3,
    EASYAV1_PIXEL_LAYOUT_YUV444 = 4
} easyav1_pixel_layout;


/**
 * Video color range.
 */
typedef enum {
    EASYAV1_COLOR_SPACE_UNKNOWN = 0,
    EASYAV1_COLOR_SPACE_LIMITED = 1,
    EASYAV1_COLOR_SPACE_FULL = 2
} easyav1_color_space;


/**
 * Bits per color.
 */
typedef enum {
    EASYAV1_BITS_PER_COLOR_UNKNOWN = 0,
    EASYAV1_BITS_PER_COLOR_8 = 1,
    EASYAV1_BITS_PER_COLOR_10 = 2,
    EASYAV1_BITS_PER_COLOR_12 = 3
} easyav1_bits_per_color;


/**
 * Video color primaries.
 */
typedef enum {
    EASYAV1_COLOR_PRIMARIES_UNSPECIFIED = 0,
    EASYAV1_COLOR_PRIMARIES_BT709 = 1,
    EASYAV1_COLOR_PRIMARIES_UNKNOWN = 2,
    EASYAV1_COLOR_PRIMARIES_BT470M = 3,
    EASYAV1_COLOR_PRIMARIES_BT470BG = 4,
    EASYAV1_COLOR_PRIMARIES_BT601 = 5,
    EASYAV1_COLOR_PRIMARIES_SMPTE240 = 6,
    EASYAV1_COLOR_PRIMARIES_FILM = 7,
    EASYAV1_COLOR_PRIMARIES_BT2020 = 8,
    EASYAV1_COLOR_PRIMARIES_XYZ = 9,
    EASYAV1_COLOR_PRIMARIES_SMPTE431 = 10,
    EASYAV1_COLOR_PRIMARIES_SMPTE432 = 11,
    EASYAV1_COLOR_PRIMARIES_EBU3213 = 12
} easyav1_color_primaries;


/**
 * Video transfer characteristics. 
 */
typedef enum {
    EASYAV1_TRANSFER_CHARACTERISTICS_UNSPECIFIED = 0,
    EASYAV1_TRANSFER_CHARACTERISTICS_BT709 = 1,
    EASYAV1_TRANSFER_CHARACTERISTICS_UNKNOWN = 2,
    EASYAV1_TRANSFER_CHARACTERISTICS_BT470M = 3,
    EASYAV1_TRANSFER_CHARACTERISTICS_BT470BG = 4,
    EASYAV1_TRANSFER_CHARACTERISTICS_BT601 = 5,
    EASYAV1_TRANSFER_CHARACTERISTICS_SMPTE240 = 6,
    EASYAV1_TRANSFER_CHARACTERISTICS_LINEAR = 7,
    EASYAV1_TRANSFER_CHARACTERISTICS_LOG_100 = 8,
    EASYAV1_TRANSFER_CHARACTERISTICS_LOG_100_SQRT = 9,
    EASYAV1_TRANSFER_CHARACTERISTICS_IEC61966 = 10,
    EASYAV1_TRANSFER_CHARACTERISTICS_BT1361 = 11,
    EASYAV1_TRANSFER_CHARACTERISTICS_SRGB = 12,
    EASYAV1_TRANSFER_CHARACTERISTICS_BT2020_10 = 13,
    EASYAV1_TRANSFER_CHARACTERISTICS_BT2020_12 = 14,
    EASYAV1_TRANSFER_CHARACTERISTICS_SMPTE2084 = 15,
    EASYAV1_TRANSFER_CHARACTERISTICS_SMPTE428 = 16,
    EASYAV1_TRANSFER_CHARACTERISTICS_HLG = 17
} easyav1_transfer_characteristics;


/**
 * Video matrix coefficients.
 */
typedef enum {
    EASYAV1_MATRIX_COEFFICIENTS_UNSPECIFIED = 0,
    EASYAV1_MATRIX_COEFFICIENTS_IDENTITY = 1,
    EASYAV1_MATRIX_COEFFICIENTS_BT709 = 2,
    EASYAV1_MATRIX_COEFFICIENTS_UNKONWN = 3,
    EASYAV1_MATRIX_COEFFICIENTS_FCC = 4,
    EASYAV1_MATRIX_COEFFICIENTS_BT470BG = 5,
    EASYAV1_MATRIX_COEFFICIENTS_BT601 = 6,
    EASYAV1_MATRIX_COEFFICIENTS_SMPTE240 = 7,
    EASYAV1_MATRIX_COEFFICIENTS_SMPTE_YCGCO = 8,
    EASYAV1_MATRIX_COEFFICIENTS_BT2020_NCL = 9,
    EASYAV1_MATRIX_COEFFICIENTS_BT2020_CL = 10,
    EASYAV1_MATRIX_COEFFICIENTS_SMPTE2085 = 11,
    EASYAV1_MATRIX_COEFFICIENTS_CHROMATICITY_NCL = 12,
    EASYAV1_MATRIX_COEFFICIENTS_CHROMATICITY_CL = 13,
    EASYAV1_MATRIX_COEFFICIENTS_ICTCP = 14
} easyav1_matrix_coefficients;

typedef enum {
    EASYAV1_CHROMA_SAMPLE_POSITION_UNKNOWN = 0,
    EASYAV1_CHROMA_SAMPLE_POSITION_VERTICAL = 1,
    EASYAV1_CHROMA_SAMPLE_POSITION_COLOCATED = 2
} easyav1_chroma_sample_position;


/**
 * Video frame.
 */
typedef struct {
    easyav1_pixel_layout pixel_layout;                         // The pixel layout.
    easyav1_bits_per_color bits_per_color;                     // The bits per color.
    easyav1_color_space color_space;                           // The color space.
    easyav1_color_primaries color_primaries;                   // The color primaries.
    easyav1_transfer_characteristics transfer_characteristics; // The transfer characteristics.
    easyav1_matrix_coefficients matrix_coefficients;           // The matrix coefficients.
    easyav1_chroma_sample_position chroma_sample_position;     // The chroma sample position.
    unsigned int width;                                        // The width of the frame.
    unsigned int height;                                       // The height of the frame.
    easyav1_timestamp timestamp;                               // The timestamp of the frame.
    const void *data[3];                                       // The data for each YUV plane.
    size_t stride[3];                                          // The stride for each YUV plane.
} easyav1_video_frame;


/**
 * Audio frame.
 */
typedef struct {
    unsigned int channels; // Number of channels.
    unsigned int samples;  // Number of samples in this frame.
    easyav1_timestamp timestamp; // The timestamp of the frame.
    size_t bytes; // Number of bytes in this frame. This is equal to `samples * sizeof(float) * channels` if
                  // `interlace_audio` is 1, and `samples * sizeof(float)` if `interlace_audio` is 0.
    union {
        const float **deinterlaced; // Deinterlaced audio samples.
        const float *interlaced;    // Interlaced audio samples.
    } pcm; // The PCM samples. This is either deinterlaced or interlaced depending on the `interlace_audio` setting.
} easyav1_audio_frame;

/**
 * Video and audio callbacks.
 */
typedef void(*easyav1_video_callback)(const easyav1_video_frame *frame, void *userdata);
typedef void(*easyav1_audio_callback)(const easyav1_audio_frame *frame, void *userdata);


/**
 * Log levels.
 */
typedef enum {
    EASYAV1_LOG_LEVEL_ERROR,
    EASYAV1_LOG_LEVEL_WARNING,
    EASYAV1_LOG_LEVEL_INFO
} easyav1_log_level_t;


/**
 * @brief Settings for the easyav1 instance.
 *
 * Before initializing the `easyav1` instance, you should  get the default settings by calling
 * `easyav1_default_settings`. You can then modify the settings as needed.
 *
 * The settings are:
 *
 * - `enable_video`: Indicates whether video decoding is enabled.
 *
 * - `enable_audio`: Indicates whether audio decoding is enabled.
 *
 * - `skip_unprocessed_frames`: Indicates whether unprocessed frames should be skipped.
 *    If this is set to `EASYAV1_TRUE`, the decoder will skip frames that have not been processed by the video callback.
 *
 * - `interlace_audio`: Indicates whether audio should be interlaced.
 *    If this is set to `EASYAV1_TRUE`, the audio samples will be interleaved. If it is set to `EASYAV1_FALSE`,
 *    the samples will be deinterleaved.
 *
 *    Interleaved audio can be obtained by calling `easyav1_get_audio_frame` and accessing the `pcm.interlaced` field
 *    and deinterleaved audio can be obtained by calling `easyav1_get_audio_frame` and then accessing the
 *    `pcm.deinterlaced` field. Do note, though, that you can only use the appropriate field depending on the
 *    `interlace_audio` setting.
 *
 * - `close_handle_on_destroy`: Indicates whether the handle should be closed on destroy.
 *
 *     If this is set to `EASYAV1_TRUE`, the handle will be closed when calling `easyav1_destroy`.
 *     If it is set to `EASYAV1_FALSE`, the handle will not be closed.
 *     The handle will either be the `FILE` handle (if the instance was initialized from such a handle using
 *     `easyav1_init_from_file`) or the memory buffer (if the instance was initialized from `easyav1_init_from_memory`).
 *
 * - `callbacks`: The callbacks to use for video and audio.
 *
 *   - `video`: The video callback to use. If this is set to `NULL`, no video callback will be used.
 *      The format of the callback should be: `void callback(const easyav1_video_frame *frame, void *userdata)`.
 *
 *   - `audio`: The audio callback to use. If this is set to `NULL`, no audio callback will be used.
 *      The format of the callback should be: `void callback(const easyav1_audio_frame *frame, void *userdata)`.
 *
 *   - `userdata`: The userdata to pass to the callbacks.
 *
 * - `video_track`: The video track to use. The track is 0-indexed and based only on the video tracks in the file:
 *    if you have a video track, and audio track, and another video track, the video tracks will be 0 and 1.
 *
 * - `audio_track`: The audio track to use. The track is 0-indexed and based only on the audio tracks in the file.
 *   if you have a video track, and audio track, and another video track, the audio track will be 0.
 *
 * - `max_audio_samples`: The maximum number of audio samples.
 *
 * - `use_fast_seeking`: Indicates whether fast seeking should be used. If fast seeking is enabled, easyav1 will seek
 *    to the nearest keyframe before the requested timestamp. Otherwise it will seek to the requested timestamp, which
 *    can be slower as all frames between the nearest keyframe and the requested timestamp will need to be decoded.
 *
 * - `audio_offset_time`: The audio offset time in relation to video, in milliseconds.
 *
 *    If negative, this will make the audio play earlier than video by the specified amount.
 *
 *    If positive, audio will play later in relation to video.
 *    This can be useful if the audio and video are out of sync, for example due to audio buffering.
 *
 *    If using SDL2 for audio, you should set the adjustment to `(SDL_AudioSpec.samples / SDL_AudioSpec.freq)`.
 *    Note: The values set in the settings will also be offset by the webm's internal audio delay value.
 *
 * - `log_level`: The log level to use.
 *
 *     This can be one of the following:
 *
 *     - `EASYAV1_LOG_LEVEL_ERROR`: Only errors are logged.
 *
 *     - `EASYAV1_LOG_LEVEL_WARNING`: Errors and warnings are logged.
 *
 *     - `EASYAV1_LOG_LEVEL_INFO`: Errors, warnings, and info messages are logged. This is most useful for debugging
 *        purposes.
 */
typedef struct {
    easyav1_bool enable_video;
    easyav1_bool enable_audio;
    easyav1_bool skip_unprocessed_frames;
    easyav1_bool interlace_audio;
    easyav1_bool close_handle_on_destroy;
    struct {
        easyav1_video_callback video;
        easyav1_audio_callback audio;
        void *userdata;
    } callbacks;
    unsigned int video_track;
    unsigned int audio_track;
    easyav1_bool use_fast_seeking;
    int64_t audio_offset_time;
    easyav1_log_level_t log_level;
} easyav1_settings;

/**
 * @brief Returns the default settings for easyav1.
 *
 * The default settings are:
 *
 * - Video enabled (`.enable_video = EASYAV1_TRUE`)
 * - Audio enabled (`.enable_audio = EASYAV1_TRUE`)
 * - Skip unprocessed frames (`.skip_unprocessed_frames = EASYAV1_TRUE`)
 * - Interlace audio (`.interlace_audio = EASYAV1_TRUE`)
 * - Don't close the handle on destroy (`.close_handle_on_destroy = EASYAV1_FALSE`)
 * - No callbacks (`callbacks.video = NULL, callbacks.audio = NULL, callbacks.userdata = NULL`)
 * - Video track 0 (`.video_track = 0`)
 * - Audio track 0 (`.audio_track = 0`)
 * - Don't use fast seeking (`.use_fast_seeking = EASYAV1_FALSE`)
 * - No audio offset time (`.audio_offset_time = 0`)
 * - Log level warning (`.log_level = EASYAV1_LOG_LEVEL_WARNING`)
 *
 * @return The default settings.
 */
easyav1_settings easyav1_default_settings(void);


/**
 * @brief Initializes an easyav1 instance from a file.
 *
 * @param filename The filename of the file to open.
 * @param settings The settings to use for the easyav1 instance. If this is `NULL`, the default settings will be used.
 *
 * @return The `easyav1` instance, or `NULL` if an error occurred.
 */
easyav1_t *easyav1_init_from_filename(const char *filename, const easyav1_settings *settings);


/**
 * @brief Initializes an easyav1 instance from a `FILE` handle.
 *
 * @param f The open `FILE` handle.
 * @param settings The settings to use for the easyav1 instance. If this is `NULL`, the default settings will be used.
 *
 * @return The `easyav1` instance, or `NULL` if an error occurred.
 */
easyav1_t *easyav1_init_from_file(FILE *f, const easyav1_settings *settings);


/**
 * @brief Initializes an easyav1 instance from a memory buffer.
 *
 * @param data The buffer to read from.
 * @param size The size of the buffer.
 * @param settings The settings to use for the easyav1 instance. If this is `NULL`, the default settings will be used.
 *
 * @return The `easyav1` instance, or `NULL` if an error occurred.
 */
easyav1_t *easyav1_init_from_memory(const void *data, size_t size, const easyav1_settings *settings);


/**
 * @brief Initializes an easyav1 instance from a custom stream.
 *
 * @param stream The custom stream to read from. Please refer to the `easyav1_stream` struct for more information.
 * @param settings The settings to use for the easyav1 instance. If this is NULL, the default settings will be used.
 *
 * @return The `easyav1` instance, or `NULL` if an error occurred.
 */
easyav1_t *easyav1_init_from_custom_stream(const easyav1_stream *stream, const easyav1_settings *settings);


/**
 * @brief Decodes the next packet.
 *
 * @param easyav1 The easyav1 instance.
 * @return `EASYAV1_STATUS_OK` if a packet was decoded, `EASYAV1_STATUS_FINISHED` if the end of the stream was reached
 *          or `EASYAV1_STATUS_ERROR` if there was an error.
 */
easyav1_status easyav1_decode_next(easyav1_t *easyav1);


/**
 * @brief Decodes the next packets until the specified timestamp.
 *
 * @param easyav1 The easyav1 instance.
 * @param timestamp The timestamp to decode until.
 *
 * @return `EASYAV1_STATUS_OK` if a packet was decoded, `EASYAV1_STATUS_FINISHED` if the end of the stream was reached
 *          or `EASYAV1_STATUS_ERROR` if there was an error.
 */
easyav1_status easyav1_decode_until(easyav1_t *easyav1, easyav1_timestamp timestamp);


/**
 * @brief Decodes the next packets that appear in the specified duration.
 *
 * @param easyav1 The easyav1 instance.
 * @param time The duration to decode for.
 *
 * @return `EASYAV1_STATUS_OK` if a packet was decoded, `EASYAV1_STATUS_FINISHED` if the end of the stream was reached
 *          or `EASYAV1_STATUS_ERROR` if there was an error.
 */
easyav1_status easyav1_decode_for(easyav1_t *easyav1, easyav1_timestamp time);


/**
 * @brief Starts playing the video and audio.
 *
 * This function will start decoding the video and audio in the background, syncing with the elapsed time.
 * If the video and audio callbacks are set, they will be called, otherwise you must get video and audio from
 * `easyav1_get_video_frame` and `easyav1_get_audio_frame` respectively.
 * 
 * This function will not block, it will return immediately after starting the decoding process.
 * 
 * You must call this function after initializing the easyav1 instance and setting the callbacks.
 * 
 * @note This function runs on its own thread, so you must ensure that the callbacks are thread-safe.
 *
 * @note If you wish to seek while playing, you should pause the playback by calling `easyav1_stop` and then call
 *       `easyav1_seek_to_timestamp` or `easyav1_seek_forward`/`easyav1_seek_backward`.
 *
 * @param easyav1 The easyav1 instance.
 *
 * @return `EASYAV1_STATUS_OK` if successful, `EASYAV1_STATUS_ERROR` if there was an error.
 */
easyav1_status easyav1_play(easyav1_t *easyav1);


/**
 * @brief Stops the playback of the video and audio.
 *
 * This function will stop the decoding process and will not call the video or audio callbacks anymore.
 * It will also stop the background thread that is decoding the video and audio.
 * 
 * You can resume playback by calling `easyav1_play` again.
 *
 * @param easyav1 The easyav1 instance.
 */
void easyav1_stop(easyav1_t *easyav1);


/**
 * @brief Seeks forward by the specified duration.
 *
 * @param easyav1 The easyav1 instance.
 * @param time The duration to seek forward by.
 *
 * @return `EASYAV1_STATUS_OK` if successful, `EASYAV1_STATUS_ERROR` if there was an error.
 */
easyav1_status easyav1_seek_forward(easyav1_t *easyav1, easyav1_timestamp time);


/**
 * @brief Seeks backward by the specified duration.
 *
 * @param easyav1 The easyav1 instance.
 * @param time The duration to seek backward by.
 *
 * @return `EASYAV1_STATUS_OK` if successful, `EASYAV1_STATUS_ERROR` if there was an error.
 */
easyav1_status easyav1_seek_backward(easyav1_t *easyav1, easyav1_timestamp time);


/**
 * @brief Seeks to a specified timestamp.
 *
 * @param easyav1 The easyav1 instance.
 * @param timestamp The timestamp to seek to.
 *
 * @return `EASYAV1_STATUS_OK` if successful, `EASYAV1_STATUS_ERROR` if there was an error.
 */
easyav1_status easyav1_seek_to_timestamp(easyav1_t *easyav1, easyav1_timestamp timestamp);


/**
 * @brief Indicates the current status of the easyav1 instance.
 *
 * @param easyav1 The easyav1 instance.
 *
 * @return The current status of the easyav1 instance.
 *
 * The status can be one of the following:
 *
 * - `EASYAV1_STATUS_OK`: Everything is fine.
 *
 * - `EASYAV1_STATUS_FINISHED`: The end of the stream has been reached.
 *
 * - `EASYAV1_STATUS_ERROR`: There was an error.
 */
easyav1_status easyav1_get_status(easyav1_t *easyav1);

/**
 * @brief Gets the current timestamp of the parsing.
 *
 * @param easyav1 The easyav1 instance.
 *
 * @return The current timestamp.
 */
easyav1_timestamp easyav1_get_current_timestamp(easyav1_t *easyav1);


/**
 * @brief Indicates whether there is a video track in the file.
 *
 * @param easyav1 The easyav1 instance.
 *
 * @return `EASYAV1_TRUE` if there is a video track, `EASYAV1_FALSE` otherwise.
 */
easyav1_bool easyav1_has_video_track(const easyav1_t *easyav1);


/**
 * @brief Indicates whether there is an audio track in the file.
 *
 * @param easyav1 The easyav1 instance.
 *
 * @return `EASYAV1_TRUE` if there is an audio track, `EASYAV1_TRUE` otherwise.
 */
easyav1_bool easyav1_has_audio_track(const easyav1_t *easyav1);


/**
 * @brief Gets the total number of video tracks in the file.
 *
 * @param easyav1 The easyav1 instance.
 *
 * @return The total number of video tracks in the file or `0` if there was an error or there are no video tracks.
 *
 */
unsigned int easyav1_get_total_video_tracks(const easyav1_t *easyav1);


/**
 * @brief Gets the total number of audio tracks in the file.
 *
 * @param easyav1 The easyav1 instance.
 *
 * @return The total number of audio tracks in the file or `0` if there was an error or there are no audio tracks.
 */
unsigned int easyav1_get_total_audio_tracks(const easyav1_t *easyav1);


/**
 * @brief Gets the width of the video track.
 *
 * @param easyav1 The easyav1 instance.
 *
 * @return The width of the video track, `0` if there was an error or there is no video track.
 */
unsigned int easyav1_get_video_width(const easyav1_t *easyav1);


/**
 * @brief Gets the height of the video track.
 *
 * @param easyav1 The easyav1 instance.
 *
 * @return The height of the video track, or `0` if there was an error or there is no video track.
 */
unsigned int easyav1_get_video_height(const easyav1_t *easyav1);


/**
 * @brief Gets the FPS of the video track.
 *
 * @param easyav1 The easyav1 instance.
 *
 * @return The FPS of the video track, or `0` if there was an error or there is no video track.
 */
unsigned int easyav1_get_video_fps(const easyav1_t *easyav1);


/**
 * @brief Gets the number of audio channels.
 *
 * @param easyav1 The easyav1 instance.
 *
 * @return The number of audio channels, or `0` if there was an error or there is no audio track.
 */
unsigned int easyav1_get_audio_channels(const easyav1_t *easyav1);


/**
 * @brief Gets the audio sample rate.
 *
 * @param easyav1 The easyav1 instance.
 *
 * @return The audio sample rate, or `0` if there was an error or there is no audio track.
 */
unsigned int easyav1_get_audio_sample_rate(const easyav1_t *easyav1);


/**
 * @brief Indicates whether there is a video frame ready to be displayed.
 *
 * @param easyav1 The easyav1 instance.
 *
 * @return `EASYAV1_TRUE` if a video frame is available, `EASYAV1_FALSE` otherwise.
 */
easyav1_bool easyav1_has_video_frame(easyav1_t *easyav1);


/**
 * @brief Gets the current video frame, is one is available.
 *
 * The returned frame is only valid until the next call to `easyav1_get_video_frame`.
 * Calling this function will mark the frame as displayed, so you will only receive a decoded frame once.
 *
 * @param easyav1 The easyav1 instance.
 *
 * @return A pointer to the video frame, or `NULL` if no frame is available.
 */
const easyav1_video_frame *easyav1_get_video_frame(easyav1_t *easyav1);


/**
 * @brief Gets the total number of video frames processed up to this point.
 *
 * @param easyav1 The easyav1 instance.
 *
 * @return The total number of video frames processed.
 */
uint64_t easyav1_get_total_video_frames_processed(easyav1_t *easyav1);


/**
 * @brief Indicates whether the audio buffer is filled.
 *
 * @param easyav1 The easyav1 instance.
 *
 * @return `EASYAV1_TRUE` if the audio buffer is filled, `EASYAV1_FALSE` otherwise.
 */
easyav1_bool easyav1_is_audio_buffer_filled(const easyav1_t *easyav1);


/**
 * @brief Gets the current audio frame, if there are available samples.
 *
 * The frame is only valid until the next call to `easyav1_decode_next` or `easyav1_decode_until`.
 * Calling this function will mark all the samples in the frame as played, so you will only receive the samples once.
 *
 * @param easyav1 The easyav1 instance.
 *
 * @return A pointer to the audio frame, or `NULL` if there are no samples to process.
 */
const easyav1_audio_frame *easyav1_get_audio_frame(easyav1_t *easyav1);


/**
 * @brief Gets the duration of the file.
 *
 * @param easyav1 The easyav1 instance.
 *
 * @return The duration of the file, or `0` if there was an error.
 */
easyav1_timestamp easyav1_get_duration(const easyav1_t *easyav1);


/**
 * @brief Gets the current settings of the easyav1 instance.
 *
 * @param easyav1 The easyav1 instance.
 *
 * @return The settings of the easyav1 instance.
 */
easyav1_settings easyav1_get_current_settings(const easyav1_t *easyav1);


/**
 * @brief Updates the settings of the easyav1 instance.
 *
 * @note This will only update the settings that are different from the current settings.
 *
 * @param easyav1 The easyav1 instance.
 * @param settings The new settings to use.
 *
 * @return `EASYAV1_STATUS_OK` if the settings were updated, `EASYAV1_STATUS_ERROR` if there was an error.
 */
easyav1_status easyav1_update_settings(easyav1_t *easyav1, const easyav1_settings *settings);


/**
 * @brief Indicates whether the easyav1 instance has finished decoding the file.
 *
 * @param easyav1 The easyav1 instance.
 *
 * @return `EASYAV1_TRUE` if the instance has finished decoding, `EASYAV1_FALSE` otherwise.
 */
easyav1_bool easyav1_is_finished(easyav1_t *easyav1);


/**
 * @brief Destroys an easyav1 instance.
 *
 * @param easyav1 The easyav1 instance to destroy.
 */
void easyav1_destroy(easyav1_t **easyav1);

#ifdef __cplusplus
}
#endif

#endif // EASYAV1_H
