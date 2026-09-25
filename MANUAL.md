# easyAV1 Manual

A complete usage guide for easyAV1 — a small, self-contained C99 library that demuxes WebM files
and decodes AV1 video and Vorbis audio entirely in software. Its goal is to let you add video
playback to a game or application without dragging in a half-gigabyte of ffmpeg.

The formal API reference is [chapter 11](#11-api-full-reference) — a complete transcription
of the documented comments in [`src/easyav1.h`](src/easyav1.h), which remains the source of
truth — but a reference rarely tells you *when* and *why* to use a function. That's what this
manual is for: it walks through the library's concepts, shows the typical lifecycle of an
easyAV1 instance, and provides complete, commented examples, including full SDL3 integration.

*Disclaimer*: This manual was created with the assistance of AI due to lack of time to do it
manually. However, I've proof read the entire manual myself and made sure everything is correct.

## How to read this manual

- If you just want working code, jump to [Examples](#8-examples) and start from 8.1 or 8.2.
- If you're integrating this into a game and want to understand the moving parts first, read
  [Core concepts](#3-core-concepts) and [Decoding and playback](#5-decoding-and-playback).
- If you need a function's exact contract, go straight to [API full reference](#11-api-full-reference).
- If something isn't working, check [Limitations and gotchas](#12-limitations-and-gotchas) and
  [Troubleshooting](#13-troubleshooting) before digging into the source.

---

## Table of contents

1. [Overview](#1-overview)
2. [Building the library](#2-building-the-library)
3. [Core concepts](#3-core-concepts)
4. [Initialization and settings](#4-initialization-and-settings)
5. [Decoding and playback](#5-decoding-and-playback)
6. [Video frames](#6-video-frames)
7. [Audio frames](#7-audio-frames)
8. [Examples](#8-examples)
9. [Seeking](#9-seeking)
10. [API quick reference](#10-api-quick-reference)
11. [API full reference](#11-api-full-reference)
12. [Limitations and gotchas](#12-limitations-and-gotchas)
13. [Troubleshooting](#13-troubleshooting)
14. [License](#14-license)

---

## 1. Overview

### What problem does easyAV1 solve?

Video playback in games has historically been a pain. Your options are roughly:

1. **ffmpeg + libav** — battle-tested, but a very large dependency tree, a huge binary, and an
   API that rewards expertise. Overkill for "play this intro cinematic".
2. **OS media APIs** (DirectShow, AVFoundation, …) — platform-specific, hard to embed in your
   own rendering pipeline, and usually tied to the OS's player UI assumptions.
3. **A small, purpose-built library** — this is what easyAV1 is.

easyAV1 gives you one C header, one static library, and a handful of functions. It supports
exactly one container (WebM) and exactly one codec pair (AV1 video + Vorbis audio), which is
enough for most game cinematics, menus and in-engine videos, especially since AV1 is a
patent-unencumbered, very efficient codec.

### What's inside the box?

You never talk to the underlying libraries directly; easyAV1 wraps them:

| Role | Library | Notes |
| --- | --- | --- |
| WebM demuxing | [nestegg](https://github.com/mozilla/nestegg) (Mozilla) | Bundled as a single C file (`ext/nestegg/nestegg.c`) |
| AV1 video decoding | [dav1d](https://code.videolan.org/videolan/dav1d) | Bundled as a Git submodule, or use your own system copy |
| Vorbis audio decoding | [minivorbis](https://github.com/edubart/minivorbis) | libogg + libvorbis consolidated into a single header file |

The practical consequence of "software-only decoding" is that easyAV1 runs anywhere your CPU
runs — Windows, Linux, macOS, Android, and even consoles such as PSVita and Switch. There is no
GPU dependency and no driver story to worry about, but on very weak CPUs (e.g. the PSVita)
expect to be limited to modest resolutions.

### What easyAV1 is *not*

- It is **not** a general video framework. No MP4, no H.264, no AAC, no subtitles.
- It is **not** a player. It produces raw YUV video planes and raw float PCM audio; *you* decide
  what to do with them (upload to a texture, push into an audio stream, draw on a canvas, …).
- It is **not** a black box. The API is small on purpose, and every knob (tracks, offsets,
  seeking behavior) is exposed in the settings struct.

---

## 2. Building the library

### Requirements

- A working C compiler (C99) and assembler
- CMake ≥ 3.8
- NASM (for the dav1d assembly optimizations, unless you use a prebuilt dav1d)
- SDL3 — **only** if you want to build the optional `easyav1_player` tool (the benchmark doesn't need it)
- Perl — **only** for ARM64 Windows / Visual Studio builds

### Building from source

```bash
# --recurse-submodules is required: dav1d is a submodule
git clone --recurse-submodules https://github.com/crudelios/easyav1
mkdir build && cd build
cmake ..
cmake --build .
```

This produces:

- `libeasyav1.a` (or `easyav1.lib` on Windows) — the library you link against, and
- `tools/easyav1_player` (only if SDL3 was found) and `tools/easyav1_benchmark` — reference
  programs.

### Useful CMake options

| Option | Default | What it does, and when you'd care |
| --- | --- | --- |
| `EASYAV1_USE_EXTERNAL_DAV1D_LIBRARY` | `OFF` | `ON`: find dav1d with `find_package` instead of compiling the submodule. Useful if your distro provides dav1d or you want one shared dav1d across multiple projects. |
| `EASYAV1_BUILD_TOOLS` | `ON` | Set `OFF` if you don't want either tool. Note: the player is only built if SDL3 is found (otherwise it's skipped with a warning); the benchmark needs no SDL3 at all. |
| `EASYAV1_USE_SANITIZERS` | `ON` | Debug builds on non-Windows get `-fsanitize=thread,undefined`. Turn off if you want a "fast" debug build. |
| `bitdepths` | 8bpc | Pass `-Dbitdepths=All` to also build the 16bpc decoder (covers 10/12-bit content). Costs ~1MB of binary size for content most games don't have. |

### Linking from your own CMake project

The simplest recipe: add easyAV1 as a subdirectory so CMake handles the include paths and the
dav1d dependency for you.

```cmake
add_subdirectory(path/to/easyav1 easyav1)

add_executable(mygame main.c)
target_link_libraries(mygame PRIVATE easyav1)
# `easyav1` is an interface-correct target: includes (src/ and ext/) and
# the dav1d/threads dependencies come along automatically.
```

If you prefer a prebuilt static library, make sure your include path contains easyAV1's `src/`
and `ext/` directories and that you link the dav1d static libraries alongside `libeasyav1.a`
(link order matters for static libraries).

### On Windows

Set `CMAKE_PREFIX_PATH` to your SDL3 development libraries (MSVC or MinGW, from SDL's release
page) when configuring; CMake will detect SDL3 from there.

### Making test videos

Finding legally safe, high-quality AV1 WebM samples is hard, but you can generate your own in
seconds:

```bash
ffmpeg -i input.mp4 -c:v libsvtav1 -b:v 3000k -c:a libvorbis -q:a 4 -format webm output.webm
```

- `-c:v libsvtav1` — AV1 encoding (use `libaom-av1` for slower/conformant encoding)
- `-b:v 3000k` — 3 Mbit/s; bump it if you want to stress-test the decoder
- `-c:a libvorbis -q:a 4` — Vorbis audio at quality 4
- `-format webm` — the only container easyAV1 understands

Encoding a 720p or 1080p clip and playing it back through the bundled player is the fastest
end-to-end sanity check you can do.

---

## 3. Core concepts

Before touching any function, internalize four ideas. Everything else in the library falls out
of them.

### 3.1 One handle, one lifecycle

All state lives in a single opaque object, `easyav1_t`. You create it with one of the
`easyav1_init_from_*` functions, use it, and tear it down with `easyav1_destroy`. There are no
global objects and no hidden state, which makes reasoning about ownership easy:

```
init ──► (settings, queries, decoding, seeking…) ──► destroy
```

`easyav1_destroy(&handle)` takes the **address** of the handle and clears it for you, so the
idiomatic end of every program is just:

```c
easyav1_destroy(&easyav1);
```

### 3.2 Two independent axes: who drives the decoder, and how frames reach you

This is the single most important design decision in the library, and the source of most
confusion, so it's worth understanding properly. easyAV1 has two **independent** knobs:

**Axis 1 — who drives the decoder. Pick one of these two models (they are mutually
exclusive per instance):**

**Model A — Manual decoding.** *You* call the decoder: `easyav1_decode_next` (or
`decode_until` / `decode_for`). Each call advances the demux/decode pipeline by one packet (or
up to a target time). You are responsible for pacing: decode at full speed, decode in lockstep
with your own clock, decode ahead to build a buffer — your choice.

**Model B — Background playback.** You call `easyav1_play`, and easyAV1 spawns an internal
thread that decodes continuously, paced by wall-clock time so the video plays at its intended
speed. You stop (pause) with `easyav1_stop` and resume with `easyav1_play` again.

**Axis 2 — how frames reach you. Either delivery works with *either* model:**

**Polling.** Ask `easyav1_has_video_frame` / `easyav1_is_audio_buffer_filled` and take what's
available with `easyav1_get_video_frame` / `easyav1_get_audio_frame` (for audio, you'll usually
want to just drain `easyav1_get_audio_frame` directly — see [7.2](#72-is-there-audio--a-naming-nuance)).

**Callbacks.** Register `settings.callbacks.video` and `settings.callbacks.audio`; easyAV1
invokes them each time a frame is ready. In Model A they fire synchronously on *your* thread,
right after your decode call; in Model B they fire on easyAV1's internal thread.

That gives four working combinations — the two "canonical" ones are in bold:

| | **Polling** | **Callbacks** |
| --- | --- | --- |
| **Model A — manual** | **The classic loop** (§5.1, §8.1, §8.5): decode, check, consume. | Event-driven manual decoding: the callback fires on your thread after each `easyav1_decode_*` call. Handy if your app is callback-based. |
| **Model B — background** | Frames wait in the queue while the background thread decodes; you poll from your app loop. | **The cinematic setup** (§8.2): easyAV1 paces and delivers; you just render and submit audio. |

Which should you pick?

| You want… | Use | Why |
| --- | --- | --- |
| A cinematic that plays like a normal video (constant speed, A/V synced for you) | **B + callbacks** | easyAV1's background thread does the pacing; your job is only "render frame" and "submit audio" when the callbacks fire |
| Frame-stepping, slow motion, fast-forward, or syncing to *your* game clock | **A + polling** | You control exactly how much media time is decoded per iteration |
| Benchmarking decoding speed | **A + polling** | decode as fast as the CPU allows |
| A "pause/resume" button in a player UI | Either — B with `play`/`stop`, or A by simply stopping the calls | |

The one hard rule: **don't mix the drive models** on one instance — either call
`easyav1_decode_*` yourself or call `easyav1_play`, not both. The delivery choice (polling vs
callbacks) is free and independent.

### 3.3 Frames are fleeting

**Video frames** (pointer and memory) are valid until the next `easyav1_get_video_frame`
call — or a seek, or `easyav1_destroy`. In Model A nothing else can free them, and in the
callback model the callback ends exactly when delivery completes, so **you have full control
over their lifetime**.

**Audio frames** are different — and more fragile. The frame points into one shared sample
ring, and the next audio decode **overwrites it**: `easyav1_get_audio_frame` only clears an
internal "has samples" flag, so the next decode resets `frame->samples` to `0` and writes the
new samples starting at the *beginning* of the ring — on top of the ones you were holding. The
`samples`/`timestamp`/`bytes` fields are rewritten too. In Model B that decode happens on the
playback thread, outside your control — and the audio path is **not synchronized**, so polling
audio from your app thread while Model B runs is a data race. In practice, what this means is that every now and then the sound may pop or crackle. Nothing serious, but annoying nonetheless.

This is a **known design flaw**: this part of the API is expected to change in a future
release to be drain-friendly and thread-safe (see [7.2](#72-is-there-audio--a-naming-nuance)).
Until then, the safe patterns are: use the **audio callback** (in Model B it runs on the
playback thread, so it's inherently serialized with the decoder — the recommended path there),
or **copy the samples immediately** after `easyav1_get_audio_frame` if you need to keep them.

Consequences:

1. **Consume immediately.** Upload the YUV planes to a GPU texture, push the PCM into your audio
   device, or copy the buffers — before doing anything else that touches easyAV1.
2. **Each frame is delivered exactly once.** Fetching a frame marks it as consumed. If your
   render code can't take the frame right now (backpressure), the behavior depends on
   `skip_unprocessed_frames` (see [Settings](#4-initialization-and-settings)).
3. **Want to hold or defer a frame? Copy it.** Video: copy the planes (their memory is freed
   by the next `easyav1_get_video_frame` — *even if that call returns `NULL`*). Audio: copy the
   samples immediately (they're overwritten by the next decode — see
   [7.2](#72-is-there-audio--a-naming-nuance)).

### 3.4 Time is measured in milliseconds

Every timestamp you see or pass — frame timestamps, `easyav1_get_duration`,
`easyav1_get_current_timestamp`, seek arguments, `audio_offset_time` — is in **milliseconds**.
This is deliberate: WebM timestamps are nanosecond-scale internally, but milliseconds are the
resolution you actually need for playback, and they keep the math pleasant. Do **not**
multiply by 10⁶ out of habit.

---

## 4. Initialization and settings

### 4.1 Choosing where the video comes from

There are four constructors; they differ only in *where the bytes come from*. After
initialization, everything behaves identically.

| Function | Source | Typical use |
| --- | --- | --- |
| `easyav1_init_from_filename` | A path on disk | Tools, quick prototyping, user-supplied files |
| `easyav1_init_from_file` | An already-open `FILE*` | You own the file lifetime (e.g. you opened it with special permissions) |
| `easyav1_init_from_memory` | A buffer in memory | **The common game case**: the video is baked into your asset pipeline and handed to you as bytes |
| `easyav1_init_from_custom_stream` | Your `read`/`seek`/`tell` callbacks | Anything else: archives, memory-mapped files, `SDL_IOStream`, network |

The last one deserves a look, because it's the most flexible. Fill in an `easyav1_stream`:

```c
// Read `size` bytes into `buffer`.
//   return 1  if `size` bytes were read
//   return 0  if the end of the stream was reached (EOF)
//   return -1 on error
int my_read(void *buffer, size_t size, void *userdata);

// Seek. `origin` is SEEK_SET, SEEK_CUR or SEEK_END (same semantics as stdio).
//   return 0 on success, non-zero on error
int my_seek(int64_t offset, int origin, void *userdata);

// Report the current position in bytes (or -1 on error)
int64_t my_tell(void *userdata);

easyav1_stream stream = { my_read, my_seek, my_tell, my_context_ptr };
easyav1_t *easyav1 = easyav1_init_from_custom_stream(&stream, &settings);
```

`userdata` is passed back to all three callbacks, so it's your hook into whatever object owns
the bytes (an `SDL_IOStream`, an archive handle, a socket, …). See [Example 8.4](#84-custom-io-an-sdl_iostream)
for a complete SDL3 implementation.

### 4.2 The settings struct

Every constructor takes an optional `const easyav1_settings *`. If you pass `NULL`, defaults
are used; otherwise, **start from `easyav1_default_settings()` and change only what you need** —
this keeps you future-proof if defaults ever change:

```c
easyav1_settings settings = easyav1_default_settings();
settings.log_level = EASYAV1_LOG_LEVEL_ERROR;
easyav1_t *easyav1 = easyav1_init_from_filename("intro.webm", &settings);
```

Here's the struct, field by field, with the "why does this exist" explanation (where applicable, read `TRUE` as `EASYAV1_TRUE` and `FALSE` as `EASYAV1_FALSE`).

| Field | Default | What it does and why it exists |
| --- | --- | --- |
| `enable_video` | `EASYAV1_TRUE` | Set to `EASYAV1_FALSE` to skip video decoding entirely. Saves a *lot* of CPU if you only care about the audio (or vice versa with `enable_audio`). |
| `enable_audio` | `EASYAV1_TRUE` | Same idea for audio. A silent cinematic with `enable_audio = FALSE` will decode noticeably faster. |
| `skip_unprocessed_frames` | `EASYAV1_TRUE` | If a decoded frame is still sitting undelivered when the next one is ready, drop the stale one. This is what keeps real-time playback smooth when your renderer hiccups: you always see the *freshest* frame. Set to `EASYAV1_FALSE` if you decode manually and guarantee you'll consume every frame (e.g. a benchmark, or an offline tool). |
| `interlace_audio` | `EASYAV1_TRUE` | `TRUE`: audio arrives interleaved (`LRLR…`, a single `float*`). `FALSE`: planar (`float*[channels]`, one array per channel). Interleaved is what virtually every audio API (SDL, OpenAL, miniaudio…) wants directly; use planar only if your DSP pipeline is channel-based. See [Audio frames](#7-audio-frames). |
| `close_file_handle_on_destroy` | `EASYAV1_FALSE` | Who owns the resource decides what happens on `easyav1_destroy`: with `init_from_filename`, easyAV1 opened the file itself, so it **always** closes it (it's your only way of closing it, and the flag doesn't apply); with `init_from_file`, the file is yours, and this flag decides whether easyAV1 closes it for you; with `init_from_memory` and custom streams, your data/I-O is never freed or closed by easyAV1 — it only frees its own internal bookkeeping. |
| `callbacks.video` | `NULL` | The video delivery callback: `void fn(const easyav1_video_frame *frame, void *userdata)`. Called each time a frame is ready — on *your* thread right after an `easyav1_decode_*` call (Model A), or from easyAV1's internal thread during `easyav1_play` (Model B). |
| `callbacks.audio` | `NULL` | The audio delivery callback: `void fn(const easyav1_audio_frame *frame, void *userdata)`. Same threading rules as the video callback. |
| `callbacks.userdata` | `NULL` | Opaque pointer handed to both callbacks. |
| `video_track` | `0` | WebM can carry multiple video tracks (think language-dubbed video). This selects which one, counted among *video* tracks only. `0` is the first. |
| `audio_track` | `0` | Same for audio tracks — handy for picking a dubbed audio track. |
| `use_fast_seeking` | `EASYAV1_FALSE` | `FALSE` (default): seeks land *exactly* on the requested timestamp, by decoding from the previous keyframe — accurate but slower. `TRUE`: seeks snap to the nearest keyframe *before* the target — fast, but you may land a few frames early. Great for scrubbing UIs, less ideal for frame-accurate editing. |
| `audio_offset_time` | `0` | A/V sync trim in **milliseconds**. Positive shifts audio *later* relative to video, negative *earlier*. Useful to compensate for audio-buffering latency (e.g. SDL's output delay) or simply to nudge a slightly out-of-sync clip. Note the WebM's own `DefaultDuration`/audio-delay metadata is applied on top of this value. |
| `log_level` | `EASYAV1_LOG_LEVEL_WARNING` | `ERROR` (quiet, production), `WARNING` (default), `INFO` (chatty — invaluable while debugging: it logs seeks, queue decisions, etc.). |

### 4.3 Reading and updating settings at runtime

You're not stuck with your initial settings:

```c
easyav1_settings current = easyav1_get_current_settings(easyav1);
current.log_level = EASYAV1_LOG_LEVEL_INFO;   // e.g. enable debug logging on demand
easyav1_update_settings(easyav1, &current);
```

`easyav1_update_settings` merges the new values into the live instance. Use it for things the
player UI exposes: A/V offset nudging, switching track, toggling fast seeking.

### 4.4 Learning about your file

Once initialized, you can ask the demuxer about the content. Do this *before* you build your
rendering/audio targets, because it tells you exactly what you're getting:

```c
if (easyav1_has_video_track(easyav1) == EASYAV1_TRUE) {
    printf("Video: %ux%u @ %u fps\n",
        easyav1_get_video_width(easyav1),
        easyav1_get_video_height(easyav1),
        easyav1_get_video_fps(easyav1));
}

if (easyav1_has_audio_track(easyav1) == EASYAV1_TRUE) {
    printf("Audio: %u Hz, %u channels\n",
        easyav1_get_audio_sample_rate(easyav1),
        easyav1_get_audio_channels(easyav1));
}

printf("Duration: %lld ms, %u video track(s), %u audio track(s)\n",
    (long long) easyav1_get_duration(easyav1),
    easyav1_get_total_video_tracks(easyav1),
    easyav1_get_total_audio_tracks(easyav1));
```

These queries return `0`/`FALSE` when the corresponding track doesn't exist, so you can safely
call them without prior checks if you like.

---

## 5. Decoding and playback

### 5.1 Model A in detail: the manual decode loop

The core loop of Model A with polling delivery looks like this (if you registered callbacks
instead, they fire on your thread after each `easyav1_decode_*` call — see
[3.2](#32-two-independent-axes-who-drives-the-decoder-and-how-frames-reach-you) — and the loop
collapses to just the decode-driving `while`):

```c
while (easyav1_decode_next(easyav1) == EASYAV1_STATUS_OK) {
    if (easyav1_has_video_frame(easyav1) == EASYAV1_TRUE) {
        const easyav1_video_frame *v = easyav1_get_video_frame(easyav1);
        // render v
    }
    // Audio: drain whatever is ready — however many samples that is (see 7.2).
    // Consume it NOW (or copy it); don't wait for a "full" buffer.
    const easyav1_audio_frame *a;
    while ((a = easyav1_get_audio_frame(easyav1)) != NULL) {
        // submit a->pcm.interlaced, a->bytes (a->samples sample frames)
    }
}

if (easyav1_is_finished(easyav1) == EASYAV1_TRUE) {
    // success!
} else {
    // error!
}
```

A few things to understand about this loop:

- **`easyav1_decode_next` decodes one packet, not one frame.** One WebM packet is usually one
  video frame or one audio block, but video and audio alternate, so on some iterations you get a
  video frame, on others only audio (or a packet that doesn't yield anything you care about).
  That's why you check `easyav1_has_video_frame` after *every* decode call instead of assuming
  one — and why you drain audio on every iteration (a `NULL` from `easyav1_get_audio_frame`
  simply means "nothing new since last time").
- **Return values.** `EASYAV1_STATUS_OK` = keep going; `EASYAV1_STATUS_FINISHED` = clean end of
  file; `EASYAV1_STATUS_ERROR` = something went wrong (check `easyav1_get_status` for a detailed
  code and turn on `EASYAV1_LOG_LEVEL_INFO` to see what happened).
- **After the loop**, `easyav1_is_finished(easyav1)` tells you whether you reached the end
  cleanly or bailed out on an error. It's the cleanest final status check.

Two companions give you more control over *how much* to decode per call:

```c
easyav1_decode_until(easyav1, target_ms);  // decode until stream position >= target_ms
easyav1_decode_for(easyav1, delta_ms);     // decode exactly delta_ms worth of media time
```

`easyav1_decode_for` is the workhorse for game-loop integration: each frame of your game, you
ask "how much real time passed since last frame?" and decode exactly that much media time. The
result is perfect A/V sync against your own clock, with the added benefit that when your game
lags, you simply decode a bit more next frame — the video catches up instead of stalling.

### 5.2 Model B in detail: background playback

```c
// 1. Register callbacks in the settings *before* easyav1_play.
// 2. Start.
easyav1_play(easyav1);

// 3. Your normal app loop; frames arrive via the callbacks.
while (app_running && easyav1_get_status(easyav1) == EASYAV1_STATUS_OK) {
    render();        // draw the most recent video frame
    pump_audio();    // feed your audio device
}

// 4. Stop (also used for pausing).
easyav1_stop(easyav1);

// 5. Check the outcome.
easyav1_bool ok = easyav1_is_finished(easyav1);
```

Things to know:

- **`easyav1_play` is non-blocking.** It starts the internal decode thread and returns
  immediately.
- **Video callbacks are optional here.** You can omit them and *poll*
  `easyav1_has_video_frame` from your app loop — video frames simply wait in the queue (see
  [3.2](#32-two-independent-axes-who-drives-the-decoder-and-how-frames-reach-you)).
  **For audio, the callback is the recommended path in Model B**: the audio path is not
  synchronized, so polling it from your app thread while the playback thread decodes is a data
  race (see [7.2](#72-is-there-audio--a-naming-nuance)).
- **If you use callbacks, they run on that internal thread.** Treat them like any
  foreign-thread callback: keep them short, don't block, and be careful with shared state
  (textures, audio streams). See [Thread-safety notes](#121-thread-safety-notes) for concrete
  guidance.
- **Pausing = `easyav1_stop`.** Resuming = `easyav1_play` again. Playback picks up where it
  left off.
- **Seeking while playing:** just call `easyav1_seek_to_timestamp` (or `_forward`/`_backward`) —
  the playback thread picks up the request after its current decode step and continues from the
  new position. No need to stop first. Still, don't call easyAV1 functions from inside a
  callback — in Model A a seek there would re-enter the decode state machine mid-iteration (see
  [9](#9-seeking)).
- **When the file ends**, the background thread finishes, `easyav1_get_status` stops reporting
  `OK`, and `easyav1_is_finished` returns `EASYAV1_TRUE`.

### 5.3 Status codes

`easyav1_get_status` reports the current health of the instance. The "general" values are
`EASYAV1_STATUS_OK`, `EASYAV1_STATUS_FINISHED`, `EASYAV1_STATUS_ERROR`; the detailed error
codes (`EASYAV1_STATUS_INVALID_ARGUMENT`, `EASYAV1_STATUS_OUT_OF_MEMORY`,
`EASYAV1_STATUS_IO_ERROR`, `EASYAV1_STATUS_DECODER_ERROR`, `EASYAV1_STATUS_NOT_IMPLEMENTED`,
`EASYAV1_STATUS_INVALID_STATE`, `EASYAV1_STATUS_INVALID_DATA`, `EASYAV1_STATUS_UNSUPPORTED`)
tell you *why* it failed, which is exactly what you want when debugging a file that refuses to
play.

---

## 6. Video frames

A decoded video frame is handed to you as raw YUV planes — no RGB conversion, no GPU magic.
This is intentional: easyAV1 doesn't know what renderer you use, so it gives you the data in
the most neutral form and lets *you* do the upload.

```c
const easyav1_video_frame *frame = easyav1_get_video_frame(easyav1);
```

### 6.1 The properties

```c
frame->properties.width;      // width in pixels
frame->properties.height;     // height in pixels
frame->properties.pixel_layout;    // YUV400, YUV420, YUV422 or YUV444
frame->properties.bits_per_color;  // easyav1_bits_per_color: EASYAV1_BITS_PER_COLOR_8 (default build), _10/_12 with -Dbitdepths=All
frame->properties.color_space;     // EASYAV1_COLOR_SPACE_LIMITED (TV range) or _FULL (PC range)
frame->properties.color_primaries;    // BT.709, BT.601, BT.2020, …
frame->properties.transfer_characteristics; // gamma/PQ/HLG curve
frame->properties.matrix_coefficients;      // YUV↔RGB matrix
frame->properties.chroma_sample_position;   // where chroma samples sit, relative to luma
frame->timestamp;          // presentation time, in milliseconds
```

For a typical game cinematic you'll mostly care about `width`, `height`, `pixel_layout`,
`bits_per_color` and `color_space` — everything else matters if you're doing color-management
correctly (the primaries/transfer/matrix triple is the standard BT.709/BT.2020 colorimetry
description).

### 6.2 The planes

```c
frame->data[0];   // luma (Y) plane — pointer to the first row
frame->data[1];   // chroma (U) plane
frame->data[2];   // chroma (V) plane
frame->stride[0]; // bytes between the start of row N and row N+1, per plane
frame->stride[1];
frame->stride[2];
```

Three rules for working with planes:

1. **Always use `stride[i]`, never `width`, to index rows.** Decoders (dav1d included) are free
   to pad rows for alignment, so `stride` can be larger than the plane's logical width.
2. **Chroma is subsampled in YUV420** — the most common layout — meaning `data[1]`/`data[2]`
   are half the width and half the height of `data[0]`. (YUV422 is half width only; YUV444 is
   full size; YUV400 has no chroma planes at all.)
3. **The pointer is temporary.** It's valid only until the pipeline advances — upload it to a
   texture *before* calling any other easyAV1 function that decodes.

### 6.3 Rendering YUV to the screen

The standard approach with SDL3 (and with most engines) is a YUV texture: create a texture with
a YUV format (e.g. the IYUV/NV12 family your renderer supports), upload each plane into its
region of the texture honoring the plane sizes and strides, and let the GPU do the YUV→RGB
conversion during the textured draw call. It's fast (no CPU pixel conversion) and colorimetrically
correct as long as you picked a texture format matching the frame's `pixel_layout` and
`color_space`.

If your engine only accepts RGBA textures, you can convert on the CPU — it works, but it's
expensive at 1080p+ and you'll be tempted to buy the performance back with a YUV texture anyway.

---

## 7. Audio frames

easyAV1 always decodes Vorbis to **32-bit floating point PCM**, with samples in the
`[-1.0, 1.0]` range. That's the native output of minivorbis, and it's also the format that
modern audio APIs (SDL3, OpenAL, miniaudio) prefer — you can usually feed it straight in.

```c
const easyav1_audio_frame *frame = easyav1_get_audio_frame(easyav1);

frame->channels;   // e.g. 2 for stereo
frame->samples;    // number of sample *frames* in this chunk
frame->timestamp;  // presentation time, in milliseconds
frame->bytes;      // total payload size in bytes — handy for bulk API calls
frame->pcm.interlaced;     // float* — L R L R … (when settings.interlace_audio == TRUE)
frame->pcm.deinterlaced;   // float*[channels] — one contiguous array per channel (when FALSE)
```

### 7.1 Interleaved vs planar

- **Interleaved** (`interlace_audio = EASYAV1_TRUE`, the default): one flat array,
  `L R L R …`, length `samples * channels * sizeof(float)` = `frame->bytes`. This is what audio
  device APIs expect, so 95% of integrations use the default and never think about it.
- **Planar** (`interlace_audio = EASYAV1_FALSE`): `frame->pcm.deinterlaced[c]` points to
  `samples` floats for channel `c`. Choose this only if your DSP is per-channel (e.g. you're
  computing per-channel levels or running per-channel effects).

**Use the field that matches the setting.** With the default settings, read
`frame->pcm.interlaced` (and `frame->bytes` for the size). If you flip `interlace_audio`,
switch to `frame->pcm.deinterlaced` — the other union member is meaningless in that mode.

### 7.2 "Is there audio?" — a naming nuance

Video has `easyav1_has_video_frame`; the audio equivalent is
**`easyav1_is_audio_buffer_filled`** (yes, the names are asymmetric — that's the API). But note
what it *actually* answers: "is the sample ring **completely full**?" — not "are there samples
waiting for me?". A full ring is not a *ready* state; it's an *overflow-imminent* state — the
next decode will discard the oldest unprocessed samples to make room. Waiting for fullness
before consuming costs you up to a full buffer of latency (the ring holds 4096 samples per
channel, ≈85–93 ms depending on sample rate) and, in Model B where the playback thread keeps
decoding while you wait, continuous sample loss.

**The right pattern is to drain, not to wait for fullness:**

```c
const easyav1_audio_frame *a;
while ((a = easyav1_get_audio_frame(easyav1)) != NULL) {
    // a->samples sample frames are waiting — consume them now (submit, or copy)
}
```

`easyav1_get_audio_frame` hands you *whatever is in the ring*, even a few samples, and its
`samples`/`bytes` fields tell you exactly how much. `easyav1_is_audio_buffer_filled` remains
as a convenience check — but treat `EASYAV1_TRUE` as a *warning* ("consume now or lose
samples"), not as a green light.

**Known design issue — this part of the API will change.** The audio path shares one ring
buffer between "decoded, not yet consumed" and "being decoded"; `easyav1_get_audio_frame`
doesn't commit the samples it hands out (the next decode can overwrite them and rewrite
`samples`/`timestamp`/`bytes` — see [3.3](#33-frames-are-fleeting)); and the path is not
thread-safe, so polling it while a Model B playback runs is a data race. A future release will
rework this area to be drain-friendly and thread-safe. **Until then:**

- **Model B: use the audio callback.** It runs on the playback thread, so it's serialized with
  the decoder — the only fully safe audio path there.
- **Model A (or anywhere you must defer): copy the samples immediately** after
  `easyav1_get_audio_frame` and work on your copy. Consume-before-next-decode also works in
  Model A (you pace the decodes yourself), but copying is what makes it robust.

---

## 8. Examples

The examples build on each other: 8.1 is the smallest possible program, 8.2 adds real playback
with SDL, and the rest cover the interesting integration cases (embedded assets, custom I/O,
game-loop pacing). All of them compile against the actual header with `gcc -std=c99 -Wall
-Wextra` (SDL3 examples also against real SDL3 headers).

---

### 8.1 Minimal: decode a whole video, no audio

**Scenario:** you want the absolute smallest program that exercises the library — decode a file,
touch every video frame, report success. Perfect as a smoke test or as the skeleton for a
benchmark.

```c
#include <easyav1.h>
#include <stdio.h>

int main(void)
{
    // Start from the defaults and change only what this tool needs.
    easyav1_settings settings = easyav1_default_settings();
    settings.enable_audio = EASYAV1_FALSE;          // no audio: saves CPU, no "is there audio?" checks
    settings.skip_unprocessed_frames = EASYAV1_FALSE; // we consume every frame, so no dropping
    settings.log_level = EASYAV1_LOG_LEVEL_ERROR;   // keep stdout clean for our own progress output

    easyav1_t *easyav1 = easyav1_init_from_filename("video.webm", &settings);
    if (!easyav1) {
        printf("Failed to initialize easyav1.\n");
        return 1;
    }

    // The core Model-A loop: decode one packet, consume whatever came out.
    while (easyav1_decode_next(easyav1) == EASYAV1_STATUS_OK) {
        if (easyav1_has_video_frame(easyav1) == EASYAV1_TRUE) {
            const easyav1_video_frame *frame = easyav1_get_video_frame(easyav1);
            // In a real app you'd upload frame->data[0..2] to a texture here,
            // using frame->stride[0..2] and frame->properties.width/height.
            (void)frame;
        }
    }

    // Clean end of file, or error? easyav1_is_finished is the definitive check.
    printf(easyav1_is_finished(easyav1) == EASYAV1_TRUE ? "Done.\n" : "Decode failed.\n");

    easyav1_destroy(&easyav1);
    return 0;
}
```

**What to notice:**

- There's no timing code at all — this decodes as fast as the CPU allows. That's fine for
  offline work (transcoding, analysis, benchmarking) but *wrong* for playback; that's what
  `easyav1_decode_for` (8.5) and `easyav1_play` (8.2) are for.
- `easyav1_get_video_frame` is called even though we ignore the frame — calling it is what
  *marks the frame as consumed*, which is what `skip_unprocessed_frames = FALSE` expects.
- `easyav1_destroy(&easyav1)` takes a pointer to the handle and nulls it.

The full version of this, with progress output and statistics, is
[`tools/easyav1_benchmark.c`](tools/easyav1_benchmark.c) — it's the closest thing to "the
reference minimal program" in this repo.

---

### 8.2 Timed playback with callbacks (Model B) + SDL3

**Scenario:** a "just play this video" experience — a window shows the video at the correct
speed while the audio plays on your speakers, with easyAV1's background thread doing all the
pacing. This is the model for cinematics, and it's the pattern used by the bundled
`easyav1_player`.

```c
#include <easyav1.h>
#include <SDL3/SDL.h>
#include <stdio.h>

// App state. (In a real app you'd keep these in a small struct instead of globals.)
static SDL_Renderer  *g_renderer;
static SDL_Texture   *g_video_texture;
static SDL_AudioStream *g_audio_stream;
static unsigned int g_video_width, g_video_height;

// Letterbox the video to fit the (resizable) window, like easyav1_player does.
// (A static, like the player, because SDL_RenderTexture wants a pointer and we can't
// take the address of a function's return value in C.)
static const SDL_FRect *aspect_rect(void)
{
    int width, height;
    SDL_GetCurrentRenderOutputSize(g_renderer, &width, &height);

    float video_aspect = (float) g_video_width / (float) g_video_height;
    float window_aspect = (float) width / (float) height;

    static SDL_FRect rect;
    if (window_aspect > video_aspect) {
        rect.w = height * video_aspect;
        rect.x = (width - rect.w) / 2.0f;
        rect.h = height;
        rect.y = 0;
    } else {
        rect.h = width / video_aspect;
        rect.y = (height - rect.h) / 2.0f;
        rect.w = width;
        rect.x = 0;
    }
    return &rect;
}

static void audio_cb(const easyav1_audio_frame *frame, void *userdata)
{
    (void)userdata;
    // Default settings mean interleaved float32 — exactly what SDL_AudioStream wants.
    // This callback runs on the playback thread, serialized with the decoder —
    // it's the safe audio path in Model B (see 7.2).
    SDL_PutAudioStreamData(g_audio_stream, frame->pcm.interlaced, frame->bytes);
}

int main(void)
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        return 1;
    }

    // 1. Model B (background playback). Delivery is split on purpose:
    //    - audio  -> callback (runs on the playback thread; the safe audio path, see 7.2)
    //    - video  -> no callback: the playback thread then never touches the frame queue,
    //      so the main (render) thread polls easyav1_has_video_frame / easyav1_get_video_frame
    //      and does all SDL rendering calls itself, as SDL requires (see 5.2).
    easyav1_settings settings = easyav1_default_settings();
    // settings.callbacks.video = ...;   // deliberately left unset
    settings.callbacks.audio = audio_cb;
    // Note: audio_offset_time could be set here to trim A/V sync, in ms.

    // 2. Initialize and learn the format so we can build matching SDL objects.
    easyav1_t *easyav1 = easyav1_init_from_filename("video.webm", &settings);
    if (!easyav1) {
        printf("Failed to initialize easyav1.\n");
        SDL_Quit();
        return 1;
    }

    g_video_width = easyav1_get_video_width(easyav1);
    g_video_height = easyav1_get_video_height(easyav1);

    // 3. SDL side: a window sized to the video, a renderer, and an audio stream at
    //    the video's sample rate / channel count with float32 samples.
    SDL_Window *window = SDL_CreateWindow("easyav1", g_video_width, g_video_height,
        SDL_WINDOW_RESIZABLE);
    g_renderer = SDL_CreateRenderer(window, NULL);
    SDL_AudioSpec spec = { 0 };
    spec.freq = (int) easyav1_get_audio_sample_rate(easyav1);
    spec.channels = (int) easyav1_get_audio_channels(easyav1);
    spec.format = SDL_AUDIO_F32;
    g_audio_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
        &spec, NULL, NULL);

    // The audio callback (playback thread) will write into g_audio_stream, so it must
    // exist before easyAV1_play. The texture is main-thread-only (we poll the frames,
    // we don't take a video callback), so no such constraint — but create it before the
    // loop uses it, of course. (SDL can only display 8-bit YUV420 here — easyAV1's
    // default build; see 6.3 for the details.)
    g_video_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_IYUV,
        SDL_TEXTUREACCESS_STREAMING, g_video_width, g_video_height);

    // 4. GO. easyAV1_play starts the background decode thread and returns.
    if (easyav1_play(easyav1) != EASYAV1_STATUS_OK) {
        printf("Failed to start playback.\n");
        easyav1_destroy(&easyav1);
        SDL_Quit();
        return 2;
    }

    // 5. Main loop: keep the app alive, take the newest decoded frame, present it.
    int running = 1;
    while (running && easyav1_get_status(easyav1) == EASYAV1_STATUS_OK) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT ||
                (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE)) {
                running = 0;
            }
        }
        // Video arrives on the render thread: the playback thread decoded it and left it
        // in the queue (no video callback is set, so nobody else consumes it — see 5.2).
        // get_video_frame returns NULL when there's nothing new since the last call, so
        // the texture is only re-uploaded when a new frame actually arrived.
        if (easyav1_has_video_frame(easyav1) == EASYAV1_TRUE) {
            const easyav1_video_frame *v = easyav1_get_video_frame(easyav1);
            if (v) {
                // Upload all three planes in one call: each plane's pointer plus its row
                // pitch. v->stride[i] is the bytes-per-row for plane i — never width,
                // since planes may be padded (the classic smearing bug, see 6.2).
                SDL_UpdateYUVTexture(g_video_texture, NULL,
                    v->data[0], v->stride[0],   // Y
                    v->data[1], v->stride[1],   // U
                    v->data[2], v->stride[2]);  // V
            }
        }
        SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
        SDL_RenderClear(g_renderer);
        SDL_RenderTexture(g_renderer, g_video_texture, NULL, aspect_rect());
        SDL_RenderPresent(g_renderer);
        // Don't busy wait
        SDL_Delay(2);
    }

    // 6. Clean shutdown.
    easyav1_stop(easyav1);
    printf(easyav1_is_finished(easyav1) ? "Finished playing.\n" : "Playback failed.\n");

    easyav1_destroy(&easyav1);
    if (g_video_texture) {
        SDL_DestroyTexture(g_video_texture);
    }
    SDL_Quit();
    return 0;
}
```

**What to notice:**

- **Delivery is split along thread-safety lines, not habit.** Audio *in*, callback: it runs
  on easyAV1's playback thread, serialized with the decoder — the safe audio path in Model B
  (polling the audio buffer from the app thread is a data race, see [7.2](#72-is-there-audio--a-naming-nuance)).
  Video *out*, polling: SDL's render calls must come from the render thread, and easyAV1
  allows exactly this — with no video callback registered the playback thread never consumes
  the frame queue, so polling `easyav1_has_video_frame` / `easyav1_get_video_frame` from the
  app loop is safe (see [5.2](#52-model-b-in-detail-background-playback)). Don't mix them
  around: a *video* callback that renders into SDL from the playback thread, or an *audio*
  poll from the main thread, are the two unsafe directions.
- **The entire upload is one call**: `SDL_UpdateYUVTexture` takes each plane's pointer and
  row pitch. `v->stride[i]` (bytes per row) goes straight into SDL's pitch arguments —
  never `width`, since planes may be padded (the classic smearing bug, see [6.2](#62-the-planes)).
  `easyav1_get_video_frame` returns `NULL` when nothing new is decoded since the last call,
  so the texture is only re-uploaded when a new frame actually arrived — the previous one
  is what's on the texture already.
- **Shared state touched by a callback must exist before `easyav1_play`.** Here that's
  `g_audio_stream`: the audio callback (playback thread) writes into it while the audio
  device thread reads it — SDL serializes that pair, so it's fine. The video texture is
  different: it is main-thread-only, which is precisely why this design has no shared
  mutable state between easyAV1's thread and the render thread at all. If a callback ever
  needs to publish new shared state, *you* have to add the synchronization (see
  [12.1](#121-thread-safety-notes)) — and TSan will catch you if you forget: building a
  variant of this example with a video callback that touches a global the main loop reads
  reports the race the moment it runs.
- **The bundled `easyav1_player` does exactly this** (`tools/easyav1_player.c`): audio via
  callback, video polled in the app loop — plus one extra thing worth copying for real
  content: it builds an `SDL_Colorspace` from the frame's `properties` (range, primaries,
  transfer, matrix) and sets it as a texture-creation property, so limited-range and HD
  clips display with correct color instead of a washed-out or crushed look.
- **The main loop is deliberately *boring*.** All timing happens inside easyAV1; your job is
  only to present what arrived and keep the window alive. That's the entire appeal of Model B.
- `SDL_PutAudioStreamData` is non-blocking — it just queues samples into SDL's ring buffer —
  which makes it a safe thing to call from the decode thread.
- If audio ends up ahead of or behind video, adjust `settings.audio_offset_time` (in ms) and
  re-init or `easyav1_update_settings`.

---

### 8.3 Playing a video embedded in your game (memory buffer)

**Scenario:** the classic game case — the video isn't a file on disk, it's bytes in your asset
pipeline (packed in an archive, baked into the binary, downloaded, …). `easyav1` never cares
where the bytes came from; you hand it a pointer and a size.

```c
#include <easyav1.h>

// Produced by your asset tooling, e.g. `xxd -i intro.webm > intro_webm.h`:
extern const unsigned char intro_webm_data[];
extern const unsigned long intro_webm_size;

easyav1_settings settings = easyav1_default_settings();

easyav1_t *easyav1 = easyav1_init_from_memory(intro_webm_data, intro_webm_size, &settings);
if (!easyav1) {
    // handle error…
}

// … use exactly like any other instance (Model A or B) …

easyav1_destroy(&easyav1);
```

**What to notice:**

- There's nothing else to do — seeking works on memory buffers just like on files, because
  WebM seeks are resolved through the cues index in the (fully in-memory) stream.
- easyAV1 treats the buffer as *read-only borrowed memory*: it will never free it, no matter
  what `close_file_handle_on_destroy` says. If you malloc'd it, free it yourself after
  `easyav1_destroy`.

---

### 8.4 Custom I/O: an `SDL_IOStream`

**Scenario:** you're already loading assets through SDL's I/O layer (or your own archive), and
you'd like easyAV1 to read through it. The `easyav1_stream` interface is a 3-function C-style
vtable — `read`, `seek`, `tell` — and this example is the pattern: wrap any I/O object in
three small trampolines.

The SDL3 player ships with exactly this code — here it is, annotated:

```c
#include <easyav1.h>
#include <SDL3/SDL.h>

// --- Trampoline: easyAV1's read signature → SDL's ReadIO -------------------------
// easyAV1 wants:  1 = got bytes, 0 = clean EOF, -1 = error
static int my_read(void *buffer, size_t size, void *userdata)
{
    SDL_IOStream *s = (SDL_IOStream *)userdata; // `userdata` is how we find our stream
    size_t got = SDL_ReadIO(s, buffer, size);
    if (got > 0) return 1;
    return SDL_GetIOStatus(s) == SDL_IO_STATUS_EOF ? 0 : -1; // distinguish EOF from error
}

// --- Trampoline: easyAV1's seek → SDL's SeekIO ----------------------------------
// SDL uses the same SEEK_SET / SEEK_CUR / SEEK_END values, so this is a straight pass-through.
static int my_seek(int64_t offset, int origin, void *userdata)
{
    SDL_IOStream *s = (SDL_IOStream *)userdata;
    return SDL_SeekIO(s, offset, origin) == -1 ? -1 : 0;
}

// --- Trampoline: easyAV1's tell → SDL's TellIO ----------------------------------
static int64_t my_tell(void *userdata)
{
    return SDL_TellIO((SDL_IOStream *)userdata);
}

// --- Putting it together ---------------------------------------------------------
SDL_IOStream *s = SDL_IOFromFile("video.webm", "rb"); // or SDL_IOFromDynamicArray,
if (!s) return 1;                                      // or your own SDL_IOStream

easyav1_stream stream = { my_read, my_seek, my_tell, s };
easyav1_t *easyav1 = easyav1_init_from_custom_stream(&stream, NULL);
if (!easyav1) {
    SDL_CloseIO(s);
    return 2;
}

// … use easyav1 exactly like in 8.1–8.3 …

SDL_CloseIO(s); // (custom streams are always yours to close — easyav1 never closes them)
```

**What to notice:**

- The same three trampolines work for *any* I/O backend — swap `SDL_IOFromFile` for
  `SDL_IOFromConstDynamicArray` (memory), your archive reader, or a network stream and nothing
  else changes.
- `userdata` is the whole "where's my object?" story: it's the one pointer easyAV1 passes back
  to you, so the vtable can live in global scope and still find per-instance state.
- Custom streams are what let you play videos straight out of packed game assets (PAK files,
  etc.) — the SDL player uses it so you can open files through its dialog, including special
  locations.

---

### 8.5 A game-loop (Model A) pacing skeleton

**Scenario:** you don't want a cinematic that plays at its own speed — you want *your* game
loop to own time: pause when the player pauses, slow-motion on demand, fast-forward, etc.
That's Model A driven by `easyav1_decode_for`, synced to SDL's tick.

This is a skeleton — the render/audio plumbing is elided on purpose, but the *structure* is
exactly what a real integration looks like:

```c
#include <easyav1.h>
#include <SDL3/SDL.h>

// Your app state: window, renderer, YUV video texture, SDL_AudioStream for the device, …
// SDL_AudioStream *audio_stream = /* created at your audio spec */;
// easyav1_t *easyav1 = /* initialized as in 8.1/8.3, with skip_unprocessed_frames as you like */;

int running = 1;
Uint64 last_tick = SDL_GetTicks();
int paused = 0;

while (running) {
    // --- 1. Figure out how much *media time* should be decoded this frame -------
    Uint64 now = SDL_GetTicks();
    easyav1_timestamp elapsed = (easyav1_timestamp)(now - last_tick);
    last_tick = now;

    if (!paused) {
        // Decode exactly the media time that elapsed in real time.
        // This is the entire sync mechanism: 1 real ms in, 1 media ms out.
        easyav1_decode_for(easyav1, elapsed);
    }

    // --- 2. Video: show the newest frame (or hold the last one while paused) ----
    if (easyav1_has_video_frame(easyav1) == EASYAV1_TRUE) {
        const easyav1_video_frame *v = easyav1_get_video_frame(easyav1);
        // Upload v->data[0..2] to your YUV texture (stride-aware), then render.
    }

    // --- 3. Audio: drain whatever samples are ready ------------------------------
    // (Don't wait for is_audio_buffer_filled — that's overflow-imminent, see 7.2.)
    const easyav1_audio_frame *a;
    while ((a = easyav1_get_audio_frame(easyav1)) != NULL) {
        SDL_PutAudioStreamData(audio_stream, a->pcm.interlaced, a->bytes);
    }

    // --- 4. Input ----------------------------------------------------------------
    // Space      → paused = !paused;   (while paused, just skip the decode_for call)
    // → / ←      → easyav1_seek_to_timestamp(easyav1,
    //                  easyav1_get_current_timestamp(easyav1) ± 5000);
    // Escape     → running = 0;
}
```

**What to notice:**

- **The whole sync trick is one line**: `easyav1_decode_for(easyav1, elapsed)`. Because the
  decode budget tracks real elapsed time, the video plays at the right speed no matter what
  your frame rate is doing, and pausing is trivial (stop asking for time).
- **`skip_unprocessed_frames` matters more here**: at 30 fps your loop will decode several
  video frames per iteration; with the default `EASYAV1_TRUE`, stale frames are dropped and you
  always render the freshest one — exactly what a real-time player wants.
- Audio and video decouple naturally: audio arrives in Vorbis block-sized chunks, video in
  frames; both are consumed as they become available, and each side's buffering (SDL's audio
  stream, your texture upload) absorbs the jitter.

---

### 8.6 Where to look in this repo

- [`tools/easyav1_benchmark.c`](tools/easyav1_benchmark.c) — the simplest complete program:
  Model A, no audio, statistics. ~190 lines.
- [`tools/easyav1_player.c`](tools/easyav1_player.c) — the full real-world integration:
  custom SDL3 `IOStream` (8.4), YUV texture rendering, `SDL_AudioStream`, seeking,
  multi-track selection, A/V offset UI, pause/resume. Read it top to bottom; it's the
  definitive "how do I actually do this" answer.

---

## 9. Seeking

Seeking is one of the most useful features and one of the easiest to misuse, so the rules are
short:

```c
easyav1_seek_to_timestamp(easyav1, 30000);  // jump to 30.000 s
easyav1_seek_forward(easyav1, 5000);        // +5 s from the current position
easyav1_seek_backward(easyav1, 5000);       // -5 s from the current position
```

- **Units are milliseconds** — `30000`, not `30` and not `30000000000`.
- **Both models: just seek.** In Model B the call sets a request flag and the playback thread
  performs the seek after its current decode step, then continues playing from the new position —
  no need to stop first. In Model A the seek runs directly on your thread. Either way, don't call
  it from inside a callback — in Model A that would re-enter the decode state machine
  mid-iteration (and callbacks are *on* the decode path).
- **Accurate vs fast.** By default, easyAV1 seeks to the *exact* timestamp: it jumps back to
  the previous keyframe and decodes forward until it reaches your target — accurate, at the cost
  of decoding a few frames. With `use_fast_seeking = EASYAV1_TRUE` it lands on the nearest
  keyframe *before* the target — much faster, but you may appear a fraction of a second early.
  For a player UI with a scrub bar, fast is usually the right feel; for "jump to chapter
  mark" accuracy, keep the default.
- **Where are you now?** `easyav1_get_current_timestamp(easyav1)` returns the current position in
  ms — perfect for a progress bar (`position / easyav1_get_duration(easyav1)`), or as the base
  for relative seeks like the ±5 s above.
- **WebM's cues index** is what makes seeking O(1)-ish; files encoded without cues will seek by
  scanning, which is slow but still correct.

---

## 10. API quick reference

| Category | Functions |
| --- | --- |
| Lifecycle | [`easyav1_default_settings`](#easyav1_default_settings), [`easyav1_init_from_filename`](#easyav1_init_from_filename), [`easyav1_init_from_file`](#easyav1_init_from_file), [`easyav1_init_from_memory`](#easyav1_init_from_memory), [`easyav1_init_from_custom_stream`](#easyav1_init_from_custom_stream), [`easyav1_destroy`](#easyav1_destroy) |
| Settings | [`easyav1_get_current_settings`](#easyav1_get_current_settings), [`easyav1_update_settings`](#easyav1_update_settings) |
| Manual decoding (Model A) | [`easyav1_decode_next`](#easyav1_decode_next), [`easyav1_decode_until`](#easyav1_decode_until), [`easyav1_decode_for`](#easyav1_decode_for) |
| Playback (Model B) | [`easyav1_play`](#easyav1_play), [`easyav1_stop`](#easyav1_stop), [`easyav1_get_status`](#easyav1_get_status) |
| Seeking | [`easyav1_seek_to_timestamp`](#easyav1_seek_to_timestamp), [`easyav1_seek_forward`](#easyav1_seek_forward), [`easyav1_seek_backward`](#easyav1_seek_backward), [`easyav1_get_current_timestamp`](#easyav1_get_current_timestamp) |
| Video frames | [`easyav1_has_video_frame`](#easyav1_has_video_frame), [`easyav1_get_video_frame`](#easyav1_get_video_frame), [`easyav1_get_total_video_frames_processed`](#easyav1_get_total_video_frames_processed) |
| Audio frames | [`easyav1_is_audio_buffer_filled`](#easyav1_is_audio_buffer_filled), [`easyav1_get_audio_frame`](#easyav1_get_audio_frame) |
| Introspection | [`easyav1_has_video_track`](#easyav1_has_video_track), [`easyav1_has_audio_track`](#easyav1_has_audio_track), [`easyav1_get_total_video_tracks`](#easyav1_get_total_video_tracks), [`easyav1_get_total_audio_tracks`](#easyav1_get_total_audio_tracks), [`easyav1_get_video_width`](#easyav1_get_video_width), [`easyav1_get_video_height`](#easyav1_get_video_height), [`easyav1_get_video_fps`](#easyav1_get_video_fps), [`easyav1_get_audio_channels`](#easyav1_get_audio_channels), [`easyav1_get_audio_sample_rate`](#easyav1_get_audio_sample_rate), [`easyav1_get_duration`](#easyav1_get_duration), [`easyav1_is_finished`](#easyav1_is_finished) |

The full documentation for every function — parameters, return values, edge cases — is in
the next chapter, [11. API full reference](#11-api-full-reference), which transcribes the
comments from [`src/easyav1.h`](src/easyav1.h).

---

## 11. API full reference

This chapter is a complete reference to [`src/easyav1.h`](src/easyav1.h): every type,
every setting, and every function, with the documentation that ships with the header. The
header is the source of truth — if you ever spot a discrepancy between the two, they should
be fixed to agree.

A few global facts apply to everything below:

- The API is plain C99, wrapped in `extern "C"` so it links cleanly from C++.
- **All timestamps are in milliseconds** — frame timestamps, duration, seek positions, the
  `audio_offset_time` setting, everything.
- The library is a single opaque handle (`easyav1_t`) plus a single settings struct
  (`easyav1_settings`); there are no other public state objects.

### 11.1 Basic types

`easyav1_t` — the easyav1 instance. The main structure holding all instance state. An
opaque pointer: you never touch its internals.

`easyav1_timestamp` — the timestamp type (`uint64_t`), used for all timestamp-related
operations. **All timestamps exposed by easyAV1 (frame timestamps, duration, seek positions,
etc.) are in milliseconds.**

`easyav1_bool` — the boolean enumeration:

| Value | Meaning |
| --- | --- |
| `EASYAV1_FALSE = 0` | False |
| `EASYAV1_TRUE = 1` | True |

### 11.2 Status codes

`easyav1_status` — decoder status, either returned by specific functions or by
`easyav1_get_status`. Two groups:

**Returned by functions** (`EASYAV1_STATUS_FINISHED` only by the `easyav1_decode_*` family):

| Value | Meaning |
| --- | --- |
| `EASYAV1_STATUS_ERROR = 0` | An error occurred |
| `EASYAV1_STATUS_OK = 1` | Success |
| `EASYAV1_STATUS_FINISHED = 2` | End of stream reached |

**Returned only by `easyav1_get_status`** (the detailed error codes, telling you *why* it
failed):

| Value | Meaning |
| --- | --- |
| `EASYAV1_STATUS_INVALID_ARGUMENT = -1` | Invalid argument |
| `EASYAV1_STATUS_OUT_OF_MEMORY = -2` | Out of memory |
| `EASYAV1_STATUS_IO_ERROR = -3` | I/O error |
| `EASYAV1_STATUS_DECODER_ERROR = -4` | Decoder error |
| `EASYAV1_STATUS_NOT_IMPLEMENTED = -5` | Not implemented |
| `EASYAV1_STATUS_INVALID_STATE = -6` | Invalid state |
| `EASYAV1_STATUS_INVALID_DATA = -7` | Invalid data |
| `EASYAV1_STATUS_UNSUPPORTED = -8` | Unsupported |

### 11.3 Custom streams

For feeding easyAV1 from anywhere, you provide three callbacks:

`easyav1_read_func` — a function that reads data from a source.

```c
typedef int (*easyav1_read_func)(void *buffer, size_t size, void *userdata);
```

| | |
| --- | --- |
| **`buffer`** | The buffer to read the data into. |
| **`size`** | The size of the buffer. |
| **`userdata`** | Custom optional user-defined data. |
| **Returns** | `1` if `size` bytes were read, `0` if the end of the stream was reached, `-1` if there was an error. |

`easyav1_seek_func` — a function that seeks to a specific position in a source.

```c
typedef int (*easyav1_seek_func)(int64_t offset, int origin, void *userdata);
```

| | |
| --- | --- |
| **`offset`** | The offset to seek to. |
| **`origin`** | The origin of the seek operation: `SEEK_SET` sets the pointer to `offset` bytes after the start of the buffer; `SEEK_CUR` sets the pointer to `offset` bytes after the current position; `SEEK_END` sets the pointer relative to the end of the buffer (negative offsets count backwards from the end). |
| **`userdata`** | Custom optional user-defined data. |
| **Returns** | `0` if the seek was successful, another value if there was an error. |

`easyav1_tell_func` — a function that returns the current position in a source.

```c
typedef int64_t (*easyav1_tell_func)(void *userdata);
```

| | |
| --- | --- |
| **`userdata`** | Custom optional user-defined data. |
| **Returns** | The current position in the source, or `-1` if there was an error. |

`easyav1_stream` — the custom stream structure you pass to
`easyav1_init_from_custom_stream`:

```c
typedef struct {
    easyav1_read_func read_func;  // Read data from the stream.
    easyav1_seek_func seek_func;  // Seek to a specific position in the stream.
    easyav1_tell_func tell_func;  // Return the current position in the stream.
    void *userdata;               // Optional user-defined data, passed to read/seek/tell.
} easyav1_stream;
```

You must provide all three functions (`read_func`, `seek_func`, `tell_func`); `userdata` is
an optional pointer to user-defined data that is passed to each of them.

### 11.4 Video types

`easyav1_pixel_layout` — video pixel layouts:
`EASYAV1_PIXEL_LAYOUT_UNKNOWN` (0), `EASYAV1_PIXEL_LAYOUT_YUV400` (1),
`EASYAV1_PIXEL_LAYOUT_YUV420` (2), `EASYAV1_PIXEL_LAYOUT_YUV422` (3),
`EASYAV1_PIXEL_LAYOUT_YUV444` (4).

`easyav1_color_space` — video color range:
`EASYAV1_COLOR_SPACE_UNKNOWN` (0), `EASYAV1_COLOR_SPACE_LIMITED` (1, TV range),
`EASYAV1_COLOR_SPACE_FULL` (2, PC range).

`easyav1_bits_per_color` — bits per color:
`EASYAV1_BITS_PER_COLOR_UNKNOWN` (0), `EASYAV1_BITS_PER_COLOR_8` (1),
`EASYAV1_BITS_PER_COLOR_10` (2), `EASYAV1_BITS_PER_COLOR_12` (3).

`easyav1_color_primaries` — video color primaries:
`EASYAV1_COLOR_PRIMARIES_UNSPECIFIED`, `_BT709`, `_UNKNOWN`, `_BT470M`, `_BT470BG`,
`_BT601`, `_SMPTE240`, `_FILM`, `_BT2020`, `_XYZ`, `_SMPTE431`, `_SMPTE432`, `_EBU3213`
(values 0–12).

`easyav1_transfer_characteristics` — video transfer characteristics (the gamma/PQ/HLG
curve): `EASYAV1_TRANSFER_CHARACTERISTICS_UNSPECIFIED`, `_BT709`, `_UNKNOWN`, `_BT470M`,
`_BT470BG`, `_BT601`, `_SMPTE240`, `_LINEAR`, `_LOG_100`, `_LOG_100_SQRT`, `_IEC61966`,
`_BT1361`, `_SRGB`, `_BT2020_10`, `_BT2020_12`, `_SMPTE2084`, `_SMPTE428`, `_HLG`
(values 0–17).

`easyav1_matrix_coefficients` — video matrix coefficients (the YUV↔RGB matrix):
`EASYAV1_MATRIX_COEFFICIENTS_UNSPECIFIED`, `_IDENTITY`, `_BT709`, `_UNKNOWN`, `_FCC`,
`_BT470BG`, `_BT601`, `_SMPTE240`, `_SMPTE_YCGCO`, `_BT2020_NCL`, `_BT2020_CL`,
`_SMPTE2085`, `_CHROMATICITY_NCL`, `_CHROMATICITY_CL`, `_ICTCP` (values 0–14).

`easyav1_chroma_sample_position` — where the chroma samples sit, relative to luma:
`EASYAV1_CHROMA_SAMPLE_POSITION_UNKNOWN` (0), `_VERTICAL` (1), `_COLOCATED` (2).

`easyav1_video_frame` — a decoded video frame:

```c
typedef struct {
    struct {
        easyav1_pixel_layout pixel_layout;                         // The pixel layout.
        easyav1_bits_per_color bits_per_color;                     // The bits per color.
        easyav1_color_space color_space;                           // The color space.
        easyav1_color_primaries color_primaries;                   // The color primaries.
        easyav1_transfer_characteristics transfer_characteristics; // The transfer characteristics.
        easyav1_matrix_coefficients matrix_coefficients;           // The matrix coefficients.
        easyav1_chroma_sample_position chroma_sample_position;     // The chroma sample position.
        unsigned int width;                                        // The width of the frame.
        unsigned int height;                                       // The height of the frame.
    } properties;
    easyav1_timestamp timestamp;   // The timestamp of the frame (ms).
    const void *data[3];           // The data for each YUV plane.
    size_t stride[3];              // The stride (bytes per row) for each YUV plane.
} easyav1_video_frame;
```

See [6. Video frames](#6-video-frames) for how to interpret the planes and the strides.

### 11.5 Audio types

`easyav1_audio_frame` — a decoded audio frame:

```c
typedef struct {
    unsigned int channels; // Number of channels.
    unsigned int samples;  // Number of samples in this frame.
    easyav1_timestamp timestamp; // The timestamp of the frame (ms).
    size_t bytes; // Number of bytes in this frame. Equal to `samples * sizeof(float) * channels`
                  // if `interlace_audio` is 1, and `samples * sizeof(float)` if it is 0.
    union {
        const float **deinterlaced; // Deinterlaced audio samples.
        const float *interlaced;    // Interlaced audio samples.
    } pcm; // Either deinterlaced or interlaced, depending on the `interlace_audio` setting.
} easyav1_audio_frame;
```

You may only use the `pcm` member that matches the `interlace_audio` setting you decoded
with. See [7. Audio frames](#7-audio-frames) for the consumption semantics.

### 11.6 Callback types

```c
// Video and audio callbacks.
typedef void (*easyav1_video_callback)(const easyav1_video_frame *frame, void *userdata);
typedef void (*easyav1_audio_callback)(const easyav1_audio_frame *frame, void *userdata);
```

### 11.7 Log levels

`easyav1_log_level_t`:

| Value | Meaning |
| --- | --- |
| `EASYAV1_LOG_LEVEL_ERROR` | Only errors are logged |
| `EASYAV1_LOG_LEVEL_WARNING` | Errors and warnings are logged |
| `EASYAV1_LOG_LEVEL_INFO` | Errors, warnings, and info messages are logged (most useful for debugging) |

### 11.8 Settings (`easyav1_settings`)

Before initializing the instance, get the defaults with `easyav1_default_settings()` and
modify what you need. The fields:

| Field | Description |
| --- | --- |
| `easyav1_bool enable_video` | Whether video decoding is enabled. |
| `easyav1_bool enable_audio` | Whether audio decoding is enabled. |
| `easyav1_bool skip_unprocessed_frames` | If `EASYAV1_TRUE`, the decoder skips frames that have not been processed by the video callback. |
| `easyav1_bool interlace_audio` | If `EASYAV1_TRUE`, audio samples are interleaved (`pcm.interlaced`); if `EASYAV1_FALSE`, deinterleaved (`pcm.deinterlaced`). You can only use the field that matches this setting. |
| `easyav1_bool close_file_handle_on_destroy` | Whether `easyav1_destroy` closes the `FILE` passed to `easyav1_init_from_file`. Ownership decides what happens on destroy: with `easyav1_init_from_filename` easyAV1 opened the file itself, so it *always* closes it (this setting does not apply); with `easyav1_init_from_file` the file belongs to you — `EASYAV1_TRUE` means easyAV1 closes it on destroy, `EASYAV1_FALSE` leaves it open for you; with `easyav1_init_from_memory` and `easyav1_init_from_custom_stream` your data/I-O is never freed or closed by easyAV1 — it only frees its own internal bookkeeping. |
| `struct { easyav1_video_callback video; easyav1_audio_callback audio; void *userdata; } callbacks` | The callbacks to use. `video`/`audio` may be `NULL` (no callback of that kind); `userdata` is passed to both. Callback format: `void callback(const easyav1_video_frame *frame, void *userdata)` (or the audio variant). |
| `unsigned int video_track` | The video track to use. 0-indexed, counted over the file's *video* tracks only (e.g. with video, audio, video the video tracks are 0 and 1). |
| `unsigned int audio_track` | The audio track to use. 0-indexed, counted over the file's *audio* tracks only (e.g. with video, audio, video the audio track is 0). |
| `easyav1_bool use_fast_seeking` | If enabled, easyAV1 seeks to the nearest keyframe *before* the requested timestamp; otherwise it seeks to the requested timestamp itself, which is slower because every frame between the keyframe and the timestamp must be decoded. |
| `int64_t audio_offset_time` | The audio offset relative to video, in milliseconds. Negative = audio plays earlier; positive = audio plays later (useful for correcting A/V sync drift, e.g. from audio buffering). When using SDL2 for audio, set the adjustment to `(SDL_AudioSpec.samples / SDL_AudioSpec.freq)`. Note the value is additionally offset by the WebM's internal audio delay. |
| `easyav1_log_level_t log_level` | One of the `easyav1_log_level_t` values above. |

### 11.9 Functions

#### `easyav1_default_settings`

```c
easyav1_settings easyav1_default_settings(void);
```

Returns the default settings for easyav1:

- Video enabled (`.enable_video = EASYAV1_TRUE`)
- Audio enabled (`.enable_audio = EASYAV1_TRUE`)
- Skip unprocessed frames (`.skip_unprocessed_frames = EASYAV1_TRUE`)
- Interlace audio (`.interlace_audio = EASYAV1_TRUE`)
- Don't close the handle on destroy (`.close_file_handle_on_destroy = EASYAV1_FALSE`)
- No callbacks (`callbacks.video = NULL`, `callbacks.audio = NULL`, `callbacks.userdata = NULL`)
- Video track 0 (`.video_track = 0`)
- Audio track 0 (`.audio_track = 0`)
- Don't use fast seeking (`.use_fast_seeking = EASYAV1_FALSE`)
- No audio offset time (`.audio_offset_time = 0`)
- Log level warning (`.log_level = EASYAV1_LOG_LEVEL_WARNING`)

**Returns** — the default settings.

#### `easyav1_init_from_filename`

```c
easyav1_t *easyav1_init_from_filename(const char *filename, const easyav1_settings *settings);
```

Initializes an easyav1 instance from a file.

| | |
| --- | --- |
| **`filename`** | The filename of the file to open. |
| **`settings`** | The settings to use for the easyav1 instance. If `NULL`, the default settings are used. |
| **Returns** | The `easyav1` instance, or `NULL` if an error occurred. |

> easyAV1 opens and therefore owns the file: it will always be closed by `easyav1_destroy`.

#### `easyav1_init_from_file`

```c
easyav1_t *easyav1_init_from_file(FILE *f, const easyav1_settings *settings);
```

Initializes an easyav1 instance from a `FILE` handle.

| | |
| --- | --- |
| **`f`** | The open `FILE` handle. |
| **`settings`** | The settings to use for the easyav1 instance. If `NULL`, the default settings are used. |
| **Returns** | The `easyav1` instance, or `NULL` if an error occurred. |

> You own the handle. Set `close_file_handle_on_destroy` in the settings if you want
> `easyav1_destroy` to close it for you; otherwise close it yourself.

#### `easyav1_init_from_memory`

```c
easyav1_t *easyav1_init_from_memory(const void *data, size_t size, const easyav1_settings *settings);
```

Initializes an easyav1 instance from a memory buffer.

| | |
| --- | --- |
| **`data`** | The buffer to read from. |
| **`size`** | The size of the buffer. |
| **`settings`** | The settings to use for the easyav1 instance. If `NULL`, the default settings are used. |
| **Returns** | The `easyav1` instance, or `NULL` if an error occurred. |

#### `easyav1_init_from_custom_stream`

```c
easyav1_t *easyav1_init_from_custom_stream(const easyav1_stream *stream, const easyav1_settings *settings);
```

Initializes an easyav1 instance from a custom stream (see [11.3](#113-custom-streams)).

| | |
| --- | --- |
| **`stream`** | The custom stream to read from. |
| **`settings`** | The settings to use for the easyav1 instance. If `NULL`, the default settings are used. |
| **Returns** | The `easyav1` instance, or `NULL` if an error occurred. |

#### `easyav1_destroy`

```c
void easyav1_destroy(easyav1_t **easyav1);
```

Destroys an easyav1 instance. Note it takes the *address* of your instance handle (a double
pointer): it destroys the instance and clears your pointer for you.

| | |
| --- | --- |
| **`easyav1`** | The easyav1 instance to destroy. |

#### `easyav1_get_current_settings`

```c
easyav1_settings easyav1_get_current_settings(const easyav1_t *easyav1);
```

Gets the current settings of the easyav1 instance.

| | |
| --- | --- |
| **`easyav1`** | The easyav1 instance. |
| **Returns** | The settings of the easyav1 instance. |

#### `easyav1_update_settings`

```c
easyav1_status easyav1_update_settings(easyav1_t *easyav1, const easyav1_settings *settings);
```

Updates the settings of the easyav1 instance. Only the settings that differ from the current
ones are updated.

| | |
| --- | --- |
| **`easyav1`** | The easyav1 instance. |
| **`settings`** | The new settings to use. |
| **Returns** | `EASYAV1_STATUS_OK` if the settings were updated, `EASYAV1_STATUS_ERROR` if there was an error. |

#### `easyav1_decode_next`

```c
easyav1_status easyav1_decode_next(easyav1_t *easyav1);
```

Decodes the next packet (Model A — manual decoding, see [5.1](#51-model-a-in-detail-the-manual-decode-loop)).

| | |
| --- | --- |
| **`easyav1`** | The easyav1 instance. |
| **Returns** | `EASYAV1_STATUS_OK` if a packet was decoded, `EASYAV1_STATUS_FINISHED` if the end of the stream was reached, or `EASYAV1_STATUS_ERROR` if there was an error. |

#### `easyav1_decode_until`

```c
easyav1_status easyav1_decode_until(easyav1_t *easyav1, easyav1_timestamp timestamp);
```

Decodes the next packets until the specified timestamp.

| | |
| --- | --- |
| **`easyav1`** | The easyav1 instance. |
| **`timestamp`** | The timestamp to decode until. |
| **Returns** | `EASYAV1_STATUS_OK` if a packet was decoded, `EASYAV1_STATUS_FINISHED` if the end of the stream was reached, or `EASYAV1_STATUS_ERROR` if there was an error. |

#### `easyav1_decode_for`

```c
easyav1_status easyav1_decode_for(easyav1_t *easyav1, easyav1_timestamp time);
```

Decodes the next packets that appear within the specified duration (i.e. up to
`current timestamp + time`).

| | |
| --- | --- |
| **`easyav1`** | The easyav1 instance. |
| **`time`** | The duration to decode for. |
| **Returns** | `EASYAV1_STATUS_OK` if a packet was decoded, `EASYAV1_STATUS_FINISHED` if the end of the stream was reached, or `EASYAV1_STATUS_ERROR` if there was an error. |

#### `easyav1_play`

```c
easyav1_status easyav1_play(easyav1_t *easyav1);
```

Starts playing the video and audio (Model B — background playback, see
[5.2](#52-model-b-in-detail-background-playback)). Decoding runs in the background, synced
with elapsed time. If the video/audio callbacks are set, they are called; otherwise you must
get video and audio from `easyav1_get_video_frame` and `easyav1_get_audio_frame`
respectively. This function does not block — it returns immediately after starting the
decoding process. Call it after initializing the instance and setting the callbacks.

> This function runs on its own thread, so the callbacks must be thread-safe.
>
> You can seek while playing by calling `easyav1_seek_to_timestamp` (or
> `easyav1_seek_forward`/`easyav1_seek_backward`) directly — the playback thread performs
> the seek after its current decode step and continues playing from the new position. There
> is no need to call `easyav1_stop` first. Do not call seek functions (or any other easyAV1
> functions) from inside a callback.

| | |
| --- | --- |
| **`easyav1`** | The easyav1 instance. |
| **Returns** | `EASYAV1_STATUS_OK` if successful, `EASYAV1_STATUS_ERROR` if there was an error. |

#### `easyav1_stop`

```c
void easyav1_stop(easyav1_t *easyav1);
```

Stops the playback of the video and audio. The decoding process stops, the video/audio
callbacks are not called anymore, and the background decoding thread is stopped. Resume by
calling `easyav1_play` again.

| | |
| --- | --- |
| **`easyav1`** | The easyav1 instance. |

#### `easyav1_get_status`

```c
easyav1_status easyav1_get_status(easyav1_t *easyav1);
```

Indicates the current status of the easyav1 instance. The status is one of:

- `EASYAV1_STATUS_OK` — everything is fine.
- `EASYAV1_STATUS_FINISHED` — the end of the stream has been reached.
- `EASYAV1_STATUS_ERROR` — there was an error (see the detailed codes in
  [11.2](#112-status-codes)).

| | |
| --- | --- |
| **`easyav1`** | The easyav1 instance. |
| **Returns** | The current status of the easyav1 instance. |

#### `easyav1_get_current_timestamp`

```c
easyav1_timestamp easyav1_get_current_timestamp(easyav1_t *easyav1);
```

Gets the current timestamp of the parsing (in milliseconds).

| | |
| --- | --- |
| **`easyav1`** | The easyav1 instance. |
| **Returns** | The current timestamp. |

#### `easyav1_seek_to_timestamp`

```c
easyav1_status easyav1_seek_to_timestamp(easyav1_t *easyav1, easyav1_timestamp timestamp);
```

Seeks to a specified timestamp. Works while playing (see the `easyav1_play` notes above and
[9. Seeking](#9-seeking)).

| | |
| --- | --- |
| **`easyav1`** | The easyav1 instance. |
| **`timestamp`** | The timestamp to seek to. |
| **Returns** | `EASYAV1_STATUS_OK` if successful, `EASYAV1_STATUS_ERROR` if there was an error. |

#### `easyav1_seek_forward`

```c
easyav1_status easyav1_seek_forward(easyav1_t *easyav1, easyav1_timestamp time);
```

Seeks forward by the specified duration.

| | |
| --- | --- |
| **`easyav1`** | The easyav1 instance. |
| **`time`** | The duration to seek forward by. |
| **Returns** | `EASYAV1_STATUS_OK` if successful, `EASYAV1_STATUS_ERROR` if there was an error. |

#### `easyav1_seek_backward`

```c
easyav1_status easyav1_seek_backward(easyav1_t *easyav1, easyav1_timestamp time);
```

Seeks backward by the specified duration.

| | |
| --- | --- |
| **`easyav1`** | The easyav1 instance. |
| **`time`** | The duration to seek backward by. |
| **Returns** | `EASYAV1_STATUS_OK` if successful, `EASYAV1_STATUS_ERROR` if there was an error. |

#### `easyav1_has_video_frame`

```c
easyav1_bool easyav1_has_video_frame(easyav1_t *easyav1);
```

Indicates whether there is a video frame ready to be displayed.

| | |
| --- | --- |
| **`easyav1`** | The easyav1 instance. |
| **Returns** | `EASYAV1_TRUE` if a video frame is available, `EASYAV1_FALSE` otherwise. |

#### `easyav1_get_video_frame`

```c
const easyav1_video_frame *easyav1_get_video_frame(easyav1_t *easyav1);
```

Gets the current video frame, if one is available. The returned frame is only valid until
the next call to `easyav1_get_video_frame`. Calling this function marks the frame as
displayed, so you only receive a decoded frame once.

| | |
| --- | --- |
| **`easyav1`** | The easyav1 instance. |
| **Returns** | A pointer to the video frame, or `NULL` if no frame is available. |

#### `easyav1_get_total_video_frames_processed`

```c
uint64_t easyav1_get_total_video_frames_processed(easyav1_t *easyav1);
```

Gets the total number of video frames processed up to this point.

| | |
| --- | --- |
| **`easyav1`** | The easyav1 instance. |
| **Returns** | The total number of video frames processed. |

#### `easyav1_is_audio_buffer_filled`

```c
easyav1_bool easyav1_is_audio_buffer_filled(const easyav1_t *easyav1);
```

Indicates whether the audio sample ring is **completely full**. "Filled" means the ring
(4096 samples per channel) has no free space left: the next audio decode will discard the
oldest unprocessed samples to make room. So `EASYAV1_TRUE` is a *warning* ("consume now or
lose samples"), not a "ready to consume" signal. Waiting for fullness before consuming costs
up to a full buffer of latency and, in background playback, continuous sample loss.

Prefer draining `easyav1_get_audio_frame` (consume whatever is in the ring, even a few
samples) over waiting for the buffer to fill.

> **Known design issue:** the audio buffer API is expected to change in a future release to
> be drain-friendly and thread-safe. In background playback (`easyav1_play`), the audio
> callback is the safe delivery path — the audio path is not synchronized, so polling it from
> another thread is a data race.

| | |
| --- | --- |
| **`easyav1`** | The easyav1 instance. |
| **Returns** | `EASYAV1_TRUE` if the audio buffer is completely full, `EASYAV1_FALSE` otherwise. |

See [7.2](#72-is-there-audio--a-naming-nuance) for the safe consumption patterns.

#### `easyav1_get_audio_frame`

```c
const easyav1_audio_frame *easyav1_get_audio_frame(easyav1_t *easyav1);
```

Gets the current audio frame, if there are available samples. The frame contains whatever
samples are currently in the ring (even a few); see the `samples` and `bytes` fields for how
much.

The samples are *not* committed when you get the frame: the next audio decode (any
`easyav1_decode_*` call, or the playback thread's next decode in background playback) can
overwrite them in place and rewrite the frame's `samples`/`timestamp`/`bytes` fields. Consume
the frame before the next decode — or copy the samples if you need to keep them.

Calling this function marks the samples in the frame as consumed, so you will only receive
them once.

> **Known design issue:** the audio buffer API is expected to change in a future release to
> be drain-friendly and thread-safe. In background playback (`easyav1_play`), the audio
> callback is the safe delivery path — the audio path is not synchronized, so polling it from
> another thread is a data race.

| | |
| --- | --- |
| **`easyav1`** | The easyav1 instance. |
| **Returns** | A pointer to the audio frame, or `NULL` if there are no samples to process. |

#### `easyav1_has_video_track`

```c
easyav1_bool easyav1_has_video_track(const easyav1_t *easyav1);
```

Indicates whether there is a video track in the file.

| | |
| --- | --- |
| **`easyav1`** | The easyav1 instance. |
| **Returns** | `EASYAV1_TRUE` if there is a video track, `EASYAV1_FALSE` otherwise. |

#### `easyav1_has_audio_track`

```c
easyav1_bool easyav1_has_audio_track(const easyav1_t *easyav1);
```

Indicates whether there is an audio track in the file.

| | |
| --- | --- |
| **`easyav1`** | The easyav1 instance. |
| **Returns** | `EASYAV1_TRUE` if there is an audio track, `EASYAV1_FALSE` otherwise. |

#### `easyav1_get_total_video_tracks`

```c
unsigned int easyav1_get_total_video_tracks(const easyav1_t *easyav1);
```

Gets the total number of video tracks in the file.

| | |
| --- | --- |
| **`easyav1`** | The easyav1 instance. |
| **Returns** | The total number of video tracks in the file, or `0` if there was an error or there are no video tracks. |

#### `easyav1_get_total_audio_tracks`

```c
unsigned int easyav1_get_total_audio_tracks(const easyav1_t *easyav1);
```

Gets the total number of audio tracks in the file.

| | |
| --- | --- |
| **`easyav1`** | The easyav1 instance. |
| **Returns** | The total number of audio tracks in the file, or `0` if there was an error or there are no audio tracks. |

#### `easyav1_get_video_width`

```c
unsigned int easyav1_get_video_width(const easyav1_t *easyav1);
```

Gets the width of the video track.

| | |
| --- | --- |
| **`easyav1`** | The easyav1 instance. |
| **Returns** | The width of the video track, `0` if there was an error or there is no video track. |

#### `easyav1_get_video_height`

```c
unsigned int easyav1_get_video_height(const easyav1_t *easyav1);
```

Gets the height of the video track.

| | |
| --- | --- |
| **`easyav1`** | The easyav1 instance. |
| **Returns** | The height of the video track, or `0` if there was an error or there is no video track. |

#### `easyav1_get_video_fps`

```c
unsigned int easyav1_get_video_fps(const easyav1_t *easyav1);
```

Gets the FPS of the video track.

| | |
| --- | --- |
| **`easyav1`** | The easyav1 instance. |
| **Returns** | The FPS of the video track, or `0` if there was an error or there is no video track. |

#### `easyav1_get_audio_channels`

```c
unsigned int easyav1_get_audio_channels(const easyav1_t *easyav1);
```

Gets the number of audio channels.

| | |
| --- | --- |
| **`easyav1`** | The easyav1 instance. |
| **Returns** | The number of audio channels, or `0` if there was an error or there is no audio track. |

#### `easyav1_get_audio_sample_rate`

```c
unsigned int easyav1_get_audio_sample_rate(const easyav1_t *easyav1);
```

Gets the audio sample rate.

| | |
| --- | --- |
| **`easyav1`** | The easyav1 instance. |
| **Returns** | The audio sample rate, or `0` if there was an error or there is no audio track. |

#### `easyav1_get_duration`

```c
easyav1_timestamp easyav1_get_duration(const easyav1_t *easyav1);
```

Gets the duration of the file.

| | |
| --- | --- |
| **`easyav1`** | The easyav1 instance. |
| **Returns** | The duration of the file (in milliseconds), or `0` if there was an error. |

#### `easyav1_is_finished`

```c
easyav1_bool easyav1_is_finished(easyav1_t *easyav1);
```

Indicates whether the easyav1 instance has finished decoding the file.

| | |
| --- | --- |
| **`easyav1`** | The easyav1 instance. |
| **Returns** | `EASYAV1_TRUE` if the instance has finished decoding, `EASYAV1_FALSE` otherwise. |

---

## 12. Limitations and gotchas

Be clear-eyed about the trade-offs that make easyAV1 "easy":

- **One container, one codec pair.** WebM + AV1 + Vorbis. If your source is MP4/H.264/AAC,
  transcode it first (one `ffmpeg` command does it).
- **8-bit by default.** 10/12-bit decoding exists but requires building with `-Dbitdepths=All`
  and costs ~1MB of binary. If your content is 8-bit (most game cinematics are), ignore this.
- **Software decoding only.** There's no hardware-accelerated path. On a modern desktop CPU
  this is rarely a problem for 1080p; on embedded-class hardware, size your videos
  accordingly.
- **Frame pointers are temporary.** Re-read [3.3](#33-frames-are-fleeting) if you ever get
  garbage after a frame — you held the pointer too long.
- **The audio buffer API is a known weak spot.** `easyav1_is_audio_buffer_filled` means
  "overflow imminent", not "ready"; the audio path is unsynchronized; and this area of the API
  is expected to change in a future release to be drain-friendly and thread-safe. See
  [7.2](#72-is-there-audio--a-naming-nuance) for the safe patterns until then.
- **Strides.** Re-read [6.2](#62-the-planes) if your video looks smeared or shifted — you
  indexed rows with `width` instead of `stride`.
- **Thread safety of callbacks in Model B.** In background playback, callbacks are
  foreign-thread code. See below.
- **Binary size.** Budget roughly +2MB (3MB with high bit depths) in your final executable.
- **Experimental status.** It works, but it hasn't been hammered by years of production use.
  Don't bolt it onto your most critical path without testing the failure modes you care about.

### 12.1 Thread-safety notes

The practical rules:

1. **In Model B, callbacks run on easyAV1's internal thread.** Never assume they run on your
   main thread. (In Model A they run synchronously on *your* calling thread, right after the
   `easyav1_decode_*` call — so rules 2–4 below matter least there, but good habits are good
   habits.)
2. **Don't call easyAV1 functions from inside a callback** — especially not
   `easyav1_play`/`easyav1_stop`/seek. If you need to react to a frame (e.g. "video ended,
   start the next one"), set a flag or push an event and handle it on your main thread.
3. **Keep callbacks short.** They're on the decode critical path; a slow callback stalls the
   pipeline. (Uploading to a GPU texture is fine — the GPU work is async. Doing a disk read is
   not.)
4. **Shared state needs protection.** If the callback and your render thread both touch the
   "current frame" object, use a lock or a double-buffer (write new frame to buffer B, swap
   pointers on present).
5. **SDL from the decode thread is fine** *for thread-safe entry points* like
   `SDL_PutAudioStreamData`; rendering calls are not — do rendering on your render thread.
6. **The audio path is not synchronized (known issue).** `easyav1_get_audio_frame` and the
   decoder's writes to the sample ring share no lock, so in Model B, polling audio from your app
   thread is a data race. Use the audio callback (same thread as the decoder) — or copy the
   samples the instant you get them. See [7.2](#72-is-there-audio--a-naming-nuance).

---

## 13. Troubleshooting

**"Failed to initialize easyav1." with no other info.**
Turn logging up: `settings.log_level = EASYAV1_LOG_LEVEL_INFO` and re-run. The library logs the
reason it rejected the file (bad header, unsupported track, I/O failure, …).

**The video looks wrong (shifted, smeared, or with a grayish cast).**
In order of likelihood: (1) you indexed planes with `width` instead of `stride`;
(2) your texture format doesn't match the frame's `pixel_layout` (e.g. feeding YUV420 data into
an NV12 texture); (3) you assumed full-range but the clip is limited-range (or vice versa) —
check `frame->properties.color_space` and set your texture's color range accordingly.

**Audio is out of sync with video.**
Nudge `settings.audio_offset_time` — positive pushes audio *later*, negative *earlier* — and
apply with `easyav1_update_settings`. Audio-device buffering is the usual culprit; a few ms of
offset is normal to add.

**Audio clicks, gaps, or dropouts (especially in Model B).**
You're probably consuming audio with `easyav1_is_audio_buffer_filled` — that means "wait until
the ring is full", and the decoder discards the oldest samples when it overflows. Drain
instead: loop `easyav1_get_audio_frame` and consume immediately — or, in Model B, move audio to
the audio callback (see [7.2](#72-is-there-audio--a-naming-nuance)).

**The video stutters but the audio is fine (or vice versa).**
If you're in Model A, make sure you're actually pacing (`easyav1_decode_for` with real elapsed
time) and not accidentally decoding faster than real time. If you're in Model B, check that your
delivery path isn't doing heavy work — slow callbacks (see [12.1](#121-thread-safety-notes)) or a
busy polling loop (if you're polling instead of using callbacks).

**Seeking is slow.**
Expected behavior with the default exact seeking on long clips without nearby keyframes — try
`use_fast_seeking = EASYAV1_TRUE`, or encode with more frequent keyframes
(`-g` / keyint options in your AV1 encoder).

**Crash on the second frame / garbage pixels.**
Almost always "held the frame pointer too long" (see [3.3](#33-frames-are-fleeting)). Consume
the frame immediately after getting it.

**Linker errors about `dav1d_*` symbols.**
Static-library link order issue: `libeasyav1.a` must come before the `libdav1d*.a` archives
(your linker resolves left-to-right).

---

## 14. License

easyAV1 is distributed under the BSD 3-Clause License — see [LICENSE](LICENSE) at the root of
the repository. It's a permissive license: you may use, modify, and redistribute the library
(including in commercial games and applications) as long as you keep the copyright notice and
disclaimer.

This manual is part of the repository and is covered by the same license.

The bundled third-party libraries are each under their own permissive, BSD-style licenses —
[nestegg](https://github.com/mozilla/nestegg) (ISC),
[dav1d](https://code.videolan.org/videolan/dav1d) (BSD 2-clause), and
[minivorbis](https://github.com/edubart/minivorbis) (BSD 3-clause). The
actual texts are under `ext/` in the repository.

**Combining easyAV1 with other code.** Since every component is permissively licensed, there
is no copyleft anywhere in the dependency tree, which makes easyAV1 combinable with code
under any license:

- **Commercial/proprietary software:** yes. Use, modify, and redistribute freely — including
  selling the result — as long as you keep the copyright notices and license texts of
  easyAV1 and the bundled libraries in your distribution (an "about"/credits screen or a
  license folder is the usual spot). Note the BSD 3-clause no-endorsement clause: don't use
  the copyright holders' names to promote your product without their permission.
- **GPL/AGPL projects:** yes. easyAV1 can be linked into a GPL or AGPL program; the combined
  work is then distributed under that copyleft license, and the permissive libraries'
  copyright notices simply carry along.

---

*Found something in this manual that's wrong or missing? The best way to improve it is to edit
it and send a pull request — this file is part of the repo on purpose.*
