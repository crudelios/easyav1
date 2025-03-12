# Script to build dav1d with CMake
# This script is based on the original script by Miku AuahDark, available at
# https://github.com/MikuAuahDark/dav1d-cmake
#
# This script was heavily edited by me - José Cadete - to support david up to v1.5.1
#
# This CMake script also depends on TargetArch.cmake which is now bundled in this file,
# modified to detect AArch64, Risc-V and Loongarch. To get the unmodified version, visit
# https://github.com/axr/solar-cmake/blob/73cfea0/TargetArch.cmake
#
# Both the original script and the original TargetArch.cmake are licensed under the BSD-2-Clause license.
# My modifications to this file and to TargetArch.cmake are licensed under the same BSD-2-Clause license.

cmake_minimum_required(VERSION 3.8.0...4.0.0)

set(CMAKE_C_STANDARD 99)

set(DAVID_VERSION_MAJOR 1)
set(DAVID_VERSION_MINOR 5)
set(DAVID_VERSION_PATCH 1)
set(DAVID_VERSION "${DAVID_VERSION_MAJOR}.${DAVID_VERSION_MINOR}.${DAVID_VERSION_PATCH}")

project(dav1d VERSION "${DAVID_VERSION}" LANGUAGES C)
set(PROJECT_VERSION_REVISION ${PROJECT_VERSION_PATCH})

if(POLICY CMP0025)
    cmake_policy(SET CMP0025 NEW)
endif()


# API version

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/dav1d/version.h" DAV1D_API_VERSION_MAJOR_LINE REGEX "^#define[ \t]+DAV1D_API_VERSION_MAJOR[ \t]+[0-9]+$")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/dav1d/version.h" DAV1D_API_VERSION_MINOR_LINE REGEX "^#define[ \t]+DAV1D_API_VERSION_MINOR[ \t]+[0-9]+$")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/dav1d/version.h" DAV1D_API_VERSION_PATCH_LINE REGEX "^#define[ \t]+DAV1D_API_VERSION_PATCH[ \t]+[0-9]+$")
string(REGEX REPLACE "^#define[ \t]+DAV1D_API_VERSION_MAJOR[ \t]+([0-9]+)$" "\\1" DAV1D_API_VERSION_MAJOR "${DAV1D_API_VERSION_MAJOR_LINE}")
string(REGEX REPLACE "^#define[ \t]+DAV1D_API_VERSION_MINOR[ \t]+([0-9]+)$" "\\1" DAV1D_API_VERSION_MINOR "${DAV1D_API_VERSION_MINOR_LINE}")
string(REGEX REPLACE "^#define[ \t]+DAV1D_API_VERSION_PATCH[ \t]+([0-9]+)$" "\\1" DAV1D_API_VERSION_PATCH "${DAV1D_API_VERSION_PATCH_LINE}")
unset(DAV1D_API_VERSION_MAJOR_LINE)
unset(DAV1D_API_VERSION_MINOR_LINE)
unset(DAV1D_API_VERSION_PATCH_LINE)

set(DAV1D_SONAME_VERSION "${DAV1D_API_VERSION_MAJOR}.${DAV1D_API_VERSION_MINOR}.${DAV1D_API_VERSION_PATCH}")


####################
# Helper functions #
####################

# Target architecture detection

# Based on the Qt 5 processor detection code, so should be very accurate
# https://qt.gitorious.org/qt/qtbase/blobs/master/src/corelib/global/qprocessordetection.h
# Currently handles arm (v5, v6, v7), x86 (32/64), ia64, and ppc (32/64)

# Regarding POWER/PowerPC, just as is noted in the Qt source,
# "There are many more known variants/revisions that we do not handle/detect."

# Set ppc_support to TRUE before including this file or ppc and ppc64
# will be treated as invalid architectures since they are no longer supported by Apple

function(target_architecture output_var)
    if(APPLE AND CMAKE_OSX_ARCHITECTURES)
        # On OS X we use CMAKE_OSX_ARCHITECTURES *if* it was set
        # First let's normalize the order of the values

        # Note that it's not possible to compile PowerPC applications if you are using
        # the OS X SDK version 10.6 or later - you'll need 10.4/10.5 for that, so we
        # disable it by default
        # See this page for more information:
        # http://stackoverflow.com/questions/5333490/how-can-we-restore-ppc-ppc64-as-well-as-full-10-4-10-5-sdk-support-to-xcode-4

        # Architecture defaults to i386 or ppc on OS X 10.5 and earlier, depending on the CPU type detected at runtime.
        # On OS X 10.6+ the default is x86_64 if the CPU supports it, i386 otherwise.

        foreach(osx_arch ${CMAKE_OSX_ARCHITECTURES})
            if("${osx_arch}" STREQUAL "ppc" AND ppc_support)
                set(osx_arch_ppc TRUE)
            elseif("${osx_arch}" STREQUAL "i386")
                set(osx_arch_i386 TRUE)
            elseif("${osx_arch}" STREQUAL "x86_64")
                set(osx_arch_x86_64 TRUE)
            elseif("${osx_arch}" STREQUAL "ppc64" AND ppc_support)
                set(osx_arch_ppc64 TRUE)
            else()
                message(FATAL_ERROR "Invalid OS X arch name: ${osx_arch}")
            endif()
        endforeach()

        # Now add all the architectures in our normalized order
        if(osx_arch_ppc)
            list(APPEND ARCH ppc)
        endif()

        if(osx_arch_i386)
            list(APPEND ARCH i386)
        endif()

        if(osx_arch_x86_64)
            list(APPEND ARCH x86_64)
        endif()

        if(osx_arch_ppc64)
            list(APPEND ARCH ppc64)
        endif()
    else()

        # Detect the architecture in a rather creative way...
        # This compiles a small C program which is a series of ifdefs that selects a
        # particular #error preprocessor directive whose message string contains the
        # target architecture. The program will always fail to compile (both because
        # file is not a valid C program, and obviously because of the presence of the
        # #error preprocessor directives... but by exploiting the preprocessor in this
        # way, we can detect the correct target architecture even when cross-compiling,
        # since the program itself never needs to be run (only the compiler/preprocessor)

        set(archdetect_c_code "
            #if defined(__arm__) || defined(__TARGET_ARCH_ARM) || defined(_M_ARM) || defined(_M_ARM64)
                #if defined(__aarch64__)  || defined(_M_ARM64)
                    #error cmake_ARCH aarch64
                #elif defined(__ARM_ARCH_7__) \\
                    || defined(__ARM_ARCH_7A__) \\
                    || defined(__ARM_ARCH_7R__) \\
                    || defined(__ARM_ARCH_7M__) \\
                    || (defined(__TARGET_ARCH_ARM) && __TARGET_ARCH_ARM-0 >= 7)
                    #error cmake_ARCH armv7
                #elif defined(__ARM_ARCH_6__) \\
                    || defined(__ARM_ARCH_6J__) \\
                    || defined(__ARM_ARCH_6T2__) \\
                    || defined(__ARM_ARCH_6Z__) \\
                    || defined(__ARM_ARCH_6K__) \\
                    || defined(__ARM_ARCH_6ZK__) \\
                    || defined(__ARM_ARCH_6M__) \\
                    || (defined(__TARGET_ARCH_ARM) && __TARGET_ARCH_ARM-0 >= 6)
                    #error cmake_ARCH armv6
                #elif defined(__ARM_ARCH_5TEJ__) \\
                    || (defined(__TARGET_ARCH_ARM) && __TARGET_ARCH_ARM-0 >= 5)
                    #error cmake_ARCH armv5
                #else
                    #error cmake_ARCH arm
                #endif
            #elif defined(__i386) || defined(__i386__) || defined(_M_IX86)
                #error cmake_ARCH i386
            #elif defined(__x86_64) || defined(__x86_64__) || defined(__amd64) || defined(_M_X64)
                #error cmake_ARCH x86_64
            #elif defined(__ia64) || defined(__ia64__) || defined(_M_IA64)
                #error cmake_ARCH ia64
            #elif defined(__ppc__) || defined(__ppc) || defined(__powerpc__) \\
                || defined(_ARCH_COM) || defined(_ARCH_PWR) || defined(_ARCH_PPC)  \\
                || defined(_M_MPPC) || defined(_M_PPC)
                #if defined(__ppc64__) || defined(__powerpc64__) || defined(__64BIT__)
                    #error cmake_ARCH ppc64
                #else
                    #error cmake_ARCH ppc
                #endif
            #elif defined(__riscv) || defined(__riscv__)
                #if defined(_LP64) || defined(__LP64__)
                    #error cmake_ARCH riscv64
                #else
                    #error cmake_ARCH riscv32
                #endif
            #elif defined(__loongarch__) || defined(_LP64) || defined(__LP64__)
                #if defined(__loongarch_lp64)
                    #error cmake_ARCH loongarch64
                #else
                    #error cmake_ARCH loongarch32
                #endif
            #endif

            #error cmake_ARCH unknown
        ")
        try_run(
            run_result_unused
            compile_result_unused
            SOURCE_FROM_CONTENT "arch.c" 
            "${archdetect_c_code}"
            COMPILE_OUTPUT_VARIABLE ARCH
            CMAKE_FLAGS CMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}
        )

        # Parse the architecture name from the compiler output
        string(REGEX MATCH "cmake_ARCH ([a-zA-Z0-9_]+)" ARCH "${ARCH}")

        # Get rid of the value marker leaving just the architecture name
        string(REPLACE "cmake_ARCH " "" ARCH "${ARCH}")

        # If we are compiling with an unknown architecture this variable should
        # already be set to "unknown" but in the case that it's empty (i.e. due
        # to a typo in the code), then set it to unknown
        if (NOT ARCH)
            set(ARCH unknown)
        else()
            message(STATUS "Detected processor: ${ARCH}")
        endif()
    endif()

    set(${output_var} "${ARCH}" PARENT_SCOPE)
