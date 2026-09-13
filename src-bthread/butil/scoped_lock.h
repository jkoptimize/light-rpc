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

// Ported from brpc butil/scoped_lock.h, trimmed to the C++11 BAIDU_SCOPED_LOCK
// (decltype deduction) plus the std::lock_guard<pthread_mutex_t>
// specialization. This lets BAIDU_SCOPED_LOCK work with both std::mutex and
// the statically-initialized raw pthread_mutex_t used by resource_pool /
// object_pool. Removed: the pre-C++11 std::lock_guard/unique_lock fallbacks,
// pthread_spinlock_t specializations, butil::double_lock.

#ifndef FAST_BUTIL_SCOPED_LOCK_H
#define FAST_BUTIL_SCOPED_LOCK_H

#include <pthread.h>   // pthread_mutex_t
#include <mutex>       // std::lock_guard
#include <type_traits> // std::remove_reference

#include "macros.h"    // BAIDU_CONCAT, DISALLOW_COPY_AND_ASSIGN

namespace fast {
namespace butil {
namespace detail {

// NOTE: C++11 deduces an additional reference to the type, so strip it.
template <typename T>
std::lock_guard<typename std::remove_reference<T>::type> get_lock_guard();

}  // namespace detail
}  // namespace butil
}  // namespace fast

#define BAIDU_SCOPED_LOCK(ref_of_lock)                                     \
    decltype(::fast::butil::detail::get_lock_guard<decltype(ref_of_lock)>()) \
    BAIDU_CONCAT(scoped_locker_dummy_at_line_, __LINE__)(ref_of_lock)

namespace std {

// Specialization so that BAIDU_SCOPED_LOCK works on raw pthread_mutex_t, which
// resource_pool / object_pool use for constant (PTHREAD_MUTEX_INITIALIZER)
// initialization of their static members.
template<> class lock_guard<pthread_mutex_t> {
public:
    explicit lock_guard(pthread_mutex_t& mutex) : _pmutex(&mutex) {
        pthread_mutex_lock(_pmutex);
    }
    ~lock_guard() {
        pthread_mutex_unlock(_pmutex);
    }
private:
    DISALLOW_COPY_AND_ASSIGN(lock_guard);
    pthread_mutex_t* _pmutex;
};

}  // namespace std

#endif  // FAST_BUTIL_SCOPED_LOCK_H
