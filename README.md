# easyAV1

An easy to use WebM demuxer and AV1/Vorbis software decoder.


## What does it do?

In short, this is a library that allows you to add software AV1 video playback to your app or game.

Internally easyAV1 uses Mozilla's nestegg WebM demuxer to demux the video and audio, dav1d to decode the AV1 video stream and minivorbis (which is libogg and libvorbis compated to a single header file) to decode a Vorbis audio stream.


## How do I use it?

The following code will play a video file at the proper speed:

```c
#include <easyav1.h>
 
int main(int argc, char **argv)
{
    easyav1_t *easyav1 = easyav1_init_from_filename("video.webm", NULL);
 
    if (!easyav1) {
        printf("Failed to initialize easyav1.\n");
        return 1;
    }

    if (easyav1_has_video_track(easyav1) == EASYAV1_TRUE) {
        printf("Video size: %ux%u\n", easyav1_get_video_width(easyav1), easyav1_get_video_height(easyav1));
    }

    if (easyav1_has_audio_track(easyav1) == EASYAV1_TRUE) {
        printf("Audio sample rate: %u\n", easyav1_get_audio_sample_rate(easyav1));
        printf("Audio channels: %u\n", easyav1_get_audio_channels(easyav1));
    }

    if (easyav1_play(easyav1) != EASYAV1_STATUS_OK) {
        printf("Failed to start playing video.\n");
        easyav1_destroy(easyav1);

        return 2;
    }

    while (easyav1_get_status(easyav1) == EASYAV1_STATUS_OK) {
        if (easyav1_has_video_frame(easyav1) == EASYAV1_TRUE) {
            const easyav1_video_frame *video_frame = easyav1_get_video_frame(easyav1);
            // Do something with the video frame.
        }

        if (easyav1_has_audio_frame(easyav1) == EASYAV1_TRUE) {
            const easyav1_audio_frame *audio_frame = easyav1_get_audio_frame(easyav1);
            // Do something with the audio frame.
        }
    }

    easyav1_stop(easyav1);

    if (easyav1_is_finished(easyav1) == EASYAV1_TRUE) {
        printf("Finished decoding.\n");
    } else {
        printf("Failed to decode.\n");
    }

    easyav1_destroy(&easyav1);
    return 0;
}
```

Alternatively, you can check the `tools` folder:

- `easyav1_benchmark.c` - A simple benchmark tool, plays all frames as fast as possible and measures how long it takes
  to decode them.
- `easyav1_player.c` - A proper mini player with some basic features such as seeking.


## Okay, but how do I build it?

To build easyAV1, you generally need:

- A working compiler
- A working assembler
- CMake
- SDL3 (for `easyav1_player`)

If you're building for ARM64 on windows using Visual Studio, you'll also need Perl.

If using Windows, place the SDL3 libraries (either the MSVC or the MinGW dev libraries that you can get from SDL's release page) inside `ext/SDL3`. That will be enough for CMake to detect the library.

Then run:

```bash
$ mkdir build && cd build
$ cmake ..
$ make
```

This should create `libeasyav1.a`/`easyav1.lib`, as well as the executables `tools/easyav1_player` and `tools/easyav1_benchmark`.


## I want to test it! Got any videos?

Getting some test videos is actually not quite so easy, since decent quality videos have copyright issues which makes providing them a bit risky.

However, you can encode your own videos for testing using `ffmpeg`!

```bash
$ ffmpeg -i input.mp4 -c:v libsvtav1 -b:v 3000k -c:a libvorbis -q:a 4 -format webm output.webm
```

## We already have things like ffmpeg. Why should I use this instead?

The purpose of this library is to be easy to use and, in particularly, easy to add to an existing project.

In addition, it has a much smaller executable footprint than ffmpeg.

However, software AV1 decoding is all but simple. So, to try and fulfill that objective as best as possible, the library abides by the following rules:

- All code is C99, for maximum portability
- Libraries used by the code are bundled with this library
- The entire project, including bundled libraries, use CMake to build. CMake is more widely available than, say, Meson, and is also more portable


## Where would this be used, anyway?

AV1 is a very interesting video codec. Developed by AOM, it (hopefully) is free of any patents, and it's also very efficient, providing good quality video at fairly low bitrates.

In essence, this library serves the same purpose of [pl_mpeg](https://github.com/phoboslab/pl_mpeg: getting video playback into a simple app or and old-school or indie game, without requiring huge complicated libraries.


## What is the catch?

In order to keep the code simple, some sacrifices had to be made:

- Only WebM containers, AV1 video and Vorbis audio are supported
- By default, only 8bpc video is supported. If you want 16bpc support, please run cmake with `-Dbitdepths=All`
- The provided player only supports the 8 bit yuv420 picture format. Other formats will be ignored.
- The decoding is done using software, even if there's hardware decoding support for AV1

Also, despite all the claimed simplicity, the code is still a bit on the large side: expect an increase of about 2MB to your executable (or 3MB if 16bpc is enabled).


## What are the expectations going forward?

While the code appears to work, it hasn't been heavily tested, so caution should be exerted. No adding easyAV1 to critical code, please.
