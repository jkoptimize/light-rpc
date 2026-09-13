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
// Ported from brpc butil/time.cpp, trimmed to Linux. Removed:
//   - NO_CLOCK_GETTIME_IN_MAC branch
//   - detail::read_cpu_frequency / read_invariant_cpu_frequency
//     (only used by the removed BAIDU_INTERNAL cpuwide_time_ns)

#include "time.h"

namespace fast {
namespace butil {

int64_t monotonic_time_ns() {
    // MONOTONIC_RAW is slower than MONOTONIC in linux 2.6.32; using the RAW
    // version does not make sense anymore.
    // NOTE: Not inline to keep ABI-compatible with previous versions.
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1000000000L + now.tv_nsec;
}

}  // namespace butil
}  // namespace fast
