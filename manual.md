# easyav1 — Programmer’s Guide

> A lightweight C library that makes decoding AV1-in-WebM straightforward.  
> This guide covers building, configuration, the public API, threading/seek behavior, platform support, and practical usage patterns.

---

# Table of Contents

- [easyav1 — Programmer’s Guide](#easyav1--programmers-guide)
- [Table of Contents](#table-of-contents)
- [**1. Overview**](#1-overview)
  - [1.1 Introduction](#11-introduction)
  - [1.2 Design Goals](#12-design-goals)
  - [1.3 License](#13-license)
  - [1.4 Example Usage](#14-example-usage)
- [**2. Building the Library**](#2-building-the-library)
  - [2.1 Requirements](#21-requirements)
  - [2.2 Fetching Submodules](#22-fetching-submodules)
  - [2.3 Build Options](#23-build-options)
  - [2.4 Building on Linux / macOS](#24-building-on-linux--macos)
  - [2.5 Building on Windows (MSVC, x86/x64)](#25-building-on-windows-msvc-x86x64)
  - [2.6 Building on Windows (MSVC, ARM64)](#26-building-on-windows-msvc-arm64)
  - [**2.7 Linking Against easyav1**](#27-linking-against-easyav1)
    - [**Option A – Embedded Build (Recommended)**](#option-a--embedded-build-recommended)
    - [**Option B – Manual Linking**](#option-b--manual-linking)
- [**3. Main data Types and Enums**](#3-main-data-types-and-enums)
  - [3.1 `easyav1_t`](#31-easyav1_t)
  - [3.2 `easyav1_bool`](#32-easyav1_bool)
  - [3.3 `easyav1_status`](#33-easyav1_status)
  - [3.4 `easyav1_timestamp`](#34-easyav1_timestamp)
  - [3.5 `easyav1_video_frame`](#35-easyav1_video_frame)
  - [3.6 `easyav1_audio_frame`](#36-easyav1_audio_frame)
- [**4. Initialization and Cleanup**](#4-initialization-and-cleanup)
  - [4.1 `easyav1_init_from_filename`](#41-easyav1_init_from_filename)
    - [Description](#description)
    - [Parameters](#parameters)
    - [Return Value](#return-value)
  - [4.2 `easyav1_init_from_file`](#42-easyav1_init_from_file)
    - [Description](#description-1)
    - [Parameters](#parameters-1)
    - [Return Value](#return-value-1)
  - [4.3 `easyav1_init_from_memory`](#43-easyav1_init_from_memory)
    - [Description](#description-2)
    - [Parameters](#parameters-2)
    - [Return Value](#return-value-2)
    - [Note](#note)
  - [4.4 `easyav1_init_from_custom_stream`](#44-easyav1_init_from_custom_stream)
    - [Description](#description-3)
    - [Parameters](#parameters-3)
    - [Return Value](#return-value-3)
  - [4.5 `easyav1_destroy`](#45-easyav1_destroy)
    - [Description](#description-4)
    - [Parameters](#parameters-4)
- [5. Decoding Modes](#5-decoding-modes)
- [**5.1 Manual Decoding Loop**](#51-manual-decoding-loop)
  - [5.1.1 `easyav1_decode_next`](#511-easyav1_decode_next)
    - [Description](#description-5)
    - [Parameters](#parameters-5)
    - [Return Value](#return-value-4)
  - [5.1.2 `easyav1_is_finished`](#512-easyav1_is_finished)
    - [Description](#description-6)
    - [Parameters](#parameters-6)
    - [Return Value](#return-value-5)
  - [5.1.3 Usage Pattern](#513-usage-pattern)
  - [5.1.4 Example: Manual Decode Loop](#514-example-manual-decode-loop)
  - [5.1.5 When to Use Manual Decoding](#515-when-to-use-manual-decoding)
  - [5.2. Background Playback Thread (`easyav1_play`)](#52-background-playback-thread-easyav1_play)
  - [Function Reference](#function-reference)
    - [`easyav1_play`](#easyav1_play)
      - [**Description**](#description-7)
      - [**Parameters**](#parameters-7)
      - [**Return Value**](#return-value-6)
      - [**Usage Notes**](#usage-notes)
      - [**Thread Safety**](#thread-safety)
  - [Example: Real-Time Playback Loop](#example-real-time-playback-loop)
- [**6. Video Frame Access**](#6-video-frame-access)
  - [6.1 `easyav1_has_video_frame`](#61-easyav1_has_video_frame)
    - [Description](#description-8)
    - [Parameters](#parameters-8)
    - [Return Value](#return-value-7)
  - [6.2 `easyav1_get_video_frame`](#62-easyav1_get_video_frame)
    - [Description](#description-9)
    - [Parameters](#parameters-9)
    - [Return Value](#return-value-8)
  - [6.3 `easyav1_video_frame` Structure](#63-easyav1_video_frame-structure)
    - [Field Details](#field-details)
    - [Notes](#notes)
  - [6.4 Example: Saving YUV Frames](#64-example-saving-yuv-frames)
  - [6.5 Integration with SDL](#65-integration-with-sdl)
- [**7. Audio Frame Access**](#7-audio-frame-access)
  - [7.1 `easyav1_has_audio_frame`](#71-easyav1_has_audio_frame)
    - [Description](#description-10)
    - [Parameters](#parameters-10)
    - [Return Value](#return-value-9)
  - [7.2 `easyav1_get_audio_frame`](#72-easyav1_get_audio_frame)
    - [Description](#description-11)
    - [Parameters](#parameters-11)
    - [Return Value](#return-value-10)
  - [7.3 `easyav1_audio_frame` Structure](#73-easyav1_audio_frame-structure)
    - [Field Details](#field-details-1)
    - [Notes](#notes-1)
  - [7.4 Example: Writing PCM to File](#74-example-writing-pcm-to-file)
  - [7.5 Integration with SDL](#75-integration-with-sdl)
- [**8. Important structures**](#8-important-structures)
  - [**8.1 Decoder Settings (`easyav1_settings`)**](#81-decoder-settings-easyav1_settings)
    - [Overview](#overview)
    - [Structure Definition](#structure-definition)
    - [Field Reference](#field-reference)
      - [`enable_video`](#enable_video)
      - [`enable_audio`](#enable_audio)
      - [`skip_unprocessed_frames`](#skip_unprocessed_frames)
      - [`interlace_audio`](#interlace_audio)
      - [`close_handle_on_destroy`](#close_handle_on_destroy)
      - [`callbacks`](#callbacks)
      - [`video_track`](#video_track)
      - [`audio_track`](#audio_track)
      - [`use_fast_seeking`](#use_fast_seeking)
      - [`audio_offset_time`](#audio_offset_time)
      - [`log_level`](#log_level)
    - [Related Functions](#related-functions)
      - [`easyav1_default_settings`](#easyav1_default_settings)
      - [`easyav1_get_current_settings`](#easyav1_get_current_settings)
      - [`easyav1_update_settings`](#easyav1_update_settings)
    - [Usage Example](#usage-example)
  - [**8.2 Custom streams (`easyav1_stream`)**](#82-custom-streams-easyav1_stream)
    - [Overview](#overview-1)
    - [Structure Definition](#structure-definition-1)
    - [Field Reference](#field-reference-1)
      - [`easyav1_read_func`](#easyav1_read_func)
- [**8. Stream Information**](#8-stream-information)
  - [8.1 `easyav1_get_video_width` / `easyav1_get_video_height`](#81-easyav1_get_video_width--easyav1_get_video_height)
    - [Description](#description-12)
  - [8.2 `easyav1_get_framerate`](#82-easyav1_get_framerate)
    - [Description](#description-13)
  - [8.3 `easyav1_get_duration`](#83-easyav1_get_duration)
    - [Description](#description-14)
  - [8.4 `easyav1_get_audio_channels` / `easyav1_get_audio_sample_rate`](#84-easyav1_get_audio_channels--easyav1_get_audio_sample_rate)
    - [Description](#description-15)
  - [8.5 Example: Printing Stream Metadata](#85-example-printing-stream-metadata)
- [**9. Error Handling**](#9-error-handling)
  - [9.1 `easyav1_status`](#91-easyav1_status)
  - [9.2 Common Return Patterns](#92-common-return-patterns)
  - [9.3 Error Propagation](#93-error-propagation)
  - [9.4 Example: Safe Decode Loop](#94-example-safe-decode-loop)
  - [9.5 Logging Levels](#95-logging-levels)
  - [9.6 Best Practices](#96-best-practices)
- [**10. Thread Safety and Concurrency**](#10-thread-safety-and-concurrency)
  - [10.1 Overview](#101-overview)
  - [10.2 Thread Ownership](#102-thread-ownership)
  - [10.3 Safe Function Calls](#103-safe-function-calls)
  - [10.4 Callbacks](#104-callbacks)
    - [Callback Guidelines](#callback-guidelines)
  - [10.5 Typical Concurrency Model](#105-typical-concurrency-model)
  - [10.6 Example: Polling vs Callbacks](#106-example-polling-vs-callbacks)
  - [10.7 Best Practices](#107-best-practices)
  - [10.8 Implementation detail](#108-implementation-detail)
- [**11. Utility Functions**](#11-utility-functions)
  - [11.1 Version Information](#111-version-information)
    - [Description](#description-16)
    - [Example](#example)
  - [11.2 Seeking](#112-seeking)
    - [Description](#description-17)
    - [Return Value](#return-value-11)
  - [11.3 Get Current Timestamp](#113-get-current-timestamp)
    - [Description](#description-18)
  - [11.4 End-of-Stream Detection](#114-end-of-stream-detection)
    - [Description](#description-19)
  - [11.5 Example: Basic Seek](#115-example-basic-seek)
- [13. Tutorials](#13-tutorials)
- [**Tutorial 1: Minimal SDL3 Player**](#tutorial-1-minimal-sdl3-player)
  - [T1.1 Requirements](#t11-requirements)
  - [T1.2 Code Listing](#t12-code-listing)
  - [T1.3 How It Works](#t13-how-it-works)
  - [T1.4 Notes](#t14-notes)
- [**Tutorial 2: Using Callbacks**](#tutorial-2-using-callbacks)
  - [T2.1 Defining Callbacks](#t21-defining-callbacks)
  - [T2.2 Setting Up](#t22-setting-up)
  - [T2.3 Adding Audio Callback](#t23-adding-audio-callback)
  - [T2.4 Key Notes](#t24-key-notes)
- [**Tutorial 3: Seeking in a Video**](#tutorial-3-seeking-in-a-video)
  - [T3.1 Basics of Seeking](#t31-basics-of-seeking)
  - [T3.2 Example: Jump to 10 Seconds](#t32-example-jump-to-10-seconds)
  - [T3.3 Tips](#t33-tips)
- [**Tutorial 4: Adjusting Audio Synchronization**](#tutorial-4-adjusting-audio-synchronization)
  - [T4.1 `audio_offset_time`](#t41-audio_offset_time)
  - [T4.2 Example: Advancing Audio by 50ms](#t42-example-advancing-audio-by-50ms)
  - [T4.3 SDL Audio Case](#t43-sdl-audio-case)
  - [T4.4 Notes](#t44-notes)
- [**Tutorial 5: Error Handling and Logging**](#tutorial-5-error-handling-and-logging)
  - [T5.1 Checking Return Codes](#t51-checking-return-codes)
    - [Example: Safe Decode Loop](#example-safe-decode-loop)
  - [T5.2 Using Logging](#t52-using-logging)
    - [Levels](#levels)
  - [T5.3 Example: Debugging with Verbose Logging](#t53-example-debugging-with-verbose-logging)
  - [T5.4 Best Practices](#t54-best-practices)
- [**Appendix A: Platform Support**](#appendix-a-platform-support)
  - [A.1 Supported Platforms](#a1-supported-platforms)
  - [A.2 Unsupported Platforms](#a2-unsupported-platforms)
  - [A.3 Platform-Specific Notes](#a3-platform-specific-notes)
- [Appendix: Function Reference (Quick List)](#appendix-function-reference-quick-list)
14. [Appendix A: Platform Support](#appendix-a-platform-support)  
15. [Appendix: Function Reference (Quick List)](#appendix-function-reference-quick-list)  


---

# **1. Overview**

## 1.1 Introduction

**easyav1** is a lightweight C library providing a **simple, high-level interface for decoding AV1 video and Vorbis audio from WebM files**.
It is designed for developers who want to embed modern video/audio decoding capabilities in their applications without dealing directly with complex codec APIs.

The library wraps and integrates three core technologies:

* **[dav1d](https://code.videolan.org/videolan/dav1d)** – fast, production-ready AV1 decoder.
* **[nestegg](https://github.com/kinetiknz/nestegg)** – WebM (Matroska) container parser.
* **[minivorbis](https://github.com/lieff/minivorbis)** – lightweight Vorbis audio decoder.

Internally, `easyav1` manages demuxing, decoding, synchronization, and buffering.
Externally, it exposes a **minimal C API** that enables applications to:

* Open a WebM file.
* Query stream information (video resolution, audio sample rate, etc.).
* Decode frames either step-by-step or via background playback.
* Access decoded **YUV video frames** and **PCM float audio samples**.

---

## 1.2 Design Goals

* **Simplicity** – Easy to use from plain C without external frameworks.
* **Portability** – Works across Linux, Windows, and macOS.
* **Efficiency** – Uses `dav1d` for high-performance AV1 decoding.
* **Flexibility** – Supports both **manual decoding** (for processing pipelines) and **background threaded playback** (for real-time applications).

---

## 1.3 License

`easyav1` is licensed under the **BSD 3-Clause License**, permitting commercial and non-commercial use with minimal restrictions.
See the bundled `LICENSE` file for details.

---

## 1.4 Example Usage

A minimal program that opens a WebM file and iterates over decoded frames:

```c
#include <stdio.h>
#include <easyav1.h>

int main(void)
{
    easyav1_t *easyav1 = easyav1_init_from_filename("video.webm", NULL);
    if (!easyav1) {
        fprintf(stderr, "Failed to initialize easyav1.\n");
        return 1;
    }

    while (easyav1_decode_next(easyav1) == EASYAV1_STATUS_OK) {
        if (easyav1_has_video_frame(easyav1) == EASYAV1_TRUE) {
            const easyav1_video_frame *vf = easyav1_get_video_frame(easyav1);
            printf("Decoded video frame: %ux%u (PTS=%llu)\n",
                   vf->width, vf->height, vf->timestamp);
        }

        const easyav1_audio_frame *af = easyav1_get_audio_frame(easyav1);
        if (af) {
            printf("Decoded audio frame: %u samples (PTS=%llu)\n", af->samples, af->timestamp);
        }
    }

    easyav1_destroy(&easyav1);
    return 0;
}
```

---

# **2. Building the Library**

## 2.1 Requirements

* **CMake ≥ 3.8**
* **C99-compatible compiler** (e.g. GCC, Clang, MSVC)
* **Dependencies**:

  * [`dav1d`](https://code.videolan.org/videolan/dav1d) (AV1 decoder)

    * By default, `easyav1` uses its **bundled copy** of dav1d.
    * If you want to use your **system-installed dav1d**, set
      `-DEASYAV1_USE_EXTERNAL_DAV1D_LIBRARY=ON`.
  * [`nestegg`](https://github.com/kinetiknz/nestegg) (WebM container parser, bundled in `ext/`)
  * [`minivorbis`](https://github.com/lieff/minivorbis) (Vorbis decoder, bundled in `ext/`)
  * POSIX Threads (`pthread`) on non-Windows systems
  * For building on Arm64 versions of Windows with the bundled copy of `dav1d`, you'll also need [`Perl`](https://strawberryperl.com).

---

## 2.2 Fetching Submodules

If you are using the **bundled `dav1d`**, make sure to initialize the submodule:

```bash
git clone https://github.com/crudelios/easyav1.git
cd easyav1
git submodule update --init --recursive
```

Failing to do this will result in build errors because `ext/dav1d/` will not be populated.

---

## 2.3 Build Options

Set via `-D<option>=ON/OFF` when running CMake:

* **`EASYAV1_USE_EXTERNAL_DAV1D_LIBRARY`**
  Default: `OFF`

  * `OFF`: builds the bundled `dav1d` from `ext/dav1d`.
  * `ON`: requires system-wide `dav1d` (`find_package(dav1d REQUIRED)`).

* **`EASYAV1_BUILD_TOOLS`**
  Default: `ON`

  * Builds example tools from the `tools/` directory.

* **`EASYAV1_USE_SANITIZERS`**
  Default: `ON` (Debug builds only)

  * Enables ThreadSanitizer and UBSan for debugging.

---

## 2.4 Building on Linux / macOS

```bash
git clone https://github.com/crudelios/easyav1.git
cd easyav1
git submodule update --init --recursive
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

This produces:

* **Library:** `libeasyav1.a` (static) or `libeasyav1.so` (shared, if enabled).
* **Headers:** in `src/`.
* **Optional tools:** built in `tools/`.

---

## 2.5 Building on Windows (MSVC, x86/x64)

```powershell
git clone https://github.com/yourname/easyav1.git
cd easyav1
git submodule update --init --recursive
mkdir build && cd build
cmake .. -G "Visual Studio 16 2019" -A x64
cmake --build . --config Release
```

## 2.6 Building on Windows (MSVC, ARM64)

First load the ARM64 developer prompt. Then run:

```powershell
git clone https://github.com/yourname/easyav1.git
cd easyav1
git submodule update --init --recursive
mkdir build && cd build
cmake .. -G "Ninja"
cmake --build . --config Release
```

This ensures `dav1d` is built properly.

---

## **2.7 Linking Against easyav1**

Since `easyav1` does not currently provide a `find_package` configuration, you have two main options to integrate it into your project:

---

### **Option A – Embedded Build (Recommended)**

If your project uses CMake, you can add `easyav1` directly as a subdirectory:

```cmake
add_subdirectory(path/to/easyav1)

add_executable(myapp main.c)
target_link_libraries(myapp PRIVATE easyav1)
```

This ensures your project is always built against the correct version of `easyav1`.

---

### **Option B – Manual Linking**

If you built `easyav1` separately, you can manually specify the include and library paths:

```cmake
include_directories(/path/to/easyav1/src)
link_directories(/path/to/easyav1/build)

add_executable(myapp main.c)
target_link_libraries(myapp PRIVATE easyav1 dav1d Threads::Threads)
```

On Linux/macOS you may also need `m` (math library) depending on your system:

```cmake
target_link_libraries(myapp PRIVATE easyav1 dav1d Threads::Threads m)
```


---

# **3. Main data Types and Enums**

This chapter documents the fundamental data structures and enumerations used throughout the `easyav1` API.

---

## 3.1 `easyav1_t`

```c
typedef struct easyav1_t easyav1_t;
```

**Description**
Opaque handle representing a decoding context.
All API calls operate on a pointer to this structure, which is created by `easyav1_init_from_filename()` or `easyav1_init_from_file()` and destroyed with `easyav1_destroy()`.

Applications must not access its internal fields directly.

---

## 3.2 `easyav1_bool`

```c
typedef enum {
    EASYAV1_FALSE = 0,
    EASYAV1_TRUE  = 1
} easyav1_bool;
```

**Description**
Custom boolean type used throughout the API.
Functions returning `easyav1_bool` use `EASYAV1_TRUE` for success/presence and `EASYAV1_FALSE` otherwise.

---

## 3.3 `easyav1_status`

```c
typedef enum {
    EASYAV1_STATUS_OK = 0,              // Operation successful
    EASYAV1_STATUS_FINISHED,            // End of stream reached
    EASYAV1_STATUS_ERROR,               // Generic error
    EASYAV1_STATUS_INVALID_ARGUMENT,    // Invalid function argument
    EASYAV1_STATUS_INVALID_STATE,       // Operation not allowed in current state
    EASYAV1_STATUS_IO_ERROR,            // File or I/O error
    EASYAV1_STATUS_OUT_OF_MEMORY,       // Memory allocation failed
    EASYAV1_STATUS_DECODER_ERROR        // dav1d or Vorbis decoding error
} easyav1_status;
```

**Description**
Indicates the result of an operation.
Most API functions return `easyav1_status`.

**Helper Macro**

```c
#define EASYAV1_STATUS_IS_ERROR(status) ((status) <= EASYAV1_STATUS_ERROR ? EASYAV1_TRUE : EASYAV1_FALSE)
```

Evaluates to `EASYAV1_TRUE` if the given status indicates an error condition.

---

## 3.4 `easyav1_timestamp`

```c
typedef uint64_t easyav1_timestamp;
```

**Description**
Represents a presentation timestamp (PTS) in microseconds.
All decoded video and audio frames carry a timestamp.

---

## 3.5 `easyav1_video_frame`

```c
typedef struct {
    uint8_t *planes[3];                        // Y, U, V planes
    int stride[3];                             // Bytes per row for each plane
    easyav1_video_frame_properties properties; // Properties of the frame, such as width, height and color format
    easyav1_timestamp timestamp;               // Presentation timestamp (ms)
} easyav1_video_frame;
```

**Description**
Represents a single decoded video frame in **YUV planar format**.

* `planes[0]` → Y (luma plane)
* `planes[1]` → U (chroma)
* `planes[2]` → V (chroma)

**Usage Notes**

* The data is owned by the decoder. Do **not** free or modify it.
* If you need to keep a frame after the next decode step, copy the data.
* For more detail about video frame properties, check TODO

---

## 3.6 `easyav1_audio_frame`

```c
typedef struct {
    unsigned int samples;        // Number of samples per channel
    unsigned int channels;       // Number of audio channels
    easyav1_timestamp timestamp; // Presentation timestamp (ms)

    size_t bytes;                // Number of bytes in this frame

    union {
        const float **deinterlaced; // Deinterlaced audio samples.
        const float *interlaced;    // Interlaced audio samples.
    } pcm; // The PCM samples. This is either deinterlaced or interlaced depending on the `interlace_audio` setting.
} easyav1_audio_frame;
```

**Description**
Represents a decoded audio frame with **32-bit float PCM samples**.

**Usage Notes**

* Each call to `easyav1_get_audio_frame()` returns a frame corresponding to a chunk of decoded audio.
* `samples` indicates the number of samples **per channel**.
  
* How you use the `pcm` union depends on how you set the `intelace_audio` setting.
 
  - If interlacing is **enabled**, you should obtain the audio data by directly using `audio_frame->pcm.interlaced`.
  The size of this array is obtained from the `audio_frame->bytes`, and in this case it's equal to `samples * sizeof(float) * channels`.

  - If interlacing is **disabled**, you need to obtain each channel's audio data by using `audio_frame.pcm.deinterlaced[channel]`. The size of each of the channel arrays is obtained using `audio_frame->bytes`, which is equal to `samples * sizeof(float)`.

  In either case, make sure to respect the `intelace_audio` setting, because the decoder only properly fills the corresponding array.
  
* The data is owned by the decoder. Copy it if needed.

---

# **4. Initialization and Cleanup**

This chapter covers the functions used to create and destroy a decoder context. Every program using `easyav1` must start by initializing a decoding context, and end by destroying it to free resources.

---

## 4.1 `easyav1_init_from_filename`

```c
easyav1_t *easyav1_init_from_filename(const char *filename, const easyav1_settings *settings);
```

### Description

Creates a new decoder context from a WebM file on disk.

* Opens the file.
* Initializes container parsing with **nestegg**.
* Prepares decoders (**dav1d** for video, **minivorbis** for audio).

### Parameters

* `filename` →
  Path to the WebM file to decode.
* `settings` *(optional)* →
  If `NULL`, default settings are used.

### Return Value

* Pointer to a new `easyav1_t` context if successful.
* `NULL` if initialization failed.

---

## 4.2 `easyav1_init_from_file`

```c
easyav1_t *easyav1_init_from_file(FILE *file, const easyav1_settings *settings);
```

### Description

Creates a decoder context from an already opened `FILE*`.
Useful when integrating with custom file I/O.

### Parameters

* `file` →
  Pointer to a standard C `FILE` handle, already opened for reading.
* `settings` *(optional)* →
  If `NULL`, default settings are used.

### Return Value

* New decoder context if successful.
* `NULL` on failure.

---

## 4.3 `easyav1_init_from_memory`

```c
easyav1_t *easyav1_init_from_memory(const void *data, size_t size, const easyav1_settings *settings);
```

### Description

Creates a decoder context from a memory buffer.
Useful when integrating with custom file I/O (e.g. streaming from memory or over a network).

### Parameters

* `data` →
  The buffer to read from.
* `size` →
  The size of the buffer.
* `settings` *(optional)* →
  If `NULL`, default settings are used.

### Return Value

* New decoder context if successful.
* `NULL` on failure.

### Note

You must provide the full buffer. If you need to use partial buffers, use `easyav1_init_from_custom_stream`.

---

## 4.4 `easyav1_init_from_custom_stream`

```c
easyav1_t *easyav1_init_from_custom_stream(const easyav1_stream *stream, const easyav1_settings *settings);
```

### Description

Creates a decoder context from a custom stream.
Useful when integrating with specific memory streams that are created immediately, such as network data.

### Parameters

* `stream` →
  Pointer to an `easyav1_stream` structure, that will handle the fetching of the actual data.

  To handle the structure, check TODO

* `settings` *(optional)* →
  If `NULL`, default settings are used.

### Return Value

* New decoder context if successful.
* `NULL` on failure.

---

## 4.5 `easyav1_destroy`

```c
void easyav1_destroy(easyav1_t **ctx);
```

### Description

Releases all resources associated with a decoder context.

* Stops background threads if `easyav1_play()` was used.
* Frees video and audio buffers.
* Closes associated container and decoder objects.

After destruction, `*ctx` is set to `NULL`.

### Parameters

* `ctx`
  Address of a pointer to a decoder context. Must not be `NULL`.

---

# 5. Decoding Modes

# **5.1 Manual Decoding Loop**

The **manual decoding loop** is the traditional way of using `easyav1`.
In this mode, the application explicitly drives decoding by repeatedly calling `easyav1_decode_next()`. After each call, the program can query whether new video or audio frames are available.

This approach is best suited for **offline processing**, **transcoding**, or any use case requiring **frame-accurate iteration** over a WebM file.

---

## 5.1.1 `easyav1_decode_next`

```c
EASYAV1_API easyav1_status
easyav1_decode_next(easyav1_t *ctx);
```

### Description

Decodes the next packet from the WebM file.
Depending on the packet type, this may produce:

* A new video frame (accessible via `easyav1_has_video_frame` / `easyav1_get_video_frame`),
* A new audio frame (accessible via `easyav1_has_audio_frame` / `easyav1_get_audio_frame`),
* Or both (if interleaved).

### Parameters

* `ctx`
  Decoder context, previously initialized.

### Return Value

* `EASYAV1_STATUS_OK` → Decoded successfully, may have produced a new frame.
* `EASYAV1_STATUS_FINISHED` → End of stream reached.
* Error codes (`EASYAV1_STATUS_IO_ERROR`, `EASYAV1_STATUS_DECODER_ERROR`, etc.) on failure.

---

## 5.1.2 `easyav1_is_finished`

```c
EASYAV1_API easyav1_bool
easyav1_is_finished(const easyav1_t *ctx);
```

### Description

Checks if decoding has reached the end of the file.
Equivalent to testing whether the most recent call to `easyav1_decode_next()` returned `EASYAV1_STATUS_FINISHED`.

### Parameters

* `ctx`
  Decoder context.

### Return Value

* `EASYAV1_TRUE` → End of stream.
* `EASYAV1_FALSE` → More packets remain.

---

## 5.1.3 Usage Pattern

1. Initialize a decoder with `easyav1_init_from_filename`.
2. Enter a loop:

   * Call `easyav1_decode_next()`.
   * If status is `OK`, check for frames:

     * `easyav1_has_video_frame()` → `easyav1_get_video_frame()`.
     * `easyav1_has_audio_frame()` → `easyav1_get_audio_frame()`.
   * Stop when status is `FINISHED` or an error occurs.
3. Destroy the context with `easyav1_destroy()`.

---

## 5.1.4 Example: Manual Decode Loop

```c
#include <stdio.h>
#include "easyav1.h"

int main(void) {
    easyav1_t *ctx = easyav1_init_from_filename("movie.webm", NULL);
    if (!ctx) {
        fprintf(stderr, "Failed to initialize decoder.\n");
        return 1;
    }

    easyav1_status st;
    while ((st = easyav1_decode_next(ctx)) == EASYAV1_STATUS_OK) {
        if (easyav1_has_video_frame(ctx) == EASYAV1_TRUE) {
            const easyav1_video_frame *vf = easyav1_get_video_frame(ctx);
            printf("Video frame: %dx%d PTS=%lld\n",
                   vf->width, vf->height, (long long)vf->pts);
        }

        if (easyav1_has_audio_frame(ctx) == EASYAV1_TRUE) {
            const easyav1_audio_frame *af = easyav1_get_audio_frame(ctx);
            printf("Audio frame: %d samples @ %d Hz, PTS=%lld\n",
                   af->samples, af->sample_rate, (long long)af->pts);
        }
    }

    if (st == EASYAV1_STATUS_FINISHED) {
        printf("Decoding complete.\n");
    } else {
        fprintf(stderr, "Decoding error (status=%d)\n", st);
    }

    easyav1_destroy(&ctx);
    return 0;
}
```

---

## 5.1.5 When to Use Manual Decoding

* Batch processing (e.g. video conversion, analysis).
* Applications where **frame-accurate stepping** is required.
* Use cases where background threads are undesirable.

For real-time playback, use the threaded mode (`easyav1_play`) documented in **Chapter 5.2**.

---

## 5.2. Background Playback Thread (`easyav1_play`)

## Function Reference

### `easyav1_play`

```c
EASYAV1_API easyav1_status easyav1_play(easyav1_t *ctx);
```

#### **Description**

Starts decoding in a dedicated background thread.
Unlike the manual decoding loop (`easyav1_decode_next`), this function continuously decodes video and audio frames in the background.

The application can then retrieve the **most recently decoded frame** at its own pace using the `easyav1_has_*_frame` and `easyav1_get_*_frame` functions.

This model is intended for **real-time playback scenarios**, where decoding should keep running independently of the application loop.

---

#### **Parameters**

* `ctx`
  Pointer to a valid `easyav1_t` decoder context, previously initialized with `easyav1_init_from_filename()` or `easyav1_init_from_file()`.

---

#### **Return Value**

* `EASYAV1_STATUS_OK` if the playback thread was successfully started.
* An error status (`EASYAV1_STATUS_ERROR`, etc.) if thread creation failed or the context was invalid.

---

#### **Usage Notes**

* After calling `easyav1_play()`, the decoding loop runs in a background thread until the stream finishes or an error occurs.
* The user does **not** call `easyav1_decode_next()` in this mode.
* Instead, the application periodically polls:

  * `easyav1_has_video_frame()` → `easyav1_get_video_frame()`
  * `easyav1_has_audio_frame()` → `easyav1_get_audio_frame()`
* Frames returned represent the **latest decoded frame**. Intermediate frames may be dropped if the user loop is slower than the decoder thread.
* This mode is designed for **playback-oriented applications** (media players, streaming). For frame-accurate offline processing, prefer the manual decoding loop.

---

#### **Thread Safety**

* `easyav1_play()` spawns an internal decoding thread.
* Frame retrieval functions are safe to call from the main thread while decoding runs.
* The user must not access internal decoder state directly.
* Always call `easyav1_destroy()` to terminate decoding cleanly and release resources.

---

## Example: Real-Time Playback Loop

```c
#include <stdio.h>
#include <easyav1.h>

int main(void) {
    easyav1_t *ctx = easyav1_init_from_filename("movie.webm", NULL);
    if (!ctx) {
        fprintf(stderr, "Failed to open file.\n");
        return 1;
    }

    if (easyav1_play(ctx) != EASYAV1_STATUS_OK) {
        fprintf(stderr, "Failed to start playback.\n");
        easyav1_destroy(&ctx);
        return 1;
    }

    // Main application loop
    while (easyav1_is_finished(ctx) == EASYAV1_FALSE) {
        if (easyav1_has_video_frame(ctx) == EASYAV1_TRUE) {
            const easyav1_video_frame *vf = easyav1_get_video_frame(ctx);
            printf("Video PTS: %lld\n", (long long)vf->pts);
            // Render vf->planes[...] to screen
        }

        if (easyav1_has_audio_frame(ctx) == EASYAV1_TRUE) {
            const easyav1_audio_frame *af = easyav1_get_audio_frame(ctx);
            printf("Audio PTS: %lld\n", (long long)af->pts);
            // Send af->samples to audio device
        }

        // Small delay to simulate render loop
        // e.g., SDL_Delay(10);
    }

    easyav1_destroy(&ctx);
    return 0;
}
```

---

# **6. Video Frame Access**

After decoding, applications can retrieve decoded video frames in **YUV420 planar format**.
Frames are provided as pointers into internal decoder-managed buffers; they remain valid until the next decoding step.

---

## 6.1 `easyav1_has_video_frame`

```c
EASYAV1_API easyav1_bool
easyav1_has_video_frame(const easyav1_t *ctx);
```

### Description

Checks whether a new decoded video frame is available after the last call to `easyav1_decode_next()` (manual mode) or since the last poll (threaded mode).

### Parameters

* `ctx`
  Decoder context.

### Return Value

* `EASYAV1_TRUE` → A video frame is available.
* `EASYAV1_FALSE` → No new video frame.

---

## 6.2 `easyav1_get_video_frame`

```c
EASYAV1_API const easyav1_video_frame *
easyav1_get_video_frame(const easyav1_t *ctx);
```

### Description

Retrieves the most recent decoded video frame.
The returned pointer is valid until the next call to `easyav1_decode_next()` or until the background decoder produces a new frame in threaded mode.

### Parameters

* `ctx`
  Decoder context.

### Return Value

* Pointer to an `easyav1_video_frame`.
* `NULL` if no frame is available.

---

## 6.3 `easyav1_video_frame` Structure

```c
typedef struct {
    uint8_t *planes[3];   // Y, U, V planes
    int stride[3];        // Bytes per row for each plane
    int width;            // Frame width (pixels)
    int height;           // Frame height (pixels)
    easyav1_timestamp pts;// Presentation timestamp (µs)
} easyav1_video_frame;
```

### Field Details

* **planes\[0]** – Y (luma plane)
* **planes\[1]** – U (chroma plane, subsampled)
* **planes\[2]** – V (chroma plane, subsampled)
* **stride\[n]** – Number of bytes per row for each plane.
* **width/height** – Frame dimensions in pixels.
* **pts** – Presentation timestamp in microseconds.

### Notes

* Format is always **YUV420 planar (I420 / IYUV)**.
* The decoder owns the memory. Do not free or modify it.
* Copy frame data if it must persist beyond the next decode step.

---

## 6.4 Example: Saving YUV Frames

```c
if (easyav1_has_video_frame(ctx) == EASYAV1_TRUE) {
    const easyav1_video_frame *vf = easyav1_get_video_frame(ctx);

    FILE *out = fopen("frame.yuv", "wb");
    if (out) {
        // Write Y plane
        for (int y = 0; y < vf->height; y++)
            fwrite(vf->planes[0] + y * vf->stride[0], 1, vf->width, out);

        // Write U and V planes (subsampled by 2)
        for (int y = 0; y < vf->height / 2; y++)
            fwrite(vf->planes[1] + y * vf->stride[1], 1, vf->width / 2, out);

        for (int y = 0; y < vf->height / 2; y++)
            fwrite(vf->planes[2] + y * vf->stride[2], 1, vf->width / 2, out);

        fclose(out);
    }
}
```

---

## 6.5 Integration with SDL

When rendering with SDL, use `SDL_UpdateYUVTexture()` with the frame’s planes and strides.
See **Tutorial 6** for a complete example.

---

# **7. Audio Frame Access**

Alongside video frames, `easyav1` provides access to decoded audio frames in **interleaved 32-bit float PCM format**.
Applications can retrieve these frames for playback, processing, or storage.

---

## 7.1 `easyav1_has_audio_frame`

```c
EASYAV1_API easyav1_bool
easyav1_has_audio_frame(const easyav1_t *ctx);
```

### Description

Checks whether a new decoded audio frame is available after the last call to `easyav1_decode_next()` (manual mode) or since the last poll (threaded mode).

### Parameters

* `ctx`
  Decoder context.

### Return Value

* `EASYAV1_TRUE` → An audio frame is available.
* `EASYAV1_FALSE` → No new audio frame.

---

## 7.2 `easyav1_get_audio_frame`

```c
EASYAV1_API const easyav1_audio_frame *
easyav1_get_audio_frame(const easyav1_t *ctx);
```

### Description

Retrieves the most recent decoded audio frame.
The returned pointer is valid until the next call to `easyav1_decode_next()` (manual) or until the decoder produces a new frame (threaded).

### Parameters

* `ctx`
  Decoder context.

### Return Value

* Pointer to an `easyav1_audio_frame`.
* `NULL` if no frame is available.

---

## 7.3 `easyav1_audio_frame` Structure

```c
typedef struct {
    float *samples_data;   // Interleaved PCM float samples
    int samples;           // Number of samples per channel
    int channels;          // Number of audio channels
    int sample_rate;       // Sample rate (Hz)
    easyav1_timestamp pts; // Presentation timestamp (µs)
} easyav1_audio_frame;
```

### Field Details

* **samples\_data** – Pointer to interleaved 32-bit float PCM samples.
* **samples** – Number of samples **per channel**.
* **channels** – Number of channels (e.g. 1 = mono, 2 = stereo).
* **sample\_rate** – Audio sampling rate (e.g. 48000 Hz).
* **pts** – Presentation timestamp in microseconds.

### Notes

* Total number of floats in `samples_data` = `samples × channels`.
* Values are normalized floats in range **\[-1.0, +1.0]**.
* Memory is owned by the decoder. Copy if you need long-term storage.

---

## 7.4 Example: Writing PCM to File

```c
if (easyav1_has_audio_frame(ctx) == EASYAV1_TRUE) {
    const easyav1_audio_frame *af = easyav1_get_audio_frame(ctx);

    FILE *out = fopen("frame.pcm", "wb");
    if (out) {
        size_t count = af->samples * af->channels;
        fwrite(af->samples_data, sizeof(float), count, out);
        fclose(out);
    }
}
```

The resulting file contains raw float PCM data, which can be played or converted using tools like **ffmpeg**:

```bash
ffmpeg -f f32le -ar 48000 -ac 2 -i frame.pcm output.wav
```

---

## 7.5 Integration with SDL

When using SDL for playback, simply queue the data:

```c
SDL_QueueAudio(audio_dev,
               af->samples_data,
               af->samples * af->channels * sizeof(float));
```

See **Tutorial 6** for a full SDL integration example.

---

# **8. Important structures**

## **8.1 Decoder Settings (`easyav1_settings`)**

### Overview

The behavior of `easyav1` can be customized using the `easyav1_settings` structure.
Applications typically obtain a copy of the default settings using `easyav1_default_settings()`, modify the fields they care about, and pass the structure to one of the initialization functions (e.g. `easyav1_init_from_filename`).

If `NULL` is passed instead, default values are used automatically.

---

### Structure Definition

```c
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
    int64_t audio_offset_time;        // milliseconds
    easyav1_log_level_t log_level;
} easyav1_settings;
```

---

### Field Reference

#### `enable_video`

Enables or disables video decoding.

* `EASYAV1_TRUE` → Decode video track (if present).
* `EASYAV1_FALSE` → Skip video decoding.

#### `enable_audio`

Enables or disables audio decoding.

* `EASYAV1_TRUE` → Decode audio track (if present).
* `EASYAV1_FALSE` → Skip audio decoding.

#### `skip_unprocessed_frames`

Controls whether unprocessed frames should be skipped.

* Useful when decoding cannot keep up with real-time and old frames can be discarded.

#### `interlace_audio`

Controls the audio sample layout.

* `EASYAV1_TRUE` → **Interleaved** samples (use `frame->samples_data`).
* `EASYAV1_FALSE` → **Deinterleaved** samples (per-channel access).

#### `close_handle_on_destroy`

Whether `easyav1_destroy()` should also close the underlying I/O handle.

* Applies to `FILE*` (from `init_from_file`) and memory buffers (from `init_from_memory`).

#### `callbacks`

Optional user callbacks:

* **video** → `void (*)(const easyav1_video_frame *frame, void *userdata)`
* **audio** → `void (*)(const easyav1_audio_frame *frame, void *userdata)`
* **userdata** → User pointer passed to both callbacks.
  If `NULL`, no callback is used.

#### `video_track`

Selects which video track to decode (0-indexed among video streams only).

#### `audio_track`

Selects which audio track to decode (0-indexed among audio streams only).

#### `use_fast_seeking`

* `EASYAV1_TRUE` → Seek to nearest keyframe before the requested timestamp (fast, approximate).
* `EASYAV1_FALSE` → Seek precisely to the requested timestamp (may decode extra frames).

#### `audio_offset_time`

Adjusts audio relative to video in **milliseconds**.

* Negative → Audio plays earlier.
* Positive → Audio plays later.
  Useful for correcting A/V sync issues due to buffering.

---

#### `log_level`

Controls verbosity of internal logging. Messages are printed to `stderr` with severity labels.

Available values:

* **`EASYAV1_LOG_LEVEL_ERROR`**

  * Only errors are logged.
  * Critical failures (e.g., invalid file, decoder crash).
* **`EASYAV1_LOG_LEVEL_WARNING`**

  * Errors + warnings.
  * Includes recoverable issues (e.g., unsupported feature, skipped frame).
* **`EASYAV1_LOG_LEVEL_INFO`**

  * Errors + warnings + informational messages.
  * Includes stream details, initialization messages, seek operations, and decoder state transitions.
  * Most useful for debugging.

The default level is **`EASYAV1_LOG_LEVEL_WARNING`**.

---

### Related Functions

#### `easyav1_default_settings`

```c
EASYAV1_API easyav1_settings
easyav1_default_settings(void);
```

Returns a structure with default settings filled in.
Applications should always start from this.

#### `easyav1_get_current_settings`

```c
EASYAV1_API easyav1_settings
easyav1_get_current_settings(const easyav1_t *ctx);
```

Retrieves the current active settings from a decoder context.

#### `easyav1_update_settings`

```c
EASYAV1_API easyav1_status
easyav1_update_settings(easyav1_t *ctx,
                        const easyav1_settings *settings);
```

Updates decoder settings at runtime (not all fields may be changeable).

---

### Usage Example

```c
easyav1_settings s = easyav1_default_settings();
s.enable_audio = EASYAV1_TRUE;
s.enable_video = EASYAV1_TRUE;
s.use_fast_seeking = EASYAV1_TRUE;
s.audio_offset_time = -50; // Play audio 50ms earlier
s.log_level = EASYAV1_LOG_LEVEL_WARNING;

easyav1_t *ctx = easyav1_init_from_filename("movie.webm", &s);
```

---


## **8.2 Custom streams (`easyav1_stream`)**

### Overview

You can specify custom streams to be parsed by the decoder by initialising an `easyav1_t` instance with `easyav1_init_from_custom_stream`.

In fact, the library internally calls `easyav1_init_from_custom_stream`, using pre-populated `easyav1_stream`s.

---

### Structure Definition

```c
typedef struct {
    easyav1_read_func read_func;
    easyav1_seek_func seek_func;
    easyav1_tell_func tell_func;

    void *userdata;
} easyav1_stream;
```

---

### Field Reference

#### `easyav1_read_func`

```c
typedef int(*easyav1_read_func)(void *buffer, size_t size, void *userdata);
```

- `void *buffer` → A pointer to a buffer provided by 


# **8. Stream Information**

Before decoding begins, applications often need to inspect the properties of the WebM container — such as resolution, duration, sample rate, or codec details.
`easyav1` provides query functions to retrieve this information from the decoder context.

---

## 8.1 `easyav1_get_video_width` / `easyav1_get_video_height`

```c
EASYAV1_API int
easyav1_get_video_width(const easyav1_t *ctx);

EASYAV1_API int
easyav1_get_video_height(const easyav1_t *ctx);
```

### Description

Return the width and height of the video track in pixels.
Return `0` if video decoding is disabled or no video track is present.

---

## 8.2 `easyav1_get_framerate`

```c
EASYAV1_API double
easyav1_get_framerate(const easyav1_t *ctx);
```

### Description

Returns the average framerate of the video stream in frames per second.
If unknown, returns `0.0`.

---

## 8.3 `easyav1_get_duration`

```c
EASYAV1_API int64_t
easyav1_get_duration(const easyav1_t *ctx);
```

### Description

Returns the total duration of the media in **microseconds**.
If unknown, returns `-1`.

---

## 8.4 `easyav1_get_audio_channels` / `easyav1_get_audio_sample_rate`

```c
EASYAV1_API int
easyav1_get_audio_channels(const easyav1_t *ctx);

EASYAV1_API int
easyav1_get_audio_sample_rate(const easyav1_t *ctx);
```

### Description

* `get_audio_channels` → Number of channels in the selected audio track.
* `get_audio_sample_rate` → Audio sampling rate in Hz.
  Return `0` if no audio track is present.

---

## 8.5 Example: Printing Stream Metadata

```c
printf("Video: %dx%d @ %.2f fps\n",
       easyav1_get_video_width(ctx),
       easyav1_get_video_height(ctx),
       easyav1_get_framerate(ctx));

printf("Audio: %d channels @ %d Hz\n",
       easyav1_get_audio_channels(ctx),
       easyav1_get_audio_sample_rate(ctx));

printf("Duration: %lld ms\n",
       (long long)(easyav1_get_duration(ctx) / 1000));
```

---

# **9. Error Handling**

All functions in `easyav1` return status codes or special values to indicate success or failure.
Errors are **never reported by exceptions**; they must always be checked explicitly.

---

## 9.1 `easyav1_status`

The primary error reporting mechanism is the `easyav1_status` enumeration.

```c
typedef enum {
    EASYAV1_STATUS_OK = 0,          // Operation succeeded
    EASYAV1_STATUS_FINISHED,        // End of stream reached
    EASYAV1_STATUS_INVALID_PARAM,   // Invalid argument passed
    EASYAV1_STATUS_OUT_OF_MEMORY,   // Memory allocation failed
    EASYAV1_STATUS_IO_ERROR,        // I/O operation failed
    EASYAV1_STATUS_DECODER_ERROR,   // Decoder backend reported error
    EASYAV1_STATUS_NOT_SUPPORTED,   // Requested feature not supported
    EASYAV1_STATUS_INTERNAL_ERROR   // Unexpected internal error
} easyav1_status;
```

---

## 9.2 Common Return Patterns

* **Initialization functions** return `NULL` on failure.
* **Decoding functions** return an `easyav1_status`.
* **Queries** (e.g. width, height) return `0` or `-1` if invalid.

---

## 9.3 Error Propagation

Errors can originate from several sources:

* **I/O errors** (file not found, unexpected EOF).
* **Decoder errors** (corrupt AV1 bitstream).
* **Invalid usage** (passing a `NULL` context, calling functions in the wrong order).

The application is responsible for checking return codes and handling failures appropriately.

---

## 9.4 Example: Safe Decode Loop

```c
easyav1_status st;
while ((st = easyav1_decode_next(ctx)) == EASYAV1_STATUS_OK) {
    // consume frames
}

if (st == EASYAV1_STATUS_FINISHED) {
    printf("End of stream.\n");
} else if (st != EASYAV1_STATUS_OK) {
    fprintf(stderr, "Decoding failed (status=%d)\n", st);
}
```

---

## 9.5 Logging Levels

Errors and warnings are also logged according to the `log_level` field in `easyav1_settings`:

* **ERROR** → critical failures only.
* **WARNING** → recoverable issues.
* **INFO** → detailed diagnostics (recommended during development).

Logging is written to **stderr**. Applications that need custom logging should redirect or filter stderr output.

---

## 9.6 Best Practices

* Always check the return value of decoder functions.
* Use `EASYAV1_STATUS_FINISHED` to detect end-of-stream, not just `NULL`.
* During development, enable `EASYAV1_LOG_LEVEL_INFO` to catch subtle issues.
* On user-facing applications, prefer `WARNING` or `ERROR` only to reduce noise.

---

# **10. Thread Safety and Concurrency**

## 10.1 Overview

`easyav1` is designed to be lightweight and easy to embed.
It uses **internal worker threads** when `easyav1_play()` is active, but remains **single-threaded** in manual decoding mode.

Applications must respect the following concurrency rules to avoid race conditions.

---

## 10.2 Thread Ownership

* In **manual decoding mode** (using `easyav1_decode_next()`):

  * All calls must originate from the same application thread.
  * No background decoding threads are spawned.

* In **background playback mode** (using `easyav1_play()`):

  * A decoder thread continuously processes frames.
  * Application may poll for new frames from its own main loop.
  * Synchronization is handled internally.

---

## 10.3 Safe Function Calls

* **Always safe (thread-safe)**

  * `easyav1_has_video_frame`
  * `easyav1_get_video_frame`
  * `easyav1_has_audio_frame`
  * `easyav1_get_audio_frame`

* **Restricted (must not be called concurrently with decode loop)**

  * `easyav1_destroy` → ensure playback thread is stopped before destroying.
  * `easyav1_update_settings` → may only be called when playback is paused or during init.

---

## 10.4 Callbacks

When callbacks are configured in `easyav1_settings.callbacks`:

* **Video callback** is invoked by the decoder thread after a new frame is ready.
* **Audio callback** is invoked similarly after audio data is decoded.
* Both callbacks run in the decoder thread’s context.

### Callback Guidelines

* Keep callbacks short and non-blocking.
* Avoid calling back into `easyav1_*` functions from inside a callback.
* Use `userdata` for passing context (e.g., an SDL queue, ring buffer, or message system).

---

## 10.5 Typical Concurrency Model

1. **Main thread**: Handles UI, rendering, or SDL event loop.
2. **Decoder thread** (spawned by `easyav1_play`): Handles continuous decoding and invokes callbacks.
3. Synchronization is handled internally; applications only need to consume frames or buffer them.

---

## 10.6 Example: Polling vs Callbacks

* **Polling**

  ```c
  while (running) {
      if (easyav1_has_video_frame(ctx)) {
          const easyav1_video_frame *vf = easyav1_get_video_frame(ctx);
          render(vf);
      }
  }
  ```
* **Callbacks**

  ```c
  void video_cb(const easyav1_video_frame *vf, void *ud) {
      render(vf);
  }

  easyav1_settings s = easyav1_default_settings();
  s.callbacks.video = video_cb;
  s.callbacks.userdata = NULL;
  easyav1_t *ctx = easyav1_init_from_filename("file.webm", &s);
  easyav1_play(ctx);
  ```

---

## 10.7 Best Practices

* Use **callbacks** for streaming/real-time playback.
* Use **polling** when the application controls timing explicitly.
* Always destroy contexts (`easyav1_destroy`) after stopping playback.
* Avoid mixing manual decoding functions while `easyav1_play` is active.

---

## 10.8 Implementation detail

Internally, `easyav1` uses the dav1d library for decoding AV1 video. Dav1d manages its own worker threads to parallelize decoding. This is transparent to the application and does not affect how you interact with `easyav1`.

---

# **11. Utility Functions**

Besides initialization, decoding, and frame access, `easyav1` exposes a few additional helper functions that simplify integration.

---

## 11.1 Version Information

```c
EASYAV1_API const char *
easyav1_version(void);
```

### Description

Returns the version string of the `easyav1` library, in the format:

```
major.minor.patch
```

### Example

```c
printf("Using easyav1 version: %s\n", easyav1_version());
```

---

## 11.2 Seeking

```c
EASYAV1_API easyav1_status
easyav1_seek(easyav1_t *ctx, int64_t timestamp_us);
```

### Description

Seeks to the requested timestamp in microseconds.

* If `use_fast_seeking` is enabled in settings, the seek will jump to the **nearest keyframe before** the timestamp (fast but approximate).
* Otherwise, decoding continues until the exact timestamp is reached (slower but precise).

### Return Value

* `EASYAV1_STATUS_OK` on success.
* An error code on failure.

---

## 11.3 Get Current Timestamp

```c
EASYAV1_API int64_t
easyav1_get_position(const easyav1_t *ctx);
```

### Description

Returns the current playback position in microseconds.

* For manual decoding, corresponds to the last decoded frame.
* For background playback, corresponds to the most recent delivered frame.

---

## 11.4 End-of-Stream Detection

```c
EASYAV1_API easyav1_bool
easyav1_is_finished(const easyav1_t *ctx);
```

### Description

Checks whether the stream has reached its end.
Equivalent to receiving `EASYAV1_STATUS_FINISHED` from `easyav1_decode_next()`.

---

## 11.5 Example: Basic Seek

```c
int64_t seek_time = 30 * 1000000; // 30 seconds
if (easyav1_seek(ctx, seek_time) == EASYAV1_STATUS_OK) {
    printf("Seek successful, now at %lld ms\n",
           (long long)(easyav1_get_position(ctx) / 1000));
}
```

---


# 13. Tutorials

# **Tutorial 1: Minimal SDL3 Player**

This tutorial shows how to integrate `easyav1` with [SDL3](https://github.com/libsdl-org/SDL) to build a simple AV1 + Vorbis player.
It uses `easyav1_play()` to decode frames in the background and a main loop to render video and play audio.

---

## T1.1 Requirements

* `easyav1` library (built as described in Chapter 2).
* [SDL3 development libraries](https://github.com/libsdl-org/SDL).

Link against both `easyav1` and SDL3 in your project.

---

## T1.2 Code Listing

```c
#include <stdio.h>
#include <stdlib.h>
#include <SDL3/SDL.h>
#include "easyav1.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s file.webm\n", argv[0]);
        return 1;
    }

    // Initialize SDL (video + audio)
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "SDL init failed: %s\n", SDL_GetError());
        return 1;
    }

    // Open file with easyav1
    easyav1_t *ctx = easyav1_init_from_filename(argv[1], NULL);
    if (!ctx) {
        fprintf(stderr, "Failed to open file: %s\n", argv[1]);
        SDL_Quit();
        return 1;
    }

    // Start background decoding
    if (easyav1_play(ctx) != EASYAV1_STATUS_OK) {
        fprintf(stderr, "Failed to start playback.\n");
        easyav1_destroy(&ctx);
        SDL_Quit();
        return 1;
    }

    // Create SDL window and renderer
    SDL_Window *win = SDL_CreateWindow("easyav1 player", 800, 600, 0);
    SDL_Renderer *renderer = SDL_CreateRenderer(win, NULL);

    // Create texture for video frames (YUV420 → SDL planar format)
    SDL_Texture *texture = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_IYUV,   // matches YUV420 from easyav1
        SDL_TEXTUREACCESS_STREAMING,
        easyav1_get_video_width(ctx),
        easyav1_get_video_height(ctx)
    );

    // Open SDL audio device
    SDL_AudioSpec spec = {0};
    spec.freq = easyav1_get_audio_sample_rate(ctx);
    spec.format = SDL_AUDIO_F32;
    spec.channels = easyav1_get_audio_channels(ctx);
    SDL_AudioDeviceID audio_dev = SDL_OpenAudioDevice(NULL, &spec);
    SDL_ResumeAudioDevice(audio_dev);

    // Main loop
    int running = 1;
    while (running && easyav1_is_finished(ctx) == EASYAV1_FALSE) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) running = 0;
        }

        // If we have a new video frame
        if (easyav1_has_video_frame(ctx) == EASYAV1_TRUE) {
            const easyav1_video_frame *vf = easyav1_get_video_frame(ctx);

            // Upload planes to SDL texture
            SDL_UpdateYUVTexture(
                texture, NULL,
                vf->planes[0], vf->stride[0],
                vf->planes[1], vf->stride[1],
                vf->planes[2], vf->stride[2]
            );

            SDL_RenderClear(renderer);
            SDL_RenderTexture(renderer, texture, NULL, NULL);
            SDL_RenderPresent(renderer);
        }

        // If we have a new audio frame
        if (easyav1_has_audio_frame(ctx) == EASYAV1_TRUE) {
            const easyav1_audio_frame *af = easyav1_get_audio_frame(ctx);
            size_t bytes = af->samples * af->channels * sizeof(float);
            SDL_QueueAudio(audio_dev, af->samples_data, bytes);
        }

        SDL_Delay(10); // small delay to reduce CPU usage
    }

    // Cleanup
    easyav1_destroy(&ctx);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(win);
    SDL_CloseAudioDevice(audio_dev);
    SDL_Quit();
    return 0;
}
```

---

## T1.3 How It Works

1. **Initialization**: SDL is initialized for video and audio; `easyav1` opens the file.
2. **Background Playback**: `easyav1_play()` runs decoding in a separate thread.
3. **Video Rendering**: Each available decoded frame is uploaded to an SDL texture and rendered.
4. **Audio Playback**: PCM float samples are queued into SDL’s audio device.
5. **Event Loop**: The app polls SDL events and exits when the window is closed.

---

## T1.4 Notes

* This example uses **SDL\_PIXELFORMAT\_IYUV**, which matches `easyav1`’s YUV420 output.
* Audio is provided as **32-bit float PCM**, directly compatible with SDL.
* Frames may be dropped if rendering is slower than decoding — this is expected in real-time playback.
* For synchronization (A/V sync), you can compare the `pts` values of frames with SDL’s playback clock.

---

# **Tutorial 2: Using Callbacks**

So far, we’ve shown how to poll frames using `easyav1_has_video_frame()` and `easyav1_get_video_frame()`.
An alternative approach is to use **callbacks**, which lets `easyav1` deliver frames to your code automatically.
This is especially useful in **background playback mode** with `easyav1_play()`.

---

## T2.1 Defining Callbacks

Video and audio callbacks follow these signatures:

```c
void video_callback(const easyav1_video_frame *frame, void *userdata);
void audio_callback(const easyav1_audio_frame *frame, void *userdata);
```

Both receive:

* a pointer to the decoded frame, and
* the `userdata` pointer you specified in `easyav1_settings`.

---

## T2.2 Setting Up

Here’s a minimal example with only a video callback:

```c
#include <stdio.h>
#include "easyav1.h"

static void my_video_cb(const easyav1_video_frame *frame, void *userdata) {
    printf("Video frame: %dx%d @ pts=%lld µs\n",
           frame->width, frame->height,
           (long long)frame->pts);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s input.webm\n", argv[0]);
        return 1;
    }

    // Start from defaults
    easyav1_settings s = easyav1_default_settings();
    s.enable_video = EASYAV1_TRUE;
    s.enable_audio = EASYAV1_FALSE; // disable audio for this example
    s.callbacks.video = my_video_cb;
    s.callbacks.audio = NULL;
    s.callbacks.userdata = NULL;

    // Initialize and start playback
    easyav1_t *ctx = easyav1_init_from_filename(argv[1], &s);
    if (!ctx) {
        fprintf(stderr, "Failed to open file.\n");
        return 1;
    }

    easyav1_play(ctx);

    // Application main loop
    for (;;) {
        if (easyav1_is_finished(ctx)) break;
        // In a real app, process events or sleep here
    }

    easyav1_destroy(ctx);
    return 0;
}
```

---

## T2.3 Adding Audio Callback

```c
static void my_audio_cb(const easyav1_audio_frame *frame, void *userdata) {
    printf("Audio frame: %d samples, %d channels\n",
           frame->samples, frame->channels);
}

s.callbacks.audio = my_audio_cb;
```

---

## T2.4 Key Notes

* Callbacks are invoked **in the decoder thread** created by `easyav1_play`.
* Keep callbacks **fast and non-blocking**. For example, push frames into a thread-safe queue instead of doing heavy work inside the callback.
* Use `userdata` to pass a context pointer to your rendering or audio system.
* If you don’t set a callback (`NULL`), no function is called for that type of frame.


---

# **Tutorial 3: Seeking in a Video**

This tutorial demonstrates how to use `easyav1_seek()` to jump to a specific position in a video.
Seeking is useful for implementing features like scrubbing in a media player.

---

## T3.1 Basics of Seeking

* `easyav1_seek(ctx, timestamp_us)` takes a timestamp in **microseconds**.
* The exact behavior depends on the `use_fast_seeking` setting:

  * **Fast seeking enabled** → jumps to the nearest keyframe before the timestamp.
  * **Fast seeking disabled** → decodes forward until the exact timestamp is reached (slower).

---

## T3.2 Example: Jump to 10 Seconds

```c
#include <stdio.h>
#include "easyav1.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s input.webm\n", argv[0]);
        return 1;
    }

    easyav1_settings s = easyav1_default_settings();
    s.enable_video = EASYAV1_TRUE;
    s.enable_audio = EASYAV1_FALSE;
    s.use_fast_seeking = EASYAV1_TRUE;

    easyav1_t *ctx = easyav1_init_from_filename(argv[1], &s);
    if (!ctx) {
        fprintf(stderr, "Failed to open file.\n");
        return 1;
    }

    // Seek to 10 seconds (10,000,000 microseconds)
    int64_t target = 10 * 1000000;
    if (easyav1_seek(ctx, target) == EASYAV1_STATUS_OK) {
        printf("Seeked to ~10s\n");
    } else {
        fprintf(stderr, "Seek failed\n");
    }

    // Decode a few frames after seeking
    for (int i = 0; i < 5; i++) {
        if (easyav1_decode_next(ctx) != EASYAV1_STATUS_OK) break;
        if (easyav1_has_video_frame(ctx)) {
            const easyav1_video_frame *vf = easyav1_get_video_frame(ctx);
            printf("Frame after seek: pts=%lld µs\n", (long long)vf->pts);
        }
    }

    easyav1_destroy(ctx);
    return 0;
}
```

---

## T3.3 Tips

* Always check the return value of `easyav1_seek()`.
* After seeking, discard frames until you reach the desired timestamp.
* For smoother scrubbing, enable `use_fast_seeking` in settings.
* For precise positioning (e.g. exact frame thumbnails), disable `use_fast_seeking`.

---

# **Tutorial 4: Adjusting Audio Synchronization**

Sometimes video and audio may drift out of sync, either due to buffering, audio driver latency, or quirks in the source file.
The `audio_offset_time` setting in `easyav1_settings` lets you correct this by shifting audio relative to video.

---

## T4.1 `audio_offset_time`

* Unit: **milliseconds**
* Negative value → audio plays **earlier** than video
* Positive value → audio plays **later** than video

This adjustment is **added** to any internal delay specified in the WebM file.

---

## T4.2 Example: Advancing Audio by 50ms

```c
#include <stdio.h>
#include "easyav1.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s input.webm\n", argv[0]);
        return 1;
    }

    easyav1_settings s = easyav1_default_settings();
    s.enable_video = EASYAV1_TRUE;
    s.enable_audio = EASYAV1_TRUE;

    // Play audio 50ms earlier than video
    s.audio_offset_time = -50;

    easyav1_t *ctx = easyav1_init_from_filename(argv[1], &s);
    if (!ctx) {
        fprintf(stderr, "Failed to open file.\n");
        return 1;
    }

    easyav1_play(ctx);

    while (!easyav1_is_finished(ctx)) {
        // Normally, the main loop would handle events/rendering
    }

    easyav1_destroy(ctx);
    return 0;
}
```

---

## T4.3 SDL Audio Case

When using **SDL2 or SDL3** for audio output, you may need to compensate for its internal buffer size.

A good rule of thumb is:

```
audio_offset_time = (SDL_AudioSpec.samples / SDL_AudioSpec.freq) * 1000
```

This shifts playback so that audio aligns correctly with video.

---

## T4.4 Notes

* Fine-tuning may be required depending on the platform and audio backend.
* Too large of a correction can cause noticeable lip-sync mismatches.
* Adjustments can be applied both at initialization and dynamically using `easyav1_update_settings()`.

---

# **Tutorial 5: Error Handling and Logging**

Robust applications must deal with unexpected input, decoder errors, and I/O problems gracefully.
`easyav1` provides two main mechanisms for this:

1. **Return codes** (`easyav1_status`)
2. **Logging** (controlled by `log_level` in `easyav1_settings`)

---

## T5.1 Checking Return Codes

Every decoder function returns either:

* an `easyav1_status` (OK, FINISHED, or an error code), or
* a special value (`NULL`, `0`, or `-1`) on failure.

### Example: Safe Decode Loop

```c
easyav1_status st;
while ((st = easyav1_decode_next(ctx)) == EASYAV1_STATUS_OK) {
    if (easyav1_has_video_frame(ctx)) {
        const easyav1_video_frame *vf = easyav1_get_video_frame(ctx);
        printf("Frame: pts=%lld µs\n", (long long)vf->pts);
    }
}

if (st == EASYAV1_STATUS_FINISHED) {
    printf("End of stream reached.\n");
} else {
    fprintf(stderr, "Decoding failed (status=%d)\n", st);
}
```

---

## T5.2 Using Logging

Set `log_level` in `easyav1_settings` before initialization:

```c
s.log_level = EASYAV1_LOG_LEVEL_INFO;
```

### Levels

* **ERROR** → only critical failures
* **WARNING** → recoverable issues too
* **INFO** → detailed diagnostics (decoder events, seeking, initialization)

Logs are printed to **stderr**.

---

## T5.3 Example: Debugging with Verbose Logging

```c
easyav1_settings s = easyav1_default_settings();
s.log_level = EASYAV1_LOG_LEVEL_INFO;  // Verbose
easyav1_t *ctx = easyav1_init_from_filename("broken.webm", &s);

if (!ctx) {
    fprintf(stderr, "Could not open file.\n");
    return 1;
}
```

This will produce messages like:

```
[INFO] Opening file: broken.webm
[WARNING] Frame skipped due to corruption
[ERROR] Decoder failed at position 12345
```

---

## T5.4 Best Practices

* Always check return codes — don’t assume decoding always succeeds.
* Use `EASYAV1_LOG_LEVEL_INFO` during development to detect subtle issues.
* Switch to `WARNING` or `ERROR` in production to reduce log noise.
* On user-facing applications, handle `EASYAV1_STATUS_FINISHED` gracefully (end-of-stream is not an error).

---

# **Appendix A: Platform Support**

## A.1 Supported Platforms

The `easyav1` library has been successfully tested and verified to work on the following platforms:

* **Windows**
* **Linux**
* **macOS** (including Universal Binaries for Intel and Apple Silicon)
* **Android**
* **iOS**
* **Nintendo Switch**
* **PlayStation Vita**

Cross-compilation for mobile and console platforms has been validated, and the library does not depend on OS-specific APIs. Portability is primarily determined by availability of threading and the `dav1d` decoder backend.

---

## A.2 Unsupported Platforms

* **Emscripten / WebAssembly**
  `easyav1` is currently not compatible with Emscripten due to reliance on multi-threading. Threading support in WebAssembly remains limited and prevents proper use of the internal decoder pipeline.

---

## A.3 Platform-Specific Notes

* **Mobile (Android / iOS):** Audio/video synchronization may require adjustment using `audio_offset_time` (see Tutorial 4).
* **Nintendo Switch:** Performs well at native resolution with standard CPU/GPU allocations.
* **PlayStation Vita:**

  * Can sustain **60 FPS playback** at half resolution (≈ 480×272).
  * At native Vita resolution (960×544), the CPU is not fast enough, and video playback becomes choppy. Developers should downscale video for smooth playback.


---

# Appendix: Function Reference (Quick List)

List of API functions...
