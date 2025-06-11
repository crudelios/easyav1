cmake_minimum_required(VERSION 3.26...4.0)

option(EASYAV1_USE_EXTERNAL_DAV1D_LIBRARY "Use external library for dav1d" OFF)
option(EASYAV1_BUILD_TOOLS "Build the executable tools that use the library" ON)

set(CMAKE_MODULE_PATH "${CMAKE_MODULE_PATH}" "${CMAKE_SOURCE_DIR}/cmake/")

if (NOT EASYAV1_USE_EXTERNAL_DAV1D_LIBRARY)
	include("dav1d")
	set(BUILD_SHARED_LIBS OFF)
endif()

project("easyav1" C)

if(EASYAV1_USE_EXTERNAL_DAV1D_LIBRARY)
	find_package(dav1d REQUIRED)
	add_compile_definitions(EASYAV1_USE_EXTERNAL_DAV1D_LIBRARY)
endif()

set(CMAKE_C_STANDARD 99)


set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

#IF(WIN32)
#	ADD_DEFINITIONS(-D_CRT_SECURE_NO_WARNINGS)
#ENDIF()


SET(SOURCE_FILES
	"src/easyav1.c"
)

SET(HEADER_FILES
	"src/easyav1.h"
)

SET(NESTEGG_FILES
	"ext/nestegg/nestegg.c"
)

if(NOT WIN32)
	set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
	set(THREADS_DEPENDENCY Threads::Threads)

	if (CMAKE_BUILD_TYPE STREQUAL "Debug")
		add_compile_options("-fsanitize=thread,undefined")
		add_compile_options("-g3")
		add_link_options("-fsanitize=thread,undefined")
		add_link_options("-g3")
	endif()
endif()

add_library(${PROJECT_NAME} ${HEADER_FILES} ${SOURCE_FILES} ${NESTEGG_FILES})
target_include_directories(${PROJECT_NAME} PUBLIC
	"src"
	"ext"
)

target_link_libraries(${PROJECT_NAME} dav1d ${THREADS_DEPENDENCY})

find_library(MATH_LIBRARY m)
if(NOT APPLE AND MATH_LIBRARY)
    target_link_libraries(${PROJECT_NAME} ${MATH_LIBRARY})
endif()

if (EASYAV1_BUILD_TOOLS)
	add_subdirectory("tools")
endif()