endfunction()
target_architecture(PROCESSOR)

if(PROCESSOR STREQUAL "i386" OR PROCESSOR STREQUAL "x86_64")
    set(ARCH_X86 TRUE)
endif()


# Configuration file generation

function(add_to_configure_file)
    if(ARGC LESS 3)
        message(FATAL_ERROR "add_to_configure_file: not enough arguments")
    endif()
    set(CONFIG_NAME "${ARGV0}")
    set(VARIABLE "${ARGV1}")
    if (ARGC LESS 4)
        set(VALUE "${${ARGV1}}")
        set(TYPE "${ARGV2}")
    else()
        set(VALUE "${ARGV2}")
        set(TYPE "${ARGV3}")
    endif()

	if (TYPE STREQUAL "TYPE_BOOLEAN")
		if (${VALUE})
			set("${CONFIG_NAME}" "${${CONFIG_NAME}}#define ${VARIABLE} 1\n" PARENT_SCOPE)
		else()
			set("${CONFIG_NAME}" "${${CONFIG_NAME}}#define ${VARIABLE} 0\n" PARENT_SCOPE)
		endif()
	elseif (TYPE STREQUAL "TYPE_STRING")
		set("${CONFIG_NAME}" "${${CONFIG_NAME}}#define ${VARIABLE} \"${VALUE}\"\n" PARENT_SCOPE)
	elseif (TYPE STREQUAL "TYPE_EXPRESSION")
		set("${CONFIG_NAME}" "${${CONFIG_NAME}}#define ${VARIABLE} ${VALUE}\n" PARENT_SCOPE)
	else()
		message(WARNING "add_to_configure_file: invalid type, ignoring")
		return()
	endif()
endfunction()

function(generate_configuration_file CONFIG_NAME TYPE)
    if (TYPE STREQUAL "C")
        set(INTRO "/***\n * File generated automatically by CMake.\n * Please do not edit this file manually.\n ***/\n\n#pragma once\n\n")
    else()
        set(INTRO "; File generated automatically by CMake.\n; Please do not edit this file manually.\n\n")
        string(REPLACE "#" "%" ${CONFIG_NAME} ${${CONFIG_NAME}})
    endif()

	file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/${CONFIG_NAME}" "${INTRO}${${CONFIG_NAME}}")

    message(STATUS "Writing configuration file ${CONFIG_NAME}")
endfunction()



###########
# Options #
###########

# bitdepths
set(bitdepths "8" CACHE STRING "Enable only specified bitdepths")
set_property(CACHE bitdepths PROPERTY STRINGS All 8 16)

if (bitdepths STREQUAL "All")
    set(bitdepths 8 16)
elseif (bitdepths STREQUAL "8")
    set(bitdepths 8)
elseif (bitdepths STREQUAL "16")
    set(bitdepths 16)
else()
    message(FATAL_ERROR "Invalid bitdepths value")
endif()

foreach(BITS IN LISTS bitdepths)
    set(CONFIG_${BITS}BPC 1)
endforeach()

add_to_configure_file("config.h" "CONFIG_8BPC" TYPE_BOOLEAN)
add_to_configure_file("config.h" "CONFIG_16BPC" TYPE_BOOLEAN)


# enable_asm
option(enable_asm "Build asm files, if available" ON)

include(CheckSymbolExists)

if (enable_asm)
    if(${PROCESSOR} STREQUAL "i386" OR ${PROCESSOR} STREQUAL "x86_64" OR
        ${PROCESSOR} STREQUAL "aarch64" OR ${PROCESSOR} MATCHES "^arm" OR
        ${PROCESSOR} STREQUAL "ppc64" OR ${PROCESSOR} MATCHES "^riscv" OR ${PROCESSOR} MATCHES "^loongarch")
        set(HAVE_ASM 1)
    endif()
    if (HAVE_ASM AND ${PROCESSOR} STREQUAL "x86_64")
        check_symbol_exists(__ILP32__ "stdio.h" HAS_IPL32)
        if(HAS_IPL32)
            unset(HAVE_ASM)
        endif()
    endif()
endif()

add_to_configure_file("config.h" "HAVE_ASM" TYPE_BOOLEAN)


# trim-dsp
set(trim_dsp "if-release" CACHE STRING "Eliminate redundant DSP functions where possible'")
set_property(CACHE trim_dsp PROPERTY STRINGS "true" "false" "if-release")

if (trim_dsp)
    if (trim_dsp STREQUAL "if-release" AND "${CMAKE_BUILD_TYPE}" STREQUAL "Release")
        set(TRIM_DSP_FUNCTIONS 1)
    else()
        set(TRIM_DSP_FUNCTIONS ${trim_dsp})
    endif()
endif()

add_to_configure_file("config.h" "TRIM_DSP_FUNCTIONS" TYPE_BOOLEAN)


# logging
option(logging "Print error log messages using the provided callback function" ON)

add_to_configure_file("config.h" "CONFIG_LOG" "${logging}" TYPE_BOOLEAN)


# macos kperf
option(macos_kperf "Use the private macOS kperf API for benchmarking" OFF)

add_to_configure_file("config.h" "CONFIG_MACOS_KPERF" "${macos_kperf}" TYPE_BOOLEAN)


