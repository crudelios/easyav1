cmake_minimum_required(VERSION 3.26...4.0)

set(CMAKE_C_STANDARD 99)

add_executable("${PROJECT_NAME}_player" "easyav1_player.c")
target_include_directories(${PROJECT_NAME} PUBLIC
	"src"
	${SDL2_INCLUDE_DIR}
)
target_link_libraries("${PROJECT_NAME}_player" "${PROJECT_NAME}" ${SDL2_LIBRARY})

add_executable("${PROJECT_NAME}_benchmark" "easyav1_benchmark.c")
target_include_directories(${PROJECT_NAME} PUBLIC
	"src"
)
target_link_libraries("${PROJECT_NAME}_benchmark" "${PROJECT_NAME}")
