// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

// bthread - An M:N threading library to make applications more concurrent.

// Date: Sun Aug  3 12:46:15 CST 2014

#include <atomic>
#include "inc/fast_log.h"
#include "butex.h"
#include "mutex.h"
#include "processor.h"
#include "task_group.h"

// brpc's non-profiling mutex paths. Contention sampling, pthread interposition
// and debug-owner tracking are intentionally outside the light-rpc port.
namespace fast {
extern BAIDU_THREAD_LOCAL TaskGroup* tls_task_group;
// Same as brpc with BRPC_DEBUG_BTHREAD_SCHE_SAFETY disabled.
void CheckBthreadScheSafety() {}
// Implement bthread_mutex_t related functions
struct MutexInternal {
    std::atomic<unsigned char> locked;
    std::atomic<unsigned char> contended;
    unsigned short padding;
};

const MutexInternal MUTEX_CONTENDED_RAW = {{1},{1},0};
const MutexInternal MUTEX_LOCKED_RAW = {{1},{0},0};
// Define as macros rather than constants which can't be put in read-only
// section and affected by initialization-order fiasco.
#define BTHREAD_MUTEX_CONTENDED (*(const unsigned*)&fast::MUTEX_CONTENDED_RAW)
#define BTHREAD_MUTEX_LOCKED (*(const unsigned*)&fast::MUTEX_LOCKED_RAW)

BAIDU_CASSERT(sizeof(unsigned) == sizeof(MutexInternal),
              sizeof_mutex_internal_must_equal_unsigned);

inline int mutex_trylock_impl(bthread_mutex_t* m) {
    MutexInternal* split = (MutexInternal*)m->butex;
    if (!split->locked.exchange(1, std::memory_order_acquire)) {
            return 0;
    }
    return EBUSY;
}

const int MAX_SPIN_ITER = 4;

inline int mutex_lock_contended_impl(bthread_mutex_t* __restrict m,
                                     const struct timespec* __restrict abstime) {
    // When a bthread first contends for a lock, active spinning makes sense.
    // Spin only few times and only if local `rq' is empty.
    TaskGroup* g = BAIDU_GET_VOLATILE_THREAD_LOCAL(tls_task_group);
    if (BAIDU_UNLIKELY(NULL == g || g->rq_size() == 0)) {
        for (int i = 0; i < MAX_SPIN_ITER; ++i) {
            cpu_relax();
        }
    }

    bool queue_lifo = false;
    bool first_wait = true;
    auto whole = (std::atomic<unsigned>*)m->butex;
    while (whole->exchange(BTHREAD_MUTEX_CONTENDED) & BTHREAD_MUTEX_LOCKED) {
        if (fast::butex_wait(whole, BTHREAD_MUTEX_CONTENDED, abstime, queue_lifo) < 0 &&
            errno != EWOULDBLOCK && errno != EINTR/*note*/) {
            // A mutex lock should ignore interruptions in general since
            // user code is unlikely to check the return value.
            return errno;
        }
        // Ignore EWOULDBLOCK and EINTR.
        if (first_wait && 0 == errno) {
            first_wait = false;
        }
        if (!first_wait) {
            // Normally, bthreads are queued in FIFO order. But competing with new
            // arriving bthreads over the ownership of mutex, a woken up bthread
            // has good chances of losing. Because new arriving bthreads are already
            // running on CPU and there can be lots of them. In such case, for fairness,
            // to avoid starvation, it is queued at the head of the waiter queue.
            queue_lifo = true;
        }
    }
    return 0;
}

void FastPthreadMutex::lock() { _mutex.lock(); }
void FastPthreadMutex::unlock() { _mutex.unlock(); }
} // namespace fast

extern "C" {
int bthread_mutex_init(bthread_mutex_t* m, const bthread_mutexattr_t*) {
    m->butex = fast::butex_create_checked<unsigned>();
    if (!m->butex) return ENOMEM;
    *m->butex = 0;
    return 0;
}
int bthread_mutex_destroy(bthread_mutex_t* m) {
    fast::butex_destroy(m->butex);
    return 0;
}
int bthread_mutex_trylock(bthread_mutex_t* m) {
    return fast::mutex_trylock_impl(m);
}
int bthread_mutex_lock_contended(bthread_mutex_t* m) {
    return fast::mutex_lock_contended_impl(m, nullptr);
}
int bthread_mutex_lock(bthread_mutex_t* m) {
    if (fast::mutex_trylock_impl(m) == 0) return 0;
    return fast::mutex_lock_contended_impl(m, nullptr);
}
int bthread_mutex_timedlock(bthread_mutex_t* m, const timespec* abstime) {
    if (fast::mutex_trylock_impl(m) == 0) return 0;
    return fast::mutex_lock_contended_impl(m, abstime);
}
int bthread_mutex_unlock(bthread_mutex_t* m) {
    auto whole = reinterpret_cast<std::atomic<unsigned>*>(m->butex);
    const unsigned prev = whole->exchange(0, std::memory_order_release);
    if (prev != BTHREAD_MUTEX_LOCKED) fast::butex_wake(whole);
    return 0;
}
int bthread_mutexattr_init(bthread_mutexattr_t*) { return 0; }
int bthread_mutexattr_destroy(bthread_mutexattr_t*) { return 0; }
} // extern "C"