# TODO testdata_tests
# TODO fuzzing_engine
# TODO fuzzer_ldflags


# stack_alignment
set(stack_alignment "0" CACHE STRING "")

##############
# End Option #
##############


#
# OS/Compiler checks and defines
#

set(TEMP_COMPILE_DEFS "")
set(TEMP_COMPILE_FLAGS "")
set(TEMP_LINK_LIBRARIES "")
set(TEMP_COMPAT_FILES "")
set(RT_DEPENDENCY "")

if(${CMAKE_SYSTEM_NAME} MATCHES "Linux" OR ${CMAKE_SYSTEM_NAME} MATCHES "GNU" OR ${CMAKE_SYSTEM_NAME} MATCHES "Emscripten")
    list(APPEND TEMP_COMPILE_DEFS -D_GNU_SOURCE)
endif()

set(CMAKE_REQUIRED_DEFINITIONS ${TEMP_COMPILE_DEFS})

if(WIN32)
    add_to_configure_file("config.h" "_WIN32_WINNT" 0x0601 TYPE_EXPRESSION)
    add_to_configure_file("config.h" "UNICODE" 1 TYPE_BOOLEAN) # Define to 1 for Unicode (Wide Chars) APIs
    add_to_configure_file("config.h" "_UNICODE" 1 TYPE_BOOLEAN) # Define to 1 for Unicode (Wide Chars) APIs
    add_to_configure_file("config.h" "__USE_MINGW_ANSI_STDIO" 1 TYPE_BOOLEAN) # Define to force use of MinGW printf
    add_to_configure_file("config.h" "_CRT_DECLARE_NONSTDC_NAMES" 1 TYPE_BOOLEAN) # Define to get off_t from sys/types.h on MSVC

    check_symbol_exists(fseeko "stdio.h" HAS_FSEEKO)

    if(HAS_FSEEKO)
        add_to_configure_file("config.h" "_FILE_OFFSET_BITS" 64 TYPE_EXPRESSION)
    else()
        add_to_configure_file("config.h" "fseeko" "_fseeki64" TYPE_EXPRESSION)
        add_to_configure_file("config.h" "ftello" "_ftelli64" TYPE_EXPRESSION)
    endif()

    if (${PROCESSOR} STREQUAL "x86_64" AND NOT MSVC)
        list(APPEND TEMP_COMPILE_FLAGS "-Wl,--dynamicbase,--nxcompat,--tsaware,--high-entropy-va")
    elseif(${PROCESSOR} STREQUAL "i386" OR ${PROCESSOR} MATCHES "^arm")
        if (MSVC)
            list(APPEND TEMP_COMPILE_FLAGS "/largeaddressaware")
        else()
            list(APPEND TEMP_COMPILE_FLAGS "-Wl,--dynamicbase,--nxcompat,--tsaware,--large-address-aware")
        endif()
    endif()

    # On Windows, we use a compatibility layer to emulate pthread
    add_library(thread_compat_dep STATIC ${CMAKE_CURRENT_SOURCE_DIR}/src/win32/thread.c)
    target_include_directories(thread_compat_dep PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_BINARY_DIR}
    )
    set(THREAD_DEPENDENCY thread_compat_dep)
else()
    find_package(Threads REQUIRED)
    set(THREAD_DEPENDENCY Threads::Threads)

    check_symbol_exists(clock_gettime "time.h" HAVE_CLOCK_GETTIME)

    if(NOT HAVE_CLOCK_GETTIME)
        if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin" OR ${CMAKE_SYSTEM_NAME} MATCHES "iOS" OR ${CMAKE_SYSTEM_NAME} MATCHES "tvOS")
            find_library(LIBRT rt)
            if (LIBRT)
                list(APPEND TEMP_LINK_LIBRARIES ${LIBRT})
            endif()
            check_symbol_exists(clock_gettime "time.h" HAVE_CLOCK_GETTIME)
        endif()
        if(NOT HAVE_CLOCK_GETTIME)
            message(FATAL_ERROR "clock_gettime not found")
        endif()
    endif()

    check_symbol_exists(posix_memalign "stdlib.h" HAVE_POSIX_MEMALIGN)
    check_symbol_exists(memalign "malloc.h" HAVE_MEMALIGN)
    check_symbol_exists(aligned_alloc "stdlib.h" HAVE_ALIGNED_ALLOC)
endif()

add_to_configure_file("config.h" "HAVE_CLOCK_GETTIME" TYPE_BOOLEAN)
add_to_configure_file("config.h" "HAVE_POSIX_MEMALIGN" TYPE_BOOLEAN)
add_to_configure_file("config.h" "HAVE_MEMALIGN" TYPE_BOOLEAN)
add_to_configure_file("config.h" "HAVE_ALIGNED_ALLOC" TYPE_BOOLEAN)

# check for fseeko on android. It is not always available if _FILE_OFFSET_BITS is defined to 64
if(${CMAKE_SYSTEM_NAME} STREQUAL "Android")
    check_symbol_exists(fseeko "stdio.h" HAS_FSEEKO)

    if(NOT HAS_FSEEKO)
        set(CMAKE_REQUIRED_FLAGS "-U_FILE_OFFSET_BITS")
        check_symbol_exists(fseeko "stdio.h" HAS_FSEEKO)
        unset(CMAKE_REQUIRED_FLAGS)

        if(HAS_FSEEKO)
            message(WARNING "Files larger than 2 gigabytes might not be supported in the dav1d CLI tool.")
            list(APPEND TEMP_COMPILE_FLAGS -U_FILE_OFFSET_BITS)
        elseif(enable_tools)
            message(FATAL_ERROR "dav1d CLI tool needs fseeko()")
        else()
            unset(HAS_FSEEKO)
        endif()
    endif()
endif()

if(${CMAKE_SYSTEM_NAME} MATCHES "Linux")
    set(CMAKE_REQUIRED_LIBRARIES ${CMAKE_DL_LIBS})
    check_symbol_exists(dlsym "dlfcn.h" HAVE_DLSYM)
    unset(CMAKE_REQUIRED_LIBRARIES)

    if(HAVE_DLSYM)
        list(APPEND TEMP_LINK_LIBRARIES ${CMAKE_DL_LIBS})
    endif()
endif()

add_to_configure_file("config.h" "HAVE_DLSYM" TYPE_BOOLEAN)

#
# Header checks
#
include(CheckIncludeFile)

check_include_file(stdatomic.h HAS_STDATOMIC)

if(NOT HAS_STDATOMIC)
    if(MSVC)
        # we have a custom replacement for MSVC
        add_library(stdatomic_dependency INTERFACE)
        target_include_directories(stdatomic_dependency INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include/compat/msvc)
    else()
        try_compile(_GCC_STYLE_ATOMICS SOURCE_FROM_CONTENT "atomics-test.c" "int main() { int v = 0; return __atomic_fetch_add(&v, 1, __ATOMIC_SEQ_CST); return 0; }"
            COMPILE_DEFINITIONS ${TEMP_COMPILE_DEFS}
        )
        if(_GCC_STYLE_ATOMICS)
            add_library(stdatomic_dependency INTERFACE)
            target_include_directories(stdatomic_dependency INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include/compat/gcc)
        else()
            message(FATAL_ERROR "Atomics not supported")
        endif()
    endif()
endif()

check_include_file(sys/types.h HAVE_SYS_TYPES_H)
check_include_file(unistd.h HAVE_UNISTD_H)
check_include_file(io.h HAVE_IO_H)
check_include_file(pthread_np.h HAVE_PTHREAD_NP_H)

