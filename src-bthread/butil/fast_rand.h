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
// Ported from brpc butil/fast_rand.h, trimmed to the xorshift128+ core used by
// work-stealing. Removed: fast_rand_in, fast_rand_double, fast_rand_bytes,
// fast_rand_printable (unused by bthread core).

#ifndef FAST_BUTIL_FAST_RAND_H
#define FAST_BUTIL_FAST_RAND_H

#include <cstddef>
#include <stdint.h>

namespace fast {
namespace butil {

// Generate random values fast without global contentions.
// All functions in this header are thread-safe.

struct FastRandSeed {
    uint64_t s[2];
};

// Initialize the seed.
void init_fast_rand_seed(FastRandSeed* seed);

// Generate an unsigned 64-bit random number from thread-local or given seed.
// Cost: ~5ns
uint64_t fast_rand();
uint64_t fast_rand(FastRandSeed*);

// Generate an unsigned 64-bit random number inside [0, range) from
// thread-local seed. Returns 0 when range is 0.
// Cost: ~30ns
uint64_t fast_rand_less_than(uint64_t range);

}  // namespace butil
}  // namespace fast

#endif  // FAST_BUTIL_FAST_RAND_H
