# easyAV1

An easy to use WebM demuxer and AV1/Vorbis software decoder.


## What does it do?

In short, this is a library that allows you to add software AV1 video playback to your app or game.

Internally easyAV1 uses Mozilla's [nestegg](https://github.com/mozilla/nestegg) WebM demuxer to demux the video and audio, [dav1d](https://code.videolan.org/videolan/dav1d) to decode the AV1 video stream, and [minivorbis](https://github.com/edubart/minivorbis) (which is libogg and libvorbis consolidated into a single header file) to decode the Vorbis audio stream.

The API is plain C99, and it's wrapped in `extern "C"` so it also works from C++.


## How do I use it?

For a complete usage guide — settings reference, frame formats, seeking, SDL integration and more examples —
see [MANUAL.md](MANUAL.md).

The following code will play a video file at the proper speed:

```c
#include <easyav1.h>
#include <stdio.h>

int main(void)
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
        easyav1_destroy(&easyav1);
        return 2;
    }

    while (easyav1_get_status(easyav1) == EASYAV1_STATUS_OK) {
        if (easyav1_has_video_frame(easyav1) == EASYAV1_TRUE) {
            const easyav1_video_frame *video_frame = easyav1_get_video_frame(easyav1);
            // Do something with the video frame (e.g. upload its YUV planes to a texture).
        }

        // Audio: drain whatever samples are ready. Note that polling audio from
        // this thread while background playback is running is not thread-safe,
        // so some samples may be skipped — for real playback use the audio
        // callback instead (see MANUAL.md §7.2).
        const easyav1_audio_frame *audio_frame;
        while ((audio_frame = easyav1_get_audio_frame(easyav1)) != NULL) {
            // Do something with the audio frame (e.g. push its samples to your audio device).
        }
    }

    easyav1_stop(easyav1);

    if (easyav1_is_finished(easyav1) == EASYAV1_TRUE) {
        printf("Finished playing.\n");
    } else {
        printf("Playback failed.\n");
    }

    easyav1_destroy(&easyav1);
    return 0;
}
```

This uses background playback (`easyav1_play`): easyAV1 spawns an internal thread that decodes continuously at the correct speed, and your loop just consumes whatever arrives. If you want to drive the decoder yourself instead — offline work, frame stepping, benchmarking — use the manual decode loop from [MANUAL.md](MANUAL.md) §5.1. For a complete timed-playback setup with SDL3 (including the audio callback, which is the safe audio path in this mode), see §8.2.

Alternatively, you can check the `tools` folder:

- `easyav1_benchmark.c` - A simple benchmarking tool, plays all frames as fast as possible and measures how long it takes to decode them.
- `easyav1_player.c` - A proper mini player with some basic features such as seeking. Requires SDL3 to build.


## Okay, but how do I build it?

To build easyAV1, you generally need:

- A working C99 compiler
- An assembler (NASM — only needed when building the bundled dav1d from source)
- CMake (3.8 or newer)
- SDL3 — **only** if you want to build the optional `easyav1_player` tool

If you're building for ARM64 on Windows using Visual Studio, you'll also need Perl.

If using Windows, set `CMAKE_PREFIX_PATH` to point to your SDL3 libraries. That should be enough for CMake to detect the library.

easyAV1 has been tested to run on Windows (x64 and ARM64), Linux, macOS (including bundled as a universal binary), Android, PSVita (using VitaSDK, runs at 60 fps only at half the Vita's native resolution due to the slow CPU) and Switch (using devKitPro).

dav1d is a Git submodule, so make sure you clone this repo with submodules included:

```bash
$ git clone --recurse-submodules https://github.com/crudelios/easyav1
```

If you already have the dav1d library, you can use the CMake option `EASYAV1_USE_EXTERNAL_DAV1D_LIBRARY=ON` instead.

Then run:

```bash
$ mkdir build && cd build
$ cmake [-DCMAKE_PREFIX_PATH=<path to SDL on Windows>] [-DEASYAV1_USE_EXTERNAL_DAV1D_LIBRARY=ON] ..
$ cmake --build .
```

This should create `libeasyav1.a`/`easyav1.lib`, as well as the `tools/easyav1_benchmark` executable and, if SDL3 was found, the `tools/easyav1_player` executable (otherwise it is skipped with a warning). Use `-DEASYAV1_BUILD_TOOLS=OFF` to skip the tools entirely.

To link easyAV1 into your own CMake project, the easiest way is to add it as a subdirectory — see [MANUAL.md](MANUAL.md) §2 for details.


## I want to test it! Got any videos?

Getting some test videos is actually not quite so easy, since decent quality videos have copyright issues which makes providing them a bit risky.

However, you can encode your own videos for testing using `ffmpeg`!

```bash
$ ffmpeg -i input.mp4 -c:v libsvtav1 -b:v 3000k -c:a libvorbis -q:a 4 -format webm output.webm
```


## We already have things like ffmpeg. Why should I use this instead?

The purpose of this library is to be easy to use and, in particular, easy to add to an existing project.

In addition, it has a much smaller executable footprint than ffmpeg.

However, software AV1 decoding is all but simple. So, to try and fulfill that objective as best as possible, the library abides by the following rules:

- All code is C99, for maximum portability
- Libraries used by the code are bundled with this library (for dav1d, you need to download the Git submodules after cloning this repo)
- The entire project, including bundled libraries, uses CMake to build. CMake is more widely available than, say, Meson, and is also more portable


## Where would this be used, anyway?

AV1 is a very interesting video codec. Developed by AOM, it (hopefully) is free of any patents, and it's also very efficient, providing good quality video at fairly low bitrates.

In essence, this library serves the same purpose of [pl_mpeg](https://github.com/phoboslab/pl_mpeg): getting video playback into a simple app or an old-school or indie game, without requiring huge complicated libraries.


## What is the catch?

In order to keep the code simple, some sacrifices had to be made:

- Only WebM containers, AV1 video and Vorbis audio are supported
- By default, only 8bpc video is supported. If you want 16bpc support, please run cmake with `-Dbitdepths=All` (this also enables 10/12-bit decoding)
- The provided player only supports the 8-bit YUV420 picture format. Other formats will be ignored.
- The decoding is done using software, even if there's hardware decoding support for AV1

Also, despite all the claimed simplicity, the code is still a bit on the larger side: expect an increase of about 2MB to your executable (or 3MB if 16bpc is enabled).


## What are the expectations going forward?

While the code appears to work, it hasn't been heavily tested, so caution should be exercised. Please don't add easyAV1 to critical code just yet.


## License

easyAV1 is licensed under the BSD 3-Clause License — see [LICENSE](LICENSE). The bundled third-party libraries (nestegg, dav1d, minivorbis) are distributed under their own licenses — see the respective files under `ext/`.