if(HAVE_PTHREAD_NP_H)
    list(APPEND TEMP_COMPILE_DEFS -DHAVE_PTHREAD_NP_H=1)
else()
    list(APPEND TEMP_COMPILE_DEFS -DHAVE_PTHREAD_NP_H=0)
endif()

add_to_configure_file("config.h" "HAVE_SYS_TYPES_H" TYPE_BOOLEAN)
add_to_configure_file("config.h" "HAVE_UNISTD_H" TYPE_BOOLEAN)
add_to_configure_file("config.h" "HAVE_IO_H" TYPE_BOOLEAN)
add_to_configure_file("config.h" "HAVE_PTHREAD_NP_H" TYPE_BOOLEAN)

#
# Function checks
#
set(CMAKE_REQUIRED_DEFINITIONS ${TEMP_COMPILE_DEFS})
check_symbol_exists(getopt_long "getopt.h" HAS_GETOPT_LONG)

if(NOT HAS_GETOPT_LONG)
    add_library(getopt_dependency STATIC ${CMAKE_CURRENT_SOURCE_DIR}/tools/compat/getopt.c)
    set_property(TARGET getopt_dependency PROPERTY POSITION_INDEPENDENT_CODE ON)
    target_include_directories(getopt_dependency PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include/compat)
endif()

if(
    ${PROCESSOR} STREQUAL "aarch64" OR
    ${PROCESSOR} MATCHES "^arm" OR
    ${PROCESSOR} STREQUAL "ppc64" OR
    ${PROCESSOR} MATCHES "^riscv" OR
    ${PROCESSOR} MATCHES "^loongarch"
)
    check_symbol_exists(getauxval "sys/auxv.h" HAVE_GETAUXVAL)
    check_symbol_exists(elf_aux_info "sys/auxv.h" HAVE_ELF_AUX_INFO)
endif()

add_to_configure_file("config.h" "HAVE_GETAUXVAL" TYPE_BOOLEAN)
add_to_configure_file("config.h" "HAVE_ELF_AUX_INFO" TYPE_BOOLEAN)

check_symbol_exists(pthread_getaffinity_np "pthread.h" HAVE_PTHREAD_GETAFFINITY_NP)
check_symbol_exists(pthread_setaffinity_np "pthread.h" HAVE_PTHREAD_SETAFFINITY_NP)
check_symbol_exists(pthread_setname_np "pthread.h" HAVE_PTHREAD_SETNAME_NP)
check_symbol_exists(pthread_set_name_np "pthread.h" HAVE_PTHREAD_SET_NAME_NP)

if(HAVE_PTHREAD_NP_H)
    if(NOT HAVE_PTHREAD_GETAFFINITY_NP)
        check_symbol_exists(pthread_getaffinity_np "pthread_np.h" HAVE_PTHREAD_GETAFFINITY_NP)
    endif()
    if(NOT HAVE_PTHREAD_SETAFFINITY_NP)
        check_symbol_exists(pthread_setaffinity_np "pthread_np.h" HAVE_PTHREAD_SETAFFINITY_NP)
    endif()
    if(NOT HAVE_PTHREAD_SETNAME_NP)
        check_symbol_exists(pthread_setname_np "pthread_np.h" HAVE_PTHREAD_SETNAME_NP)
    endif()
    if(NOT HAVE_PTHREAD_SET_NAME_NP)
        check_symbol_exists(pthread_set_name_np "pthread_np.h" HAVE_PTHREAD_SET_NAME_NP)
    endif()
endif()

add_to_configure_file("config.h" "HAVE_PTHREAD_GETAFFINITY_NP" TYPE_BOOLEAN)
add_to_configure_file("config.h" "HAVE_PTHREAD_SETAFFINITY_NP" TYPE_BOOLEAN)
add_to_configure_file("config.h" "HAVE_PTHREAD_SETNAME_NP" TYPE_BOOLEAN)
add_to_configure_file("config.h" "HAVE_PTHREAD_SET_NAME_NP" TYPE_BOOLEAN)

try_compile(HAVE_C11_GENERIC SOURCE_FROM_CONTENT "generic_test.c" "int x = _Generic(0, default: 0); int main() { return 0; }"
    COMPILE_DEFINITIONS ${TEMP_COMPILE_DEFS}
)

add_to_configure_file("config.h" "HAVE_C11_GENERIC" TYPE_BOOLEAN)

#
# Compiler flag tests
#
include(CheckCCompilerFlag)

if(NOT MSVC)
    check_c_compiler_flag("-fvisibility=hidden" HAS_VISIBILITY_HIDDEN)
    if(HAS_VISIBILITY_HIDDEN)
        list(APPEND TEMP_COMPILE_FLAGS -fvisibility=hidden)
    else()
        message(WARNING "Compiler does not support -fvisibility=hidden, all symbols will be public!")
    endif()
endif()

if(HAVE_ASM)
    check_c_compiler_flag("-fsanitize=memory" HAS_MEMORY_SANITIZER)
    if(HAS_MEMORY_SANITIZER)
        message(FATAL_ERROR "asm causes false positive with memory sanitizer. Use '-Denable_asm=false'.")
    endif()
endif()

# Compiler flags that should be set
# But when the compiler does not supports them
# it is not an error and silently tolerated
set(OPTIONAL_FLAGS "")

if(MSVC)
    list(APPEND OPTIONAL_FLAGS -wd4028) # parameter different from declaration
    list(APPEND OPTIONAL_FLAGS -wd4090) # broken with arrays of pointers
    list(APPEND OPTIONAL_FLAGS -wd4996) # use of POSIX functions
else()
    list(APPEND OPTIONAL_FLAGS
      -Wundef
      -Werror=vla
      -Wno-maybe-uninitialized
      -Wno-missing-field-initializers
      -Wno-unused-parameter
      -Wstrict-prototypes
      -Werror=missing-prototypes
      -Wshorten-64-to-32
    )

    if(${CMAKE_SYSTEM_NAME} MATCHES "^i{d}86$")
        list(APPEND OPTIONAL_FLAGS
            -msse2
            -mfpmath=sse
        )
    endif()
endif()

if(NOT ${CMAKE_BUILD_TYPE} STREQUAL "Debug")
    list(APPEND OPTIONAL_FLAGS
        -fomit-frame-pointer
        -ffast-math
    )
endif()

if((${CMAKE_SYSTEM_NAME} MATCHES "Darwin" OR ${CMAKE_SYSTEM_NAME} MATCHES "iOS" OR ${CMAKE_SYSTEM_NAME} MATCHES "tvOS") AND "${XCODE_VERSION}" MATCHES "^11")
    # Workaround for Xcode 11 -fstack-check bug, see #301
    list(APPEND OPTIONAL_FLAGS -fno-stack-check)
endif()

if(${PROCESSOR} STREQUAL "aarch64" OR ${PROCESSOR} MATCHES "^arm")
    list(APPEND OPTIONAL_FLAGS -fno-align-functions)
endif()


foreach(FLAG IN LISTS OPTIONAL_FLAGS)
    check_c_compiler_flag(${FLAG} _COMPILE_FLAG_${FLAG})
    if(_COMPILE_FLAG_${FLAG})
        list(APPEND TEMP_COMPILE_FLAGS ${FLAG})
    endif()
endforeach()

# TODO libFuzzer related things

#
# Stack alignments flags
#
include(TestBigEndian)
test_big_endian(ENDIANNESS_BIG)

add_to_configure_file("config.h" "ENDIANNESS_BIG" TYPE_BOOLEAN)

