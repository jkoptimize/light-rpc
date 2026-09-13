// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// ManualConstructor statically-allocates space in which to store some
// object, but does not initialize it.  You can then call the constructor
// and destructor for the object yourself as you see fit.  This is useful
// for memory management optimizations, where you want to initialize and
// destroy an object multiple times but only allocate it once.
//
// (When I say ManualConstructor statically allocates space, I mean that
// the ManualConstructor object itself is forced to be the right size.)
//
// For example usage, check out butil/containers/small_map.h.

#ifndef FAST_BUTIL_MEMORY_MANUAL_CONSTRUCTOR_H_
#define FAST_BUTIL_MEMORY_MANUAL_CONSTRUCTOR_H_

#include <stddef.h>
#include <utility>  // std::forward

#include "aligned_memory.h"

namespace fast {
namespace butil {

template <typename Type>
class ManualConstructor {
public:
    // No constructor or destructor because one of the most useful uses of
    // this class is as part of a union, and members of a union cannot have
    // constructors or destructors.  And, anyway, the whole point of this
    // class is to bypass these.

    // NOTE: operator new[]/delete[] removed in the port — they depend on
    // AlignedAlloc/AlignedFree which the trimmed aligned_memory.h does not
    // provide, and FlatMap never allocates ManualConstructor arrays.

    Type* get() {
        return _space.template data_as<Type>();
    }

    const Type* get() const  {
        return _space.template data_as<Type>();
    }

    Type* operator->() { return get(); }
    const Type* operator->() const { return get(); }

    Type& operator*() { return *get(); }
    const Type& operator*() const { return *get(); }

    template<typename... Args>
    void Init(Args&&... args) {
        new (_space.void_data()) Type(std::forward<Args>(args)...);
    }

     void Destroy() { get()->~Type(); }

 private:
    AlignedMemory<sizeof(Type), __alignof__(Type)> _space;
};

}  // namespace butil
}  // namespace fast

#endif  // FAST_BUTIL_MEMORY_MANUAL_CONSTRUCTOR_H_
