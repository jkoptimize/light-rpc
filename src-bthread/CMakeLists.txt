# bthread M:N coroutine library, ported from brpc.
# Task2 stage: only butil base modules; bthread core sources are appended in
# later tasks.

set(BTHREAD_SOURCES
    butil/time.cpp
    butil/fast_rand.cpp
    butil/thread_local.cpp
    butil/thread_key.cpp
    butil/third_party/murmurhash3/murmurhash3.cpp
)

add_library(fast_bthread STATIC ${BTHREAD_SOURCES})

target_include_directories(fast_bthread PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}  # for #include "butil/xxx.h"
)

target_link_libraries(fast_bthread PUBLIC pthread)

# Expose clock_gettime/gettimeofday (POSIX.1b / default source) to all
# consumers of these headers.
target_compile_definitions(fast_bthread PUBLIC _GNU_SOURCE)