if(ARCH_X86)
    if("${stack_alignment}" GREATER 0)
        set(STACK_ALIGNMENT ${stack_alignment})
    elseif(${PROCESSOR} STREQUAL "x86_64" OR ${CMAKE_SYSTEM_NAME} MATCHES "Darwin" OR ${CMAKE_SYSTEM_NAME} MATCHES "Linux" OR ${CMAKE_SYSTEM_NAME} MATCHES "iOS" OR ${CMAKE_SYSTEM_NAME} MATCHES "tvOS")
        set(STACK_ALIGNMENT 16)
    else()
        set(STACK_ALIGNMENT 4)
    endif()
    add_to_configure_file("config.asm" "STACK_ALIGNMENT" TYPE_EXPRESSION)
endif()


#
# ASM specific stuff
#

# TODO gaspp

if(${PROCESSOR} STREQUAL "aarch64")
    set(ARCH_AARCH64 1)
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/gastest.S "
        .text
        MYVAR .req x0
        movi v0.16b, #100
        mov MYVAR, #100
        .unreq MYVAR")
elseif(${PROCESSOR} MATCHES "^arm")
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/gastest.S "
        .text
        .fpu neon
        .arch armv7a
        .object_arch armv4
        .arm
        pld [r0]
        vmovn.u16 d0, q0")
    set(ARCH_ARM 1)
endif()

add_to_configure_file("config.h" "ARCH_AARCH64" TYPE_BOOLEAN)
add_to_configure_file("config.h" "ARCH_ARM" TYPE_BOOLEAN)

if((HAVE_ASM) AND (ARCH_ARM OR ARCH_AARCH64))
    enable_language(ASM)
    message(STATUS ${CMAKE_ASM_SOURCE_FILE_EXTENSIONS})
    separate_arguments(CMAKE_ASM_FLAGS_SEP UNIX_COMMAND "${CMAKE_ASM_FLAGS}")
        execute_process(COMMAND ${CMAKE_ASM_COMPILER} ${CMAKE_ASM_FLAGS_SEP}
            -x assembler-with-cpp -c ${CMAKE_CURRENT_BINARY_DIR}/gastest.S
            RESULT_VARIABLE RESULT OUTPUT_VARIABLE OUTPUT ERROR_VARIABLE ERROR)
    if (RESULT EQUAL 0)
        message(STATUS "Assembler working")
                message(STATUS "Compiler: ${CMAKE_ASM_COMPILER} Output: ${OUTPUT} flags ${CMAKE_ASM_FLAGS_SEP}")
    else()
        message(WARNING "Assembler not working")
        message(STATUS "Output: ${OUTPUT}")
    endif()

    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    try_compile(HAVE_AS_FUNC SOURCE_FROM_CONTENT "test_as.c"
        "__asm__ (
            \".func meson_test\"
            \".endfunc\"
        );
        int main() { return 0; }"
    )

    # fedora package build infrastructure uses a gcc specs file to enable
    # '-fPIE' by default. The chosen way only adds '-fPIE' to the C compiler
    # with integrated preprocessor. It is not added to the standalone
    # preprocessor or the preprocessing stage of '.S' files. So we have to
    # compile code to check if we have to define PIC for the arm asm to
    # avoid absolute relocations when building for example checkasm.
    try_compile(_HAS_PIC SOURCE_FROM_CONTENT "check_pic_code.c"
        "#if defined(PIC)
        #error \"PIC already defined\"
        #elif !(defined(__PIC__) || defined(__pic__))
        #error \"no pic\"
        #endif
        int main() { return 0; }"
    )

    if(_HAS_PIC)
        set(PIC 3)
    endif()

    if (ARCH_AARCH64)
        try_compile(HAVE_AS_ARCH_DIRECTIVE SOURCE_FROM_CONTENT "check_as_arch.c" "__asm__ (\".arch armv8-a\"); int main() { return 0; }")
        if(HAVE_AS_ARCH_DIRECTIVE)
            set(AS_ARCH_LEVEL "armv8-a")
            # Check what .arch levels are supported. In principle, we only
            # want to detect up to armv8.2-a here (binutils requires that
            # in order to enable i8mm). However, older Clang versions
            # (before Clang 17, and Xcode versions up to and including 15.0)
            # didn't support controlling dotprod/i8mm extensions via
            # .arch_extension, therefore try to enable a high enough .arch
            # level as well, to implicitly make them available via that.
            foreach(ARCH IN ITEMS "armv8.2-a" "armv8.4-a" "armv8.6-a")
                try_compile(ARCH_COMPILES SOURCE_FROM_CONTENT "check_as_arch_${ARCH}.c" "__asm__ (\".arch ${ARCH}\"); int main() { return 0; }")
                if (ARCH_COMPILES)
                    set(AS_ARCH_LEVEL ${ARCH})
                endif()
            endforeach()
            # Clang versions before 17 also had a bug
            # (https://github.com/llvm/llvm-project/issues/32220)
            # causing a plain ".arch <level>" to not have any effect unless it
            # had an extra "+<feature>" included - but it was activated on the
            # next ".arch_extension" directive instead. Check if we can include
            # "+crc" as dummy feature to make the .arch directive behave as
            # expected and take effect right away.
            try_compile(ARCH_COMPILES SOURCE_FROM_CONTENT "check_as_arch_crc.c" "__asm__ (\".arch ${AS_ARCH_LEVEL}+crc\"); int main() { return 0; }")
            if (ARCH_COMPILES)
                set(AS_ARCH_LEVEL "${AS_ARCH_LEVEL}+crc")
            endif()
            set(AS_ARCH_STR "\".arch ${AS_ARCH_LEVEL}\\n\"")
        endif()

        # TODO check arch extensions using gaspp

        # Test for support for the various extensions. First test if
        # the assembler supports the .arch_extension directive for
        # enabling/disabling the extension, then separately check whether
        # the instructions themselves are supported. Even if .arch_extension
        # isn't supported, we may be able to assemble the instructions
        # if the .arch level includes support for them.
        foreach(ARCH_ENTENSION IN ITEMS
            "dotprod:udot v0.4s, v0.16b, v0.16b"
            "i8mm:usdot v0.4s, v0.16b, v0.16b"
            "sve:whilelt p0.s, x0, x1"
            "sve2:sqrdmulh z0.s, z0.s, z0.s"
        )
            string(FIND "${ARCH_ENTENSION}" ":" pos)
            if (pos LESS 1)
                message(WARNING "Skipping malformed pair (no var name): ${ARCH_ENTENSION}")
            else ()
                string(SUBSTRING "${ARCH_ENTENSION}" 0 "${pos}" INSTR_NAME)
                math(EXPR pos "${pos} + 1")  # Skip the separator
                string(SUBSTRING "${ARCH_ENTENSION}" "${pos}" -1 INSTR_CODE)
                message("${var}: ${val}")
            endif ()
            string(TOUPPER ${INSTR_NAME} INSTR_NAME_UPPER)

            try_compile(HAVE_AS_ARCHEXT_${INSTR_NAME_UPPER}_DIRECTIVE SOURCE_FROM_CONTENT "check_as_arch_${AS_ARCH_LEVEL}_${INSTR_NAME}.c" "__asm__ (${AS_ARCH_STR}\".arch_extension ${INSTR_NAME}\\n\"); int main() { return 0; }")
            set(CODE "__asm__ (${AS_ARCH_STR}")
            if (HAVE_AS_ARCHEXT_${INSTR_NAME_UPPER}_DIRECTIVE)
                string(APPEND CODE "\".arch_extension ${INSTR_NAME}\\n\"")
            endif()
            string(APPEND CODE "\"${INSTR_CODE}\\n\");")
            string(APPEND CODE "int main() { return 0; }")
            try_compile(HAVE_${INSTR_NAME_UPPER} SOURCE_FROM_CONTENT "check_as_arch_${AS_ARCH_LEVEL}_${INSTR_NAME}_code.c" CODE)
        endforeach()     
    endif()
    unset(CMAKE_TRY_COMPILE_TARGET_TYPE)
