cmake_minimum_required(VERSION 3.26...4.0)

set(PROJECT_NAME "easyav1")
project(${PROJECT_NAME} C)

set(CMAKE_C_STANDARD 99)

set(BUILD_SHARED_LIBS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

#IF(WIN32)
#	ADD_DEFINITIONS(-D_CRT_SECURE_NO_WARNINGS)
#ENDIF()

set(CMAKE_MODULE_PATH "${CMAKE_MODULE_PATH}" "${CMAKE_SOURCE_DIR}/cmake/")

add_subdirectory("ext/dav1d")

SET(SOURCE_FILES
	"src/easyav1.c"
)

SET(HEADER_FILES
	"src/easyav1.h"
)

SET(NESTEGG_FILES
	"ext/nestegg/nestegg.c"
)

find_package(SDL2)

add_library(${PROJECT_NAME} ${HEADER_FILES} ${SOURCE_FILES} ${NESTEGG_FILES})
target_include_directories(${PROJECT_NAME} PUBLIC
	"src"
	"ext"
)

target_link_libraries(${PROJECT_NAME} dav1d)
find_library(MATH_LIBRARY m)
if(NOT APPLE AND MATH_LIBRARY)
    target_link_libraries(${PROJECT_NAME} ${MATH_LIBRARY})
endif()

add_executable("${PROJECT_NAME}_player" "src/main.c")
target_include_directories(${PROJECT_NAME} PUBLIC
	"src"
	"ext"
	${SDL2_INCLUDE_DIR}
)
target_link_libraries("${PROJECT_NAME}_player" "${PROJECT_NAME}" ${SDL2_LIBRARY})
