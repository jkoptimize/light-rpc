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

// Date: Wed Nov 27 12:59:20 CST 2013

// Ported from brpc butil/containers/flat_map.h, trimmed to the non-Sparse /
// non-Multi core used by the bthread core (butex_wake_n's per-tag TaskGroup
// dedup map). The original algorithm is preserved verbatim:
//   - closed addressing (separate chaining), first node inlined in the bucket
//     array via ManualConstructor<Element>.
//   - SingleThreadedPool for node reuse.
//   - load-factor driven resize (power-of-2 rehash).
// Removed: SparseFlatMap/SparseFlatSet/MultiFlatMap/FlatSet, bit_array
// thumbnail, DefaultHasher<std::string>/StringPiece specializations,
// find_cstr/find_lowered_cstr helpers.

#ifndef FAST_BUTIL_FLAT_MAP_H
#define FAST_BUTIL_FLAT_MAP_H

#include <stdint.h>    // uint32_t
#include <cstddef>     // size_t
#include <vector>      // std::vector (seek_all)
#include <utility>     // std::pair
#include <functional>  // std::hash, std::equal_to
#include <iostream>    // std::ostream
#include <optional>    // std::optional, std::nullopt

#include "manual_constructor.h"
#include "single_threaded_pool.h"

namespace fast {
namespace butil {

template <typename _Map, typename _Element> class FlatMapIterator;
template <typename K, typename T> class FlatMapElement;
template <typename K> struct DefaultHasher;
template <typename K> struct DefaultEqualTo;

struct BucketInfo {
    size_t longest_length;
    double average_length;
};

#ifndef BRPC_FLATMAP_DEFAULT_NBUCKET
#define BRPC_FLATMAP_DEFAULT_NBUCKET 16U
#endif

// NOTE: Objects stored in FlatMap MUST be copyable.
template <typename _K, typename _T,
          typename _Hash = DefaultHasher<_K>,
          typename _Equal = DefaultEqualTo<_K>,
          typename _Alloc = PtAllocator>
class FlatMap {
public:
    typedef _K key_type;
    typedef _T mapped_type;
    typedef _Alloc allocator_type;
    typedef FlatMapElement<_K, _T> Element;
    typedef typename Element::value_type value_type;
    typedef FlatMapIterator<FlatMap, value_type> iterator;
    typedef FlatMapIterator<FlatMap, const value_type> const_iterator;
    typedef _Hash hasher;
    typedef _Equal key_equal;
    static constexpr size_t DEFAULT_NBUCKET = BRPC_FLATMAP_DEFAULT_NBUCKET;

    struct PositionHint {
        size_t nbucket;
        size_t offset;
        bool at_entry;
        key_type key;
    };

    explicit FlatMap(const hasher& hashfn = hasher(),
                     const key_equal& eql = key_equal(),
                     const allocator_type& alloc = allocator_type());
    FlatMap(const FlatMap& rhs);
    ~FlatMap();

    FlatMap& operator=(const FlatMap& rhs);
    void swap(FlatMap & rhs);

    // FlatMap will be automatically initialized with small FlatMap optimization,
    // so this function only needs to be call when a large initial number of
    // buckets or non-default `load_factor' is required.
    // Returns 0 on success, -1 on error, but FlatMap can still be used normally.
    int init(size_t nbucket, uint32_t load_factor = 80);

    // Insert a pair of |key| and |value|.
    mapped_type* insert(const key_type& key, const mapped_type& value);
    mapped_type* insert(const std::pair<key_type, mapped_type>& kv);

    // Remove |key| and its value. Returns 1 on erased, 0 otherwise.
    template <typename K2>
    size_t erase(const K2& key, mapped_type* old_value = NULL);

    // Remove all items. Allocated spaces are NOT returned by system.
    void clear();

    // Remove all items and return all allocated spaces to system.
    void clear_and_reset_pool();

    // Search for the value associated with |key|. Returns its address or NULL.
    template <typename K2> mapped_type* seek(const K2& key) const;
    template <typename K2> std::vector<mapped_type*> seek_all(const K2& key) const;

    // Get the value associated with |key|, inserting a default-constructed
    // value if absent. Returns reference of the value.
    mapped_type& operator[](const key_type& key);

    // Resize this map (optional; also triggered by insert/operator[]).
    bool resize(size_t nbucket);

    // Iterators
    iterator begin();
    iterator end();
    const_iterator begin() const;
    const_iterator end() const;

    // Iterate inconsistently across passes (see save/restore semantics).
    void save_iterator(const const_iterator&, PositionHint*) const;
    const_iterator restore_iterator(const PositionHint&) const;

    bool initialized() const { return _buckets != NULL; }
    bool empty() const { return _size == 0; }
    size_t size() const { return _size; }
    size_t bucket_count() const { return _nbucket; }
    uint32_t load_factor() const { return _load_factor; }