endif()

add_to_configure_file("config.h" "HAVE_AS_FUNC" TYPE_BOOLEAN)
add_to_configure_file("config.h" "HAVE_AS_ARCH_DIRECTIVE" TYPE_BOOLEAN)
add_to_configure_file("config.h" "HAVE_AS_ARCHEXT_DOTPROD_DIRECTIVE" TYPE_BOOLEAN)
add_to_configure_file("config.h" "HAVE_AS_ARCHEXT_I8MM_DIRECTIVE" TYPE_BOOLEAN)
add_to_configure_file("config.h" "HAVE_AS_ARCHEXT_SVE_DIRECTIVE" TYPE_BOOLEAN)
add_to_configure_file("config.h" "HAVE_AS_ARCHEXT_SVE2_DIRECTIVE" TYPE_BOOLEAN)
add_to_configure_file("config.h" "HAVE_DOTPROD" TYPE_BOOLEAN)
add_to_configure_file("config.h" "HAVE_I8MM" TYPE_BOOLEAN)
add_to_configure_file("config.h" "HAVE_SVE" TYPE_BOOLEAN)
add_to_configure_file("config.h" "HAVE_SVE2" TYPE_BOOLEAN)

if(ARCH_X86)
    if(${PROCESSOR} STREQUAL "x86_64")
        set(ARCH_X86_64 1)
    else()
        set(ARCH_X86_32 1)
    endif()

    add_to_configure_file("config.asm" "private_prefix" "dav1d" TYPE_EXPRESSION)
    add_to_configure_file("config.asm" "ARCH_X86_64" TYPE_BOOLEAN)
    add_to_configure_file("config.asm" "ARCH_X86_32" TYPE_BOOLEAN)
    add_to_configure_file("config.asm" "PIC" "1" TYPE_BOOLEAN)

    check_symbol_exists("__AVX__" "stdio.h" FORCE_VEX_ENCODING)

    add_to_configure_file("config.asm" "FORCE_VEX_ENCODING" TYPE_BOOLEAN)
endif()

add_to_configure_file("config.h" "ARCH_X86" TYPE_BOOLEAN)
add_to_configure_file("config.h" "ARCH_X86_64" TYPE_BOOLEAN)
add_to_configure_file("config.h" "ARCH_X86_32" TYPE_BOOLEAN)

if(${PROCESSOR} STREQUAL "ppc64")
    set(ARCH_PPC64LE 1)
endif()

add_to_configure_file("config.h" "ARCH_PPC64LE" TYPE_BOOLEAN)

# TODO - The following will always evaluate to false

if(${PROCESSOR} MATCHES "^riscv")
    set(ARCH_RISCV 1)
    if(${PROCESSOR} STREQUAL "riscv64")
        set(ARCH_RV64 1)
    else()
        set(ARCH_RV32 1)
    endif()
endif()

add_to_configure_file("config.h" "ARCH_RISCV" TYPE_BOOLEAN)
add_to_configure_file("config.h" "ARCH_RV32" TYPE_BOOLEAN)
add_to_configure_file("config.h" "ARCH_RV64" TYPE_BOOLEAN)

if(${PROCESSOR} MATCHES "^loongarch")
    set(ARCH_LOONGARCH 1)
    if(${PROCESSOR} STREQUAL "loongarch64")
        set(ARCH_LOONGARCH64 1)
    else()
        set(ARCH_LOONGARCH32 1)
    endif()
endif()

add_to_configure_file("config.h" "ARCH_LOONGARCH" TYPE_BOOLEAN)
add_to_configure_file("config.h" "ARCH_LOONGARCH32" TYPE_BOOLEAN)
add_to_configure_file("config.h" "ARCH_LOONGARCH64" TYPE_BOOLEAN)


# https://mesonbuild.com/Release-notes-for-0-37-0.html#new-compiler-function-symbols_have_underscore_prefix
# For example, Windows 32-bit prefixes underscore, but 64-bit does not.
# Linux does not prefix an underscore but OS X does.
if((WIN32 AND ${PROCESSOR} STREQUAL "i386") OR ${CMAKE_SYSTEM_NAME} MATCHES "Darwin" OR ${CMAKE_SYSTEM_NAME} MATCHES "iOS" OR ${CMAKE_SYSTEM_NAME} MATCHES "tvOS")
    add_to_configure_file("config.h" "PREFIX" "1" TYPE_BOOLEAN)
    add_to_configure_file("config.asm" "PREFIX" "1" TYPE_BOOLEAN)
endif()

#
# ASM specific stuff
#
if(HAVE_ASM AND ARCH_X86)
    # NASM compiler support
    enable_language(ASM_NASM)

    # check NASM version
    execute_process(COMMAND "${CMAKE_ASM_NASM_COMPILER}" "--version" OUTPUT_VARIABLE NASM_VERSION_FULL)

    if("${NASM_VERSION_FULL}" MATCHES "^NASM version")
        string(REPLACE " " ";" NASM_VERSION_FULL "${NASM_VERSION_FULL}")
        list(GET NASM_VERSION_FULL 2 NASM_VERSION)

        if(${NASM_VERSION} VERSION_LESS "2.14")
            message(FATAL_ERROR "nasm 2.14 or later is required, found nasm ${NASM_VERSION}")
        endif()
    else()
        message(FATAL_ERROR "unexpected nasm version string: ${NASM_VERSION_FULL}")
    endif()

    # Generate config.asm
    generate_configuration_file("config.asm" ASM)
endif()

if(HAVE_ASM AND ARCH_RISCV)
    try_compile(HAVE_AS_OPTION SOURCE_FROM_CONTENT "test_as_option.c"
        "__asm__ (
            \".option arch, +v\n\"
            \"vsetivli zero, 0, e8, m1, ta, ma\"
        );
        int main() { return 0; }"
    )
    if(NOT HAVE_AS_OPTION)
        message(FATAL_ERROR "Compiler doesn't support '.option arch' asm directive. Update to binutils>=2.38 or clang>=17 or use '-Denable_asm=false'.")
    endif()
endif()

generate_configuration_file("config.h" C)

# Revision file (vcs_version.h) generation
set(DAV1D_GIT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.git")

find_package(Git)

if(Git_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} --git-dir "${DAV1D_GIT_DIR}" describe --tags --long --match ?.*.* --always
        OUTPUT_VARIABLE VCS_TAG
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
else()
    set(VCS_TAG "unknown")
endif()

if(NOT VCS_TAG)
    set(VCS_TAG "unknown")
endif()

message(STATUS "Git revision: ${VCS_TAG}")
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/include/vcs_version.h.in ${CMAKE_CURRENT_BINARY_DIR}/vcs_version.h)
unset(VCS_TAG)

# libdav1d source files
set(LIBDAV1D_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cdf.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cpu.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ctx.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/data.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/decode.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dequant_tables.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/getbits.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/intra_edge.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/itx_1d.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lf_mask.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lib.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/log.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/mem.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/msac.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/obu.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pal.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/picture.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/qm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ref.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/refmvs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/scan.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tables.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/thread_task.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/warpmv.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/wedge.c
)

