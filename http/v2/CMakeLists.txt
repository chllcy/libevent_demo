cmake_minimum_required(VERSION 3.5.1)

PROJECT(samplest)
set(CMAKE_CXX_STANDARD       11)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

if (CMAKE_BUILD_TYPE STREQUAL Debug)
add_compile_options(
    -g2 -ggdb -O0 -fno-omit-frame-pointer
)

endif()

INCLUDE_DIRECTORIES(
/usr/local/include
)
include_directories(.)
link_directories(/usr/local/lib/)

add_compile_options(
    -pthread
    -fpermissive
    -g2 -ggdb -O0 -fno-omit-frame-pointer
)

add_definitions(
    -DLINUX
)

AUX_SOURCE_DIRECTORY(. DIR_SRCS)

SET(TEST_MATH
${DIR_SRCS}
)

ADD_EXECUTABLE(${PROJECT_NAME} ${TEST_MATH})
target_link_libraries(${PROJECT_NAME} PRIVATE event event_extra event_pthreads -lpthread -ldl)

