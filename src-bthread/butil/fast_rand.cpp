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
// Ported from brpc butil/fast_rand.cpp, trimmed to the xorshift128+ core.

#include <limits>          // numeric_limits

#include "time.h"          // gettimeofday_us()
#include "fast_rand.h"

namespace fast {
namespace butil {

// This state can be seeded with any value.
typedef uint64_t SplitMix64Seed;

// A very fast generator passing BigCrush. Due to its 64-bit state, it's
// solely for seeding xorshift128+.
inline uint64_t splitmix64_next(SplitMix64Seed* seed) {
    uint64_t z = (*seed += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

// xorshift128+ is the fastest generator passing BigCrush without systematic
// failures.
inline uint64_t xorshift128_next(FastRandSeed* seed) {
    uint64_t s1 = seed->s[0];
    const uint64_t s0 = seed->s[1];
    seed->s[0] = s0;
    s1 ^= s1 << 23; // a
    seed->s[1] = s1 ^ s0 ^ (s1 >> 18) ^ (s0 >> 5); // b, c
    return seed->s[1] + s0;
}

// seed xorshift128+ with splitmix64.
void init_fast_rand_seed(FastRandSeed* seed) {
    SplitMix64Seed seed4seed = gettimeofday_us();
    seed->s[0] = splitmix64_next(&seed4seed);
    seed->s[1] = splitmix64_next(&seed4seed);
}

inline uint64_t fast_rand_impl(uint64_t range, FastRandSeed* seed) {
    // Separating uint64_t values into following intervals:
    //   [0,range-1][range,range*2-1] ... [uint64_max/range*range,uint64_max]
    // If the generated 64-bit random value falls into any interval except the
    // last one, the probability of taking any value inside [0, range-1] is
    // same. If the value falls into last interval, we retry the process until
    // the value falls into other intervals. If min/max are limited to 32-bits,
    // the retrying is rare. The amortized retrying count at maximum is 1 when
    // range equals 2^32. A corner case is that even if the range is power of
    // 2(e.g. min=0 max=65535) in which case the retrying can be avoided, we
    // still retry currently. The reason is just to keep the code simpler
    // and faster for most cases.
    const uint64_t div = std::numeric_limits<uint64_t>::max() / range;
    uint64_t result;
    do {
        result = xorshift128_next(seed) / div;
    } while (result >= range);
    return result;
}

// Seeds for different threads are stored separately in thread-local storage.
static __thread FastRandSeed _tls_seed = { { 0, 0 } };

// True if the seed is (probably) uninitialized. There's definitely false
// positive, but it's OK for us.
inline bool need_init(const FastRandSeed& seed) {
    return seed.s[0] == 0 && seed.s[1] == 0;
}

uint64_t fast_rand() {
    if (need_init(_tls_seed)) {
        init_fast_rand_seed(&_tls_seed);
    }
    return xorshift128_next(&_tls_seed);
}

uint64_t fast_rand(FastRandSeed* seed) {
    return xorshift128_next(seed);
}

uint64_t fast_rand_less_than(uint64_t range) {
    if (range == 0) {
        return 0;
    }
    if (need_init(_tls_seed)) {
        init_fast_rand_seed(&_tls_seed);
    }
    return fast_rand_impl(range, &_tls_seed);
}

}  // namespace butil
}  // namespace fast
