// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Ported from brpc butil/memory/aligned_memory.h, trimmed to the
// AlignedMemory<> storage unit used by resource_pool / object_pool.
// AlignedAlloc/AlignedFree/AlignedFreeDeleter are not used by the bthread
// core and were removed.

// AlignedMemory is a POD type that provides a portable way to declare static
// or stack data of a given alignment and size, with manual control over
// construction/destruction. Usage:
//
//   static AlignedMemory<sizeof(MyClass), alignof(MyClass)> my_class;
//   new (my_class.void_data()) MyClass();        // construct in place
//   MyClass* mc = my_class.data_as<MyClass>();   // access
//   mc->~MyClass();                              // manual destruct

#ifndef FAST_BUTIL_MEMORY_ALIGNED_MEMORY_H_
#define FAST_BUTIL_MEMORY_ALIGNED_MEMORY_H_

#include <stddef.h>  // size_t
#include <stdint.h>  // uint8_t

namespace fast {
namespace butil {

// Primary template: unsupported alignment yields a compile error.
template <size_t Size, size_t ByteAlignment>
struct AlignedMemory {};

#define BUTIL_DECL_ALIGNED_MEMORY(byte_alignment)                 \
    template <size_t Size>                                        \
    class AlignedMemory<Size, byte_alignment> {                   \
     public:                                                      \
      alignas(byte_alignment) uint8_t data_[Size];                \
      void* void_data() { return static_cast<void*>(data_); }     \
      const void* void_data() const {                             \
        return static_cast<const void*>(data_);                   \
      }                                                           \
      template<typename Type>                                     \
      Type* data_as() { return static_cast<Type*>(void_data()); } \
      template<typename Type>                                     \
      const Type* data_as() const {                               \
        return static_cast<const Type*>(void_data());             \
      }                                                           \
     private:                                                     \
      void* operator new(size_t);                                 \
      void operator delete(void*);                                \
    }

// Explicit specializations for the supported alignments. 4096 is the maximum
// some compilers accept.
BUTIL_DECL_ALIGNED_MEMORY(1);
BUTIL_DECL_ALIGNED_MEMORY(2);
BUTIL_DECL_ALIGNED_MEMORY(4);
BUTIL_DECL_ALIGNED_MEMORY(8);
BUTIL_DECL_ALIGNED_MEMORY(16);
BUTIL_DECL_ALIGNED_MEMORY(32);
BUTIL_DECL_ALIGNED_MEMORY(64);
BUTIL_DECL_ALIGNED_MEMORY(128);
BUTIL_DECL_ALIGNED_MEMORY(256);
BUTIL_DECL_ALIGNED_MEMORY(512);
BUTIL_DECL_ALIGNED_MEMORY(1024);
BUTIL_DECL_ALIGNED_MEMORY(2048);
BUTIL_DECL_ALIGNED_MEMORY(4096);

#undef BUTIL_DECL_ALIGNED_MEMORY

}  // namespace butil
}  // namespace fast

#endif  // FAST_BUTIL_MEMORY_ALIGNED_MEMORY_H_
