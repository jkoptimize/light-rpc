#include <gtest/gtest.h>
#include <arpa/inet.h>
#include <thread>
#include <chrono>
#include <cstring>
#include <infiniband/verbs.h>
#include "fast_rdma_endpoint.h"

namespace fast {
extern bool g_skip_rdma_init;
}

class FastRdmaEndpointTest : public ::testing::Test {
protected:
    void SetUp() override {
        fast::g_skip_rdma_init = true;
    }
    void TearDown() override {
        fast::g_skip_rdma_init = false;
    }
};

// ============================================================
// HelloMessage
// ============================================================

TEST_F(FastRdmaEndpointTest, HelloMessageSerializeDeserialize) {
    fast::HelloMessage msg;
    msg.block_size = 8192;
    msg.sq_size    = 128;
    msg.rq_size    = 128;
    msg.lid        = 5;
    msg.gid[0]     = 0xFE;
    msg.qp_num     = 42;

    uint8_t buf[40];
    msg.Serialize(buf);

    fast::HelloMessage msg2;
    msg2.Deserialize(buf);

    EXPECT_EQ(msg2.block_size, 8192);
    EXPECT_EQ(msg2.sq_size, 128);
    EXPECT_EQ(msg2.rq_size, 128);
    EXPECT_EQ(msg2.lid, 5);
    EXPECT_EQ(msg2.gid[0], 0xFE);
    EXPECT_EQ(msg2.qp_num, 42);
    EXPECT_EQ(memcmp(msg2.magic, "RDMA", 4), 0);
}

TEST_F(FastRdmaEndpointTest, HelloNegotiationValid) {
    fast::HelloMessage msg;
    msg.block_size = 8192;
    msg.sq_size    = 128;
    msg.rq_size    = 128;

    EXPECT_TRUE(fast::HelloNegotiationValid(msg));

    msg.impl_ver = 0;
    EXPECT_FALSE(fast::HelloNegotiationValid(msg));
}

TEST_F(FastRdmaEndpointTest, HelloNegotiationInvalidBlockSize) {
    fast::HelloMessage msg;
    msg.block_size = 512;  // below kMinBlockSize=1024
    msg.sq_size    = 128;
    msg.rq_size    = 128;
    EXPECT_FALSE(fast::HelloNegotiationValid(msg));
}

TEST_F(FastRdmaEndpointTest, HelloNegotiationInvalidQpSize) {
    fast::HelloMessage msg;
    msg.block_size = 8192;
    msg.sq_size    = 10;  // below kMinQpSize=16
    msg.rq_size    = 128;
    EXPECT_FALSE(fast::HelloNegotiationValid(msg));
}

// ============================================================
// Flow control
// ============================================================

TEST_F(FastRdmaEndpointTest, FlowControlInit) {
    fast::FastRdmaEndpoint ep;
    ep.SetNegotiatedParams(32, 32, 32, 32, 8192);

    EXPECT_TRUE(ep.IsWritable());
    EXPECT_EQ(ep.sq_window_size(), 29);
    EXPECT_EQ(ep.remote_rq_window_size(), 29);
}

