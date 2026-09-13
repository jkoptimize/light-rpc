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
// Ported from brpc butil/thread_local_inl.h.

#ifndef FAST_BUTIL_THREAD_LOCAL_INL_H
#define FAST_BUTIL_THREAD_LOCAL_INL_H

namespace fast {
namespace butil {

namespace detail {

template <typename T>
class ThreadLocalHelper {
public:
    inline static T* get() {
        if (__builtin_expect(value != NULL, 1)) {
            return value;
        }
        value = new (std::nothrow) T;
        if (value != NULL) {
            thread_atexit(delete_object<T>, value);
        }
        return value;
    }
    static BAIDU_THREAD_LOCAL T* value;
};

template <typename T> BAIDU_THREAD_LOCAL T* ThreadLocalHelper<T>::value = NULL;

}  // namespace detail

template <typename T> inline T* get_thread_local() {
    return detail::ThreadLocalHelper<T>::get();
}

}  // namespace butil
}  // namespace fast

#endif  // FAST_BUTIL_THREAD_LOCAL_INL_H
