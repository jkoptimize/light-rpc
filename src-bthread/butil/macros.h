// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Ported from brpc butil/macros.h + butil/compiler_specific.h, trimmed to the
// macros actually used by the bthread core. compiler_specific.h is NOT ported
// as a standalone file; its compiler-attribute macros are absorbed here.

#ifndef FAST_BUTIL_MACROS_H_
#define FAST_BUTIL_MACROS_H_

#include <stddef.h>  // size_t
#include <stdlib.h>  // abort
#include <new>       // std::nothrow

#include "build_config.h"

// ---------------------------------------------------------------------------
// Compiler attributes (absorbed from butil/compiler_specific.h)
// build_config.h guarantees COMPILER_GCC, so the non-GCC fallbacks are dropped.
// ---------------------------------------------------------------------------

#define ALLOW_UNUSED __attribute__((unused))

#define ALIGNAS(byte_alignment) __attribute__((aligned(byte_alignment)))

#define BUTIL_FORCE_INLINE inline __attribute__((always_inline))

#define WARN_UNUSED_RESULT __attribute__((warn_unused_result))

#define BAIDU_WEAK __attribute__((weak))

#define BAIDU_LIKELY(expr) (__builtin_expect((bool)(expr), true))
#define BAIDU_UNLIKELY(expr) (__builtin_expect((bool)(expr), false))

#define BAIDU_CACHELINE_SIZE 64
#define BAIDU_CACHELINE_ALIGNMENT ALIGNAS(BAIDU_CACHELINE_SIZE)

// ---------------------------------------------------------------------------
// Copy/move control
// ---------------------------------------------------------------------------

#define BUTIL_DELETE_FUNCTION(decl) decl = delete

#define DISALLOW_COPY(TypeName) \
    BUTIL_DELETE_FUNCTION(TypeName(const TypeName&))

#define DISALLOW_ASSIGN(TypeName) \
    BUTIL_DELETE_FUNCTION(TypeName& operator=(const TypeName&))

#define DISALLOW_COPY_AND_ASSIGN(TypeName) \
    DISALLOW_COPY(TypeName);               \
    DISALLOW_ASSIGN(TypeName)

// ---------------------------------------------------------------------------
// Array size (compile-time constant, rejects pointers)
// ---------------------------------------------------------------------------

namespace fast {
namespace butil {
template <typename T, size_t N>
char (&ArraySizeHelper(T (&array)[N]))[N];

template <typename T, size_t N>
char (&ArraySizeHelper(const T (&array)[N]))[N];
}  // namespace butil
}  // namespace fast

#define arraysize(array) (sizeof(::fast::butil::ArraySizeHelper(array)))
#define ARRAY_SIZE(array) arraysize(array)

// ---------------------------------------------------------------------------
// Compile-time assertions
// ---------------------------------------------------------------------------

#define BAIDU_CASSERT(expr, msg) static_assert(expr, #msg)
#define COMPILE_ASSERT(expr, msg) BAIDU_CASSERT(expr, msg)

// ---------------------------------------------------------------------------
// Stringification / concatenation / typeof / container_of
// ---------------------------------------------------------------------------

#define BAIDU_SYMBOLSTR(a) BAIDU_SYMBOLSTR_HELPER(a)
#define BAIDU_SYMBOLSTR_HELPER(a) #a

#define BAIDU_CONCAT(a, b) BAIDU_CONCAT_HELPER(a, b)
#define BAIDU_CONCAT_HELPER(a, b) a##b

#define BAIDU_TYPEOF decltype

// ptr:     the pointer to the member.
// type:    the type of the container struct this is embedded in.
// member:  the name of the member within the struct.
#define container_of(ptr, type, member) ({                             \
            const BAIDU_TYPEOF( ((type *)0)->member ) *__mptr = (ptr);  \
            (type *)( (char *)__mptr - offsetof(type,member) );})

// ---------------------------------------------------------------------------
// DEFINE_SMALL_ARRAY(MyType, my_array, size, maxsize):
//   my_array is typed `MyType*' and as long as `size'. If `size' <= `maxsize',
//   the array is allocated on the stack; otherwise on the heap.
//   NOTE: never use ARRAY_SIZE(my_array) on it — that is always 1.
// ---------------------------------------------------------------------------

namespace fast {
namespace butil {
namespace internal {
template <typename T> struct ArrayDeleter {
    ArrayDeleter() : arr(0) {}
    ~ArrayDeleter() { delete[] arr; }
    T* arr;
};
}  // namespace internal
}  // namespace butil
}  // namespace fast

#if !defined(__clang__)
#define DEFINE_SMALL_ARRAY(Tp, name, size, maxsize)                    \
    Tp* name = 0;                                                       \
    const unsigned name##_size = (size);                                \
    const unsigned name##_stack_array_size =                            \
        (name##_size <= (maxsize) ? name##_size : 0);                   \
    Tp name##_stack_array[name##_stack_array_size];                     \
    ::fast::butil::internal::ArrayDeleter<Tp> name##_array_deleter;     \
    if (name##_stack_array_size) {                                      \
        name = name##_stack_array;                                      \
    } else {                                                            \
        name = new (::std::nothrow) Tp[name##_size];                    \
        name##_array_deleter.arr = name;                                \
    }
#else
// clang rejects variable-length arrays of non-POD types; use placement new.
namespace fast {
namespace butil {
namespace internal {
template <typename T> struct ArrayCtorDtor {
    ArrayCtorDtor(void* arr, unsigned size) : _arr((T*)arr), _size(size) {
        for (unsigned i = 0; i < size; ++i) { new (_arr + i) T; }
    }
    ~ArrayCtorDtor() {
        for (unsigned i = 0; i < _size; ++i) { _arr[i].~T(); }
    }
private:
    T* _arr;
    unsigned _size;
};
}  // namespace internal
}  // namespace butil
}  // namespace fast

#define DEFINE_SMALL_ARRAY(Tp, name, size, maxsize)                    \
    Tp* name = 0;                                                       \
    const unsigned name##_size = (size);                                \
    const unsigned name##_stack_array_size =                            \
        (name##_size <= (maxsize) ? name##_size : 0);                   \
    char name##_stack_array[sizeof(Tp) * name##_stack_array_size];      \
    ::fast::butil::internal::ArrayDeleter<char> name##_array_deleter;   \
    if (name##_stack_array_size) {                                      \
        name = (Tp*)name##_stack_array;                                 \
    } else {                                                            \
        name = (Tp*)new (::std::nothrow) char[sizeof(Tp) * name##_size];\
        name##_array_deleter.arr = (char*)name;                         \
    }                                                                   \
    const ::fast::butil::internal::ArrayCtorDtor<Tp>                    \
        name##_array_ctor_dtor(name, name##_size);
#endif  // !defined(__clang__)

// ---------------------------------------------------------------------------
// Release assertion (crash with abort(), no logging dependency)
// ---------------------------------------------------------------------------

#define RELEASE_ASSERT(condition)   \
    do {                            \
        if (!(condition)) {         \
            ::abort();              \
        }                           \
    } while (false)

#endif  // FAST_BUTIL_MACROS_H_
