#include <gtest/gtest.h>
#include "message_dispatcher.h"

using namespace fast;

// ============================================================
// MessageDispatcher: basic config (no IOBuf — IOBuf requires
// RDMA block pool which is unavailable in unit test environment)
// ============================================================

TEST(MessageDispatcher, DefaultMode) {
    MessageDispatcher md;
    EXPECT_EQ(md._mode, DispatcherMode::kServer);
}

TEST(MessageDispatcher, SetMode) {
    MessageDispatcher md;
    md.SetMode(DispatcherMode::kClient);
    EXPECT_EQ(md._mode, DispatcherMode::kClient);
}

TEST(MessageDispatcher, SetHandler) {
    MessageDispatcher md;
    int arg = 42;
    md.SetHandler([](IOBuf&, void*) -> int { return 0; }, &arg);

    EXPECT_NE(md._handler, nullptr);
    EXPECT_EQ(md._arg, &arg);
}

TEST(MessageDispatcher, SetHandlerNullArg) {
    MessageDispatcher md;
    md.SetHandler([](IOBuf&, void*) -> int { return 0; }, nullptr);

    EXPECT_NE(md._handler, nullptr);
    EXPECT_EQ(md._arg, nullptr);
}