TEST_F(FastRdmaEndpointTest, WaitForWritableBlocksAndWakes) {
    fast::FastRdmaEndpoint ep;
    ep.SetNegotiatedParams(32, 32, 32, 32, 8192);

    // exhaust window: send 29 times (32-3=29)
    ep.SimulateSendN(29);
    EXPECT_FALSE(ep.IsWritable());

    std::atomic<bool> woken{false};
    std::thread waiter([&] {
        ep.WaitForWritable();
        woken = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(woken);

    // Simulate SEND WC completing 1 batch
    ibv_wc wc{};
    wc.opcode = IBV_WC_SEND;
    wc.wr_id  = 1;
    ep.HandleCompletion(wc);

    waiter.join();
    EXPECT_TRUE(woken);
    EXPECT_EQ(ep.sq_window_size(), 1);
}

TEST_F(FastRdmaEndpointTest, MultipleWaiters) {
    fast::FastRdmaEndpoint ep;
    ep.SetNegotiatedParams(32, 32, 32, 32, 8192);
    ep.SimulateSendN(29);

    std::atomic<int> woken{0};
    auto waiter_func = [&] {
        ep.WaitForWritable();
        woken.fetch_add(1);
    };

    std::thread t1(waiter_func);
    std::thread t2(waiter_func);
    std::thread t3(waiter_func);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    ibv_wc wc{};
    wc.opcode = IBV_WC_SEND;
    wc.wr_id  = 1;
    ep.HandleCompletion(wc);

    t1.join(); t2.join(); t3.join();
    EXPECT_EQ(woken.load(), 3);
}

TEST_F(FastRdmaEndpointTest, WaitConsumeBehavior) {
    fast::FastRdmaEndpoint ep;
    ep.SetNegotiatedParams(32, 32, 32, 32, 8192);
    ep.SimulateSendN(29);

    std::thread t1([&] { ep.WaitForWritable(); });
    std::thread t2([&] { ep.WaitForWritable(); });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    ibv_wc wc{};
    wc.opcode = IBV_WC_SEND;
    wc.wr_id  = 1;
    ep.HandleCompletion(wc);

    t1.join(); t2.join();

    // Window consumed again, third wait should block
    ep.SimulateSendN(1);
    EXPECT_FALSE(ep.IsWritable());

    std::atomic<bool> blocked{false};
    std::thread t3([&] {
        ep.WaitForWritable();
        blocked = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(blocked);

    wc.wr_id = 1;
    ep.HandleCompletion(wc);
    t3.join();
    EXPECT_TRUE(blocked);
}

// ============================================================
// SendAck
// ============================================================

TEST_F(FastRdmaEndpointTest, SendAckBelowThreshold) {
    fast::FastRdmaEndpoint ep;
    ep.SetNegotiatedParams(32, 32, 32, 32, 8192);
    // remote_window_capacity_ = min(32,32)-3 = 29, threshold = 14

    // new_rq_wrs = 10 < 14: no trigger
    EXPECT_EQ(ep.TestSendAck(10), 0);
    EXPECT_EQ(ep.new_rq_wrs(), 10);
}

TEST_F(FastRdmaEndpointTest, SendAckTriggersAboveThreshold) {
    fast::FastRdmaEndpoint ep;
    ep.SetNegotiatedParams(32, 32, 32, 32, 8192);

    // new_rq_wrs = 15 >= 14: triggers (but SendImm returns 0 with skip_rdma_init)
    ep.TestSendAck(15);
    EXPECT_EQ(ep.TestSendAck(0), 0);
}

// ============================================================
// HandleCompletion
// ============================================================

TEST_F(FastRdmaEndpointTest, HandleSendCompletion) {
    fast::FastRdmaEndpoint ep;
    ep.SetNegotiatedParams(32, 32, 32, 32, 8192);
    ep.SimulateSendN(3);

    EXPECT_EQ(ep.sq_window_size(), 26);

    ibv_wc wc{};
    wc.opcode = IBV_WC_SEND;
    wc.wr_id  = 3;
    ep.HandleCompletion(wc);

    EXPECT_EQ(ep.sq_window_size(), 29);
}

TEST_F(FastRdmaEndpointTest, HandleImmSendCompletion) {
    fast::FastRdmaEndpoint ep;
    ep.SetNegotiatedParams(32, 32, 32, 32, 8192);

    ibv_wc wc{};
    wc.opcode = IBV_WC_SEND;
    wc.wr_id  = 0;  // Pure ACK / IMM
    ssize_t ret = ep.HandleCompletion(wc);

    EXPECT_EQ(ret, 0);
    EXPECT_EQ(ep.sq_window_size(), 29);  // unchanged for IMM
}

TEST_F(FastRdmaEndpointTest, HandleRecvCompletionWithImm) {
    fast::FastRdmaEndpoint ep;
    ep.SetNegotiatedParams(32, 32, 32, 32, 8192);

    EXPECT_EQ(ep.remote_rq_window_size(), 29);

    ibv_wc wc{};
    wc.opcode    = IBV_WC_RECV;
    wc.byte_len  = 1024;
    wc.wc_flags  = IBV_WC_WITH_IMM;
    wc.imm_data  = htonl(3);  // remote freed 3 RQ slots

    ssize_t nr = ep.HandleCompletion(wc);

    EXPECT_EQ(nr, 1024);
    EXPECT_EQ(ep.remote_rq_window_size(), 32);
}

// ============================================================
// CutFromIOBufList (with g_skip_rdma_init)
// ============================================================

TEST_F(FastRdmaEndpointTest, CutFromIOBufListSkipInit) {
    fast::FastRdmaEndpoint ep;
    ep.SetNegotiatedParams(32, 32, 32, 32, 8192);

    // With g_skip_rdma_init=true, CutFromIOBufList ignores IOBuf content.
    // Use empty IOBufs to avoid needing block pool initialization.
    fast::IOBuf iobuf1, iobuf2;
    fast::IOBuf* arr[2] = { &iobuf1, &iobuf2 };

    EXPECT_EQ(ep.sq_window_size(), 29);
    ssize_t ret = ep.CutFromIOBufList(arr, 2);
    EXPECT_EQ(ret, 2);
    EXPECT_EQ(ep.sq_window_size(), 27);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
