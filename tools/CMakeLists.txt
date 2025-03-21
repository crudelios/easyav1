cmake_minimum_required(VERSION 3.26...4.0)

set(CMAKE_C_STANDARD 99)

find_package(SDL3)

if (NOT SDL3_FOUND)
	message(WARNING "SDL3 not found, ${PROJECT_NAME}_player will not be built")
else()
	add_executable("${PROJECT_NAME}_player" "easyav1_player.c")
	target_include_directories(${PROJECT_NAME} PUBLIC
		"src"
		${SDL3_INCLUDE_DIR}
	)
	target_link_libraries("${PROJECT_NAME}_player" "${PROJECT_NAME}" ${SDL3_LIBRARY})
endif()

add_executable("${PROJECT_NAME}_benchmark" "easyav1_benchmark.c")
target_include_directories(${PROJECT_NAME} PUBLIC
	"src"
)
target_link_libraries("${PROJECT_NAME}_benchmark" "${PROJECT_NAME}")
