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

#include <atomic>
#include "butex.h"    // butex_*
#include "types.h"    // bthread_sem_t

namespace fast {

static inline int bthread_sem_trywait(bthread_sem_t* sema) {
    auto whole = (std::atomic<unsigned>*)sema->butex;
    while (true) {
        unsigned num = whole->load(std::memory_order_relaxed);
        if (num == 0) {
            return EAGAIN;
        }
        if (whole->compare_exchange_weak(num, num - 1,
                                         std::memory_order_acquire,
                                         std::memory_order_relaxed)) {
            return 0;
        }
    }
}

static int bthread_sem_wait_impl(bthread_sem_t* sem, const struct timespec* abstime) {
    bool queue_lifo = false;
    bool first_wait = true;
    auto whole = (std::atomic<unsigned>*)sem->butex;
    while (true) {
        unsigned num = whole->load(std::memory_order_relaxed);
        if (num > 0) {
            if (whole->compare_exchange_weak(num, num - 1,
                                             std::memory_order_acquire,
                                             std::memory_order_relaxed)) {
                return 0;
            }
        }
        if (butex_wait(sem->butex, 0, abstime, queue_lifo) < 0 &&
            errno != EWOULDBLOCK && errno != EINTR) {
            // A semaphore should ignore interruptions in general since
            // user code is unlikely to check the return value.
            return errno;
        }
        // Ignore EWOULDBLOCK and EINTR.
        if (first_wait && 0 == errno) {
            first_wait = false;
        }
        if (!first_wait) {
            // Normally, bthreads are queued in FIFO order. But competing with new
            // arriving bthreads over sema, a woken up bthread has good chances of
            // losing. Because new arriving bthreads are already running on CPU and
            // there can be lots of them. In such case, for fairness, to avoid
            // starvation, it is queued at the head of the waiter queue.
            queue_lifo = true;
        }
    }
}

static inline int bthread_sem_post(bthread_sem_t* sem, size_t num) {
    if (num > 0) {
        unsigned n = ((std::atomic<unsigned>*)sem->butex)
            ->fetch_add(num, std::memory_order_relaxed);
        butex_wake_n(sem->butex, n);
    }
    return 0;
}

} // namespace fast

extern "C" {

int bthread_sem_init(bthread_sem_t* sem, unsigned value) {
    sem->butex = fast::butex_create_checked<unsigned>();
    if (!sem->butex) {
        return ENOMEM;
    }
    *sem->butex = value;
    return 0;
}

int bthread_sem_destroy(bthread_sem_t* semaphore) {
    fast::butex_destroy(semaphore->butex);
    return 0;
}

int bthread_sem_trywait(bthread_sem_t* sem) {
    return fast::bthread_sem_trywait(sem);
}

int bthread_sem_wait(bthread_sem_t* sem) {
    return fast::bthread_sem_wait_impl(sem, NULL);
}

int bthread_sem_timedwait(bthread_sem_t* sem, const struct timespec* abstime) {
    return fast::bthread_sem_wait_impl(sem, abstime);
}

int bthread_sem_post(bthread_sem_t* sem) {
    return fast::bthread_sem_post(sem, 1);
}

int bthread_sem_post_n(bthread_sem_t* sem, size_t n) {
    return fast::bthread_sem_post(sem, n);
}

} // extern "C"