    BucketInfo bucket_info() const;

    struct Bucket {
        Bucket() : next((Bucket*)-1UL) {}
        explicit Bucket(const _K& k) : next(NULL) {
            element_space_.Init(k);
        }
        Bucket(const Bucket& other) : next(NULL) {
            element_space_.Init(other.element());
        }

        bool is_valid() const { return next != (const Bucket*)-1UL; }
        void set_invalid() { next = (Bucket*)-1UL; }
        // NOTE: Only be called when is_valid() is true.
        Element& element() { return *element_space_; }
        const Element& element() const { return *element_space_; }
        void destroy_element() { element_space_.Destroy(); }

        void swap(Bucket& rhs) {
            if (!is_valid() && !rhs.is_valid()) {
                return;
            } else if (is_valid() && !rhs.is_valid()) {
                rhs.element_space_.Init(std::move(element()));
                destroy_element();
            } else if (!is_valid() && rhs.is_valid()) {
                element_space_.Init(std::move(rhs.element()));
                rhs.destroy_element();
            } else {
                element().swap(rhs.element());
            }
            std::swap(next, rhs.next);
        }

        Bucket* next;

    private:
        ManualConstructor<Element> element_space_;
    };

private:
    template <typename _Map, typename _Element> friend class FlatMapIterator;

    struct NewBucketsInfo {
        NewBucketsInfo() : buckets(NULL), nbucket(0) {}
        NewBucketsInfo(Bucket* b, size_t n) : buckets(b), nbucket(n) {}
        Bucket* buckets;
        size_t nbucket;
    };

    std::optional<NewBucketsInfo> new_buckets_and_thumbnail(size_t size,
                                                            size_t new_nbucket);

    allocator_type& get_allocator() { return _pool.get_allocator(); }
    allocator_type get_allocator() const { return _pool.get_allocator(); }

    bool is_too_crowded(size_t size) const {
        return is_too_crowded(size, _nbucket, _load_factor);
    }
    static bool is_too_crowded(size_t size, size_t nbucket, uint32_t load_factor) {
        return size * 100 >= nbucket * load_factor;
    }

    void init_load_factor(uint32_t load_factor) {
        if (_is_default_load_factor) {
            _is_default_load_factor = false;
            _load_factor = load_factor;
        }
    }

    bool is_default_buckets() const {
        return _buckets == (Bucket*)(&_default_buckets);
    }

    static void init_buckets_and_thumbnail(Bucket* buckets, size_t nbucket) {
        for (size_t i = 0; i < nbucket; ++i) {
            buckets[i].set_invalid();
        }
        buckets[nbucket].next = NULL;
    }

    // Note: need an extra bucket to let iterator know where buckets end.
    // Small map optimization.
    Bucket _default_buckets[DEFAULT_NBUCKET + 1];
    size_t _size;
    size_t _nbucket;
    Bucket* _buckets;
    uint32_t _load_factor;
    bool _is_default_load_factor;
    hasher _hashfn;
    key_equal _eql;
    SingleThreadedPool<sizeof(Bucket), 1024, 16, allocator_type> _pool;
};

// Implement FlatMapElement
template <typename K, typename T>
class FlatMapElement {
public:
    typedef std::pair<const K, T> value_type;
    // NOTE: Have to initialize _value in this way which is treated by GCC
    // specially that _value is zeroized(POD) or constructed(non-POD).
    explicit FlatMapElement(const K& k) : _key(k), _value(T()) {}

    FlatMapElement(const FlatMapElement& rhs)
        : _key(rhs._key), _value(rhs._value) {}

    FlatMapElement(FlatMapElement&& rhs) noexcept
        : _key(std::move(rhs._key)), _value(std::move(rhs._value)) {}

    const K& first_ref() const { return _key; }
    T& second_ref() { return _value; }
    T&& second_movable_ref() { return std::move(_value); }
    value_type& value_ref() { return *reinterpret_cast<value_type*>(this); }
    inline static const K& first_ref_from_value(const value_type& v)
    { return v.first; }
    inline static const T& second_ref_from_value(const value_type& v)
    { return v.second; }
    inline static T&& second_movable_ref_from_value(value_type& v)
    { return std::move(v.second); }

    void swap(FlatMapElement& rhs) {
        std::swap(_key, rhs._key);
        std::swap(_value, rhs._value);
    }

private:
    K _key;
    T _value;
};

// Implement DefaultHasher and DefaultEqualTo
template <typename K>
struct DefaultHasher : public std::hash<K> {};

template <typename K>
struct DefaultEqualTo : public std::equal_to<K> {};

}  // namespace butil
}  // namespace fast

#include "flat_map_inl.h"

#endif  // FAST_BUTIL_FLAT_MAP_H