# libdav1d bitdepth source files
# These files are compiled for each bitdepth with
# `BITDEPTH` defined to the currently built bitdepth.
set(LIBDAV1D_TMPL_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cdef_apply_tmpl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cdef_tmpl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/fg_apply_tmpl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/filmgrain_tmpl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ipred_prepare_tmpl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ipred_tmpl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/itx_tmpl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lf_apply_tmpl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/loopfilter_tmpl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/looprestoration_tmpl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lr_apply_tmpl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/mc_tmpl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recon_tmpl.c
)

set(LIBDAV1D_ARCH_TMPL_SOURCES "")
set(LIBDAV1D_BITDEPTH_OBJS "")


# ASM specific sources
set(LIBDAV1D_SOURCES_ASM "")
# Arch-specific flags
set(ARCH_SPECIFIC_FLAGS "")

if(HAVE_ASM)
    if(ARCH_ARM OR ARCH_AARCH64)
        list(APPEND LIBDAV1D_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/arm/cpu.c)

        if(ARCH_AARCH64)
            list(APPEND LIBDAV1D_SOURCES
                # itx.S is used for both 8 and 16 bpc.
                ${CMAKE_CURRENT_SOURCE_DIR}/src/arm/64/itx.S
                ${CMAKE_CURRENT_SOURCE_DIR}/src/arm/64/looprestoration_common.S
                ${CMAKE_CURRENT_SOURCE_DIR}/src/arm/64/msac.S
                ${CMAKE_CURRENT_SOURCE_DIR}/src/arm/64/refmvs.S
            )

            if(CONFIG_8BPC)
                list(APPEND LIBDAV1D_SOURCES
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/arm/64/cdef.S
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/arm/64/filmgrain.S
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/arm/64/ipred.S
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/arm/64/loopfilter.S
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/arm/64/looprestoration.S
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/arm/64/mc.S
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/arm/64/mc_dotprod.S
                )
            endif()

            if(CONFIG_16BPC)
                list(APPEND LIBDAVID_SOURCES
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/arm/64/cdef16.S
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/arm/64/filmgrain16.S
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/arm/64/ipred16.S
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/arm/64/itx16.S
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/arm/64/loopfilter16.S
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/arm/64/looprestoration16.S
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/arm/64/mc16.S
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/arm/64/mc16_sve.S
                )
            endif()
        else()
            list(APPEND LIBDAV1D_SOURCES
                # itx.S is used for both 8 and 16 bpc.
                ${CMAKE_CURRENT_SOURCE_DIR}/src/arm/32/itx.S
                ${CMAKE_CURRENT_SOURCE_DIR}/src/arm/32/looprestoration_common.S
                ${CMAKE_CURRENT_SOURCE_DIR}/src/arm/32/msac.S
                ${CMAKE_CURRENT_SOURCE_DIR}/src/arm/32/refmvs.S
            )

            if(CONFIG_8BPC)
                list(APPEND LIBDAV1D_SOURCES
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/arm/32/cdef.S
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/arm/32/filmgrain.S
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/arm/32/ipred.S
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/arm/32/loopfilter.S
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/arm/32/looprestoration.S
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/arm/32/mc.S
                )
            endif()

            if(CONFIG_16BPC)
                list(APPEND LIBDAVID_SOURCES
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/arm/32/cdef16.S
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/arm/32/filmgrain16.S
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/arm/32/ipred16.S
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/arm/32/itx16.S
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/arm/32/loopfilter16.S
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/arm/32/looprestoration16.S
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/arm/32/mc16.S
                )
            endif()
        endif()
    elseif(ARCH_X86)
        list(APPEND LIBDAV1D_SOURCES
            ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/cpu.c
        )

        # NASM source files
        list(APPEND LIBDAV1D_SOURCES_ASM
            ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/cpuid.asm
            ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/msac.asm
            ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/pal.asm
            ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/refmvs.asm
            ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/itx_avx512.asm
            ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/cdef_avx2.asm
            ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/itx_avx2.asm
            ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/cdef_sse.asm
            ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/itx_sse.asm
        )

        if(CONFIG_8BPC)
            list(APPEND LIBDAV1D_SOURCES_ASM
                ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/cdef_avx512.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/filmgrain_avx512.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/ipred_avx512.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/loopfilter_avx512.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/looprestoration_avx512.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/mc_avx512.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/filmgrain_avx2.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/ipred_avx2.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/loopfilter_avx2.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/looprestoration_avx2.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/mc_avx2.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/filmgrain_sse.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/ipred_sse.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/loopfilter_sse.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/looprestoration_sse.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/mc_sse.asm
            )
        endif()

        if(CONFIG_16BPC)
            list(APPEND LIBDAV1D_SOURCES_ASM
                ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/cdef16_avx512.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/filmgrain16_avx512.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/ipred16_avx512.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/itx16_avx512.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/loopfilter16_avx512.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/looprestoration16_avx512.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/mc16_avx512.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/cdef16_avx2.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/filmgrain16_avx2.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/ipred16_avx2.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/itx16_avx2.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/loopfilter16_avx2.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/looprestoration16_avx2.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/mc16_avx2.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/cdef16_sse.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/filmgrain16_sse.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/ipred16_sse.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/itx16_sse.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/loopfilter16_sse.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/looprestoration16_sse.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/src/x86/mc16_sse.asm
            )
        endif()
    elseif(ARCH_PPC64LE)
        list(APPEND ARCH_SPECIFIC_FLAGS -maltivec -mvsx -DDAV1D_VSX)
        list(APPEND LIBDAV1D_SOURCES
            ${CMAKE_CURRENT_SOURCE_DIR}/src/ppc/cpu.c
        )
        list(APPEND LIBDAV1D_ARCH_TMPL_SOURCES
            ${CMAKE_CURRENT_SOURCE_DIR}/src/ppc/cdef_tmpl.c
            ${CMAKE_CURRENT_SOURCE_DIR}/src/ppc/looprestoration_tmpl.c
        )
    elseif(ARCH_LOONGARCH)
        list(APPEND LIBDAV1D_SOURCES
            ${CMAKE_CURRENT_SOURCE_DIR}/src/loongarch/cpu.c
        )
        list(APPEND LIBDAV1D_ARCH_TMPL_SOURCES
            ${CMAKE_CURRENT_SOURCE_DIR}/src/loongarch/looprestoration_tmpl.c
        )
        list(APPEND LIBDAV1D_SOURCES
            ${CMAKE_CURRENT_SOURCE_DIR}/src/loongarch/cdef.S
            ${CMAKE_CURRENT_SOURCE_DIR}/src/loongarch/ipred.S
            ${CMAKE_CURRENT_SOURCE_DIR}/src/loongarch/mc.S
            ${CMAKE_CURRENT_SOURCE_DIR}/src/loongarch/loopfilter.S
            ${CMAKE_CURRENT_SOURCE_DIR}/src/loongarch/looprestoration.S
            ${CMAKE_CURRENT_SOURCE_DIR}/src/loongarch/msac.S
            ${CMAKE_CURRENT_SOURCE_DIR}/src/loongarch/refmvs.S
            ${CMAKE_CURRENT_SOURCE_DIR}/src/loongarch/itx.S
        )
    elseif(ARCH_RISCV)
        list(APPEND LIBDAV1D_SOURCES
            ${CMAKE_CURRENT_SOURCE_DIR}/src/riscv/cpu.c
        )
        if(ARCH_RV64)
            list(APPEND LIBDAV1D_SOURCES
                ${CMAKE_CURRENT_SOURCE_DIR}/src/riscv/64/cpu.S
                ${CMAKE_CURRENT_SOURCE_DIR}/src/riscv/64/pal.S
            )

            if(CONFIG_8BPC)
                list(APPEND LIBDAV1D_SOURCES
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/riscv/64/cdef.S
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/riscv/64/ipred.S
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/riscv/64/itx.S
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/riscv/64/mc.S
                )
            endif()

            if(CONFIG_16BPC)
                list(APPEND LIBDAVID_SOURCES
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/riscv/64/cdef16.S
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/riscv/64/ipred16.S
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/riscv/64/mc16.S
                )
            endif()
        endif()
    endif()
