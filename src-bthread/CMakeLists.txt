# bthread M:N coroutine library, ported from brpc.
# Include the scheduler/timer/mutex dependency closure required by butex.

set(BTHREAD_SOURCES
    bthread.cpp
    bthread_once.cpp
    butex.cpp
    condition_variable.cpp
    context.cpp
    id.cpp
    errno.cpp
    interrupt_pthread.cpp
    key.cpp
    mutex.cpp
    rwlock.cpp
    semaphore.cpp
    stack.cpp
    sys_futex.cpp
    task_control.cpp
    task_group.cpp
    timer_thread.cpp
    butil/time.cpp
    butil/fast_rand.cpp
    butil/thread_local.cpp
    butil/thread_key.cpp
    butil/class_name.cpp
    butil/string_printf.cpp
    butil/third_party/murmurhash3/murmurhash3.cpp
)

add_library(fast_bthread STATIC ${BTHREAD_SOURCES})

# Only quoted project headers should search this flattened directory.
# Using -I here would make src-bthread/errno.h shadow the system <errno.h>,
# including when it is included indirectly through <cerrno> or <mutex>.
target_compile_options(fast_bthread PUBLIC
    "-iquote${CMAKE_CURRENT_SOURCE_DIR}"  # for #include "butil/xxx.h"
)

target_link_libraries(fast_bthread PUBLIC pthread)

# Expose clock_gettime/gettimeofday (POSIX.1b / default source) to all
# consumers of these headers.
target_compile_definitions(fast_bthread PUBLIC _GNU_SOURCE)
