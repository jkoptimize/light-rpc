#include <gtest/gtest.h>
#include "message_dispatcher.h"
#include "fast_define.h"
#include <arpa/inet.h>
#include <cstring>
#include <cerrno>

using namespace fast;

// ============================================================
// MessageDispatcher: config and pure framing. Frame tests wrap ordinary
// user memory, without initializing the RDMA block pool or invoking verbs.
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

namespace {
IOBuf Bytes(const void* data, size_t size) {
    IOBuf buf;
    auto* copy = new char[size];
    memcpy(copy, data, size);
    buf.append_user_data_with_meta(copy, size,
        [](void* p) { delete[] static_cast<char*>(p); }, 0);
    return buf;
}
}

TEST(MessageDispatcher, InvalidLengthIsNotIncomplete) {
    MessageDispatcher md;
    IOBuf frame;
    for (uint32_t len : {0u, static_cast<uint32_t>(kFrameHeaderBytes - 1)}) {
        uint32_t be = htonl(len);
        auto input = Bytes(&be, sizeof(be));
        EXPECT_EQ(-1, md.CutInputMessage(input, frame));
        EXPECT_EQ(EPROTO, errno);
        EXPECT_EQ(sizeof(be), input.length());
    }
}

TEST(MessageDispatcher, SplitHeaderAndIncompleteBody) {
    MessageDispatcher md;
    uint32_t words[3] = {htonl(kFrameHeaderBytes), 0, 0};
    const char* bytes = reinterpret_cast<const char*>(words);
    auto input = Bytes(bytes, 1);
    IOBuf frame;
    EXPECT_EQ(0, md.CutInputMessage(input, frame));
    input.append(Bytes(bytes + 1, 3));
    EXPECT_EQ(0, md.CutInputMessage(input, frame));
    EXPECT_EQ(4u, input.length());
    input.append(Bytes(bytes + 4, sizeof(words) - 4));
    EXPECT_EQ(1, md.CutInputMessage(input, frame));
    EXPECT_EQ(sizeof(words), frame.length());
    EXPECT_TRUE(input.empty());
}

TEST(MessageDispatcher, InvalidFramePropagatesToCaller) {
    MessageDispatcher md;
    md.SetHandler([](IOBuf&, void*) { ADD_FAILURE(); return 0; }, nullptr);
    uint32_t be = htonl(1);
    auto input = Bytes(&be, sizeof(be));
    // Invalid frame is rejected before an Endpoint or worker is needed.
    EXPECT_EQ(-1, md.ProcessNewMessage(input, nullptr));
    EXPECT_EQ(EPROTO, errno);
}
