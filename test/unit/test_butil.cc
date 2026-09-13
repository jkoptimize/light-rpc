#include <gtest/gtest.h>
#include <stdint.h>
#include <stddef.h>

#include "butil/build_config.h"
#include "butil/macros.h"
#include "butil/aligned_memory.h"
#include "butil/time.h"
#include "butil/fast_rand.h"

namespace fast {
namespace butil {

namespace {

// build_config.h must define the Linux/x86_64/arm64 + GCC macros.
#if !defined(OS_LINUX) || !defined(ARCH_CPU_64_BITS) || !defined(COMPILER_GCC)
#error "trimmed build_config.h must define OS_LINUX/ARCH_CPU_64_BITS/COMPILER_GCC"
#endif

// DISALLOW_COPY_AND_ASSIGN must at least compile.
class Uncopyable {
public:
    Uncopyable() = default;
private:
    DISALLOW_COPY_AND_ASSIGN(Uncopyable);
};

struct Container {
    int first;
    int second;
};

TEST(ButilMacrosTest, ArraySize) {
    int arr[5];
    EXPECT_EQ(5u, ARRAY_SIZE(arr));
}

TEST(ButilMacrosTest, ContainerOf) {
    Container c{1, 2};
    Container* pc = container_of(&c.second, Container, second);
    EXPECT_EQ(&c, pc);
    EXPECT_EQ(1, pc->first);
}

TEST(AlignedMemoryTest, ConstructInPlace) {
    AlignedMemory<sizeof(int), alignof(int)> slot;
    EXPECT_NE(nullptr, slot.void_data());
    using T = int;
    new (slot.void_data()) T(42);
    EXPECT_EQ(42, *slot.data_as<T>());
    slot.data_as<T>()->~T();
}

TEST(TimeTest, CpuwideTimeMonotonic) {
    const int64_t t1 = cpuwide_time_ns();
    const int64_t t2 = cpuwide_time_ns();
    EXPECT_LE(t1, t2);
}

TEST(TimeTest, GetTimeOfDayUs) {
    const int64_t t1 = gettimeofday_us();
    const int64_t t2 = gettimeofday_us();
    EXPECT_LE(t1, t2);
}

TEST(TimeTest, TimerElapsed) {
    Timer timer(Timer::STARTED);
    EXPECT_GE(timer.n_elapsed(), 0);
}

TEST(TimeTest, TimespecConvert) {
    const timespec ts = microseconds_to_timespec(1500);  // 1.5ms
    EXPECT_EQ(1500, timespec_to_microseconds(ts));
}

TEST(FastRandTest, FastRandInRange) {
    const uint64_t r = fast_rand_less_than(100);
    EXPECT_LT(r, 100u);
}

TEST(FastRandTest, FastRandDeterministicSeed) {
    FastRandSeed a{{1, 2}};
    FastRandSeed b{{1, 2}};
    EXPECT_EQ(fast_rand(&a), fast_rand(&b));  // same seed -> same value
}

}  // namespace

}  // namespace butil
}  // namespace fast