endif()

#
# Windows .rc file and API export flags
#
set(API_EXPORT_DEFS "")

if(WIN32)
    if(BUILD_SHARED_LIBS)
        set(PROJECT_VERSION_MAJOR "${DAVID_VERSION_MAJOR}")
        set(PROJECT_VERSION_MINOR "${DAVID_VERSION_MINOR}")
        set(PROJECT_VERSION_REVISION "${DAVID_VERSION_PATCH}")
        set(API_VERSION_MAJOR "${DAV1D_API_VERSION_MAJOR}")
        set(API_VERSION_MINOR "${DAV1D_API_VERSION_MINOR}")
        set(API_VERSION_REVISION "${DAV1D_API_VERSION_PATCH}")
        set(COPYRIGHT_YEARS "2018-2025")
        configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/dav1d.rc.in ${CMAKE_CURRENT_BINARY_DIR}/dav1d.rc)
        list(APPEND LIBDAV1D_SOURCES ${CMAKE_CURRENT_BINARY_DIR}/dav1d.rc)
        list(APPEND API_EXPORT_DEFS DAV1D_BUILDING_DLL)
    endif()

    if(ARCH_X86_64 AND MINGW)
        set(LIBDAV1D_FLAGS "-mcmodel=small")
    endif()
endif()

#
# Library definitions
#

set(LIBDAV1D_INCLUDE_DIRS_PRIV
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include/dav1d
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/include/dav1d
)
set(LIBDAV1D_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/dav1d
    ${CMAKE_CURRENT_BINARY_DIR}/include/dav1d
)

# The final dav1d library
add_library(dav1d ${TEMP_COMPAT_FILES} ${LIBDAV1D_SOURCES} ${LIBDAV1D_SOURCES_ASM})
target_include_directories(dav1d PRIVATE ${LIBDAV1D_INCLUDE_DIRS_PRIV})
target_include_directories(dav1d PUBLIC ${LIBDAV1D_INCLUDE_DIRS})
target_compile_definitions(dav1d PRIVATE ${TEMP_COMPILE_DEFS} ${API_EXPORT_DEFS})

add_library(dav1d::dav1d ALIAS dav1d)

if(NOT WIN32)
    set_target_properties(dav1d PROPERTIES SOVERSION "${DAV1D_API_VERSION_MAJOR}")
else()
    set_target_properties(dav1d PROPERTIES PREFIX "lib")
endif()

if (NOT ("x${CMAKE_DL_LIBS}" STREQUAL "x"))
    target_link_libraries(dav1d ${CMAKE_DL_LIBS})
endif()

if(NOT HAS_STDATOMIC)
    target_link_libraries(dav1d stdatomic_dependency)
endif()

# Helper library for each bitdepth (and architecture-specific flags)
foreach(BITS IN LISTS bitdepths)
    add_library(dav1d_bitdepth_${BITS} STATIC ${LIBDAV1D_TMPL_SOURCES})
    set_property(TARGET dav1d_bitdepth_${BITS} PROPERTY POSITION_INDEPENDENT_CODE ON)
    target_include_directories(dav1d_bitdepth_${BITS} PRIVATE ${LIBDAV1D_INCLUDE_DIRS_PRIV})
    target_include_directories(dav1d_bitdepth_${BITS} PUBLIC ${LIBDAV1D_INCLUDE_DIRS})
    target_compile_options(dav1d_bitdepth_${BITS} PRIVATE ${TEMP_COMPILE_FLAGS})
    target_compile_definitions(dav1d_bitdepth_${BITS} PRIVATE ${TEMP_COMPILE_DEFS} BITDEPTH=${BITS})
    target_link_libraries(dav1d_bitdepth_${BITS} dav1d)
    target_link_libraries(dav1d dav1d_bitdepth_${BITS})

    if(NOT HAS_STDATOMIC)
        target_link_libraries(dav1d_bitdepth_${BITS} stdatomic_dependency)
    endif()

    if(LIBDAV1D_ARCH_TMPL_SOURCES)
        add_library(dav1d_arch_bitdepth_${BITS} STATIC ${LIBDAV1D_ARCH_TMPL_SOURCES})
        set_property(TARGET dav1d_arch_bitdepth_${BITS} PROPERTY POSITION_INDEPENDENT_CODE ON)
        target_include_directories(dav1d_arch_bitdepth_${BITS} PRIVATE ${LIBDAV1D_INCLUDE_DIRS_PRIV})
        target_include_directories(dav1d_arch_bitdepth_${BITS} PUBLIC ${LIBDAV1D_INCLUDE_DIRS})
        target_compile_options(dav1d_arch_bitdepth_${BITS} PRIVATE ${TEMP_COMPILE_FLAGS} ${ARCH_SPECIFIC_FLAGS})
        target_compile_definitions(dav1d_arch_bitdepth_${BITS} PRIVATE ${TEMP_COMPILE_DEFS} BITDEPTH=${BITS})
        target_link_libraries(dav1d_arch_bitdepth_${BITS} dav1d)
        target_link_libraries(dav1d dav1d_arch_bitdepth_${BITS})

        if(NOT HAS_STDATOMIC)
            target_link_libraries(dav1d_arch_bitdepth_${BITS} stdatomic_dependency)
        endif()
    endif()
endforeach()

if(TEMP_LINK_LIBRARIES)
    target_link_directories(dav1d PUBLIC ${TEMP_LINK_LIBRARIES})
endif()

if(THREAD_DEPENDENCY)
    target_link_libraries(dav1d ${THREAD_DEPENDENCY})
endif()

#if(enable_tools)
#    file(GENERATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/cli_config.h CONTENT " ")

#    add_executable(dav1d_exe
#        dav1d/tools/input/input.c
#        dav1d/tools/input/annexb.c
#        dav1d/tools/input/ivf.c
#        dav1d/tools/input/section5.c
#        dav1d/tools/output/md5.c
#        dav1d/tools/output/null.c
#        dav1d/tools/output/output.c
#        dav1d/tools/output/y4m2.c
#        dav1d/tools/output/yuv.c
#        dav1d/tools/dav1d.c
#        dav1d/tools/dav1d_cli_parse.c
#    )
#    target_include_directories(dav1d_exe PRIVATE
#        ${LIBDAV1D_INCLUDE_DIRS_PRIV}
#        ${LIBDAV1D_INCLUDE_DIRS}
#        ${CMAKE_CURRENT_SOURCE_DIR}/dav1d/tools
#    )
#    target_link_libraries(dav1d_exe dav1d)
#    set_target_properties(dav1d_exe PROPERTIES OUTPUT_NAME "dav1d")

#    if(NOT HAS_GETOPT_LONG)
#        target_link_libraries(dav1d_exe getopt_dependency)
#    endif()

#    if(THREAD_DEPENDENCY)
#        target_link_libraries(dav1d ${THREAD_DEPENDENCY})
#    endif()
#endif()

# TODO tests
