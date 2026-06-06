#include <gtest/gtest.h>
#include <arpa/inet.h>
#include <thread>
#include <chrono>
#include <cstring>
#include <infiniband/verbs.h>
#include "fast_rdma_endpoint.h"

// ============================================================
// HelloMessage
// ============================================================

TEST(FastRdmaEndpoint, HelloMessageSerializeDeserialize) {
    fast::HelloMessage msg;
    msg.block_size = 8192;
    msg.sq_size    = 128;
    msg.rq_size    = 128;
    msg.lid        = 5;
    msg.gid.raw[0]     = 0xFE;
    msg.qp_num     = 42;

    uint8_t buf[40];
    msg.Serialize(buf);

    fast::HelloMessage msg2;
    msg2.Deserialize(buf);

    EXPECT_EQ(msg2.block_size, 8192);
    EXPECT_EQ(msg2.sq_size, 128);
    EXPECT_EQ(msg2.rq_size, 128);
    EXPECT_EQ(msg2.lid, 5);
    EXPECT_EQ(msg2.gid.raw[0], 0xFE);
    EXPECT_EQ(msg2.qp_num, 42);
    EXPECT_EQ(memcmp(msg2.magic, "RDMA", 4), 0);
}

TEST(FastRdmaEndpoint, HelloNegotiationValid) {
    fast::HelloMessage msg;
    msg.block_size = 8192;
    msg.sq_size    = 128;
    msg.rq_size    = 128;

    EXPECT_TRUE(fast::HelloNegotiationValid(msg));

    msg.impl_ver = 0;
    EXPECT_FALSE(fast::HelloNegotiationValid(msg));
}

TEST(FastRdmaEndpoint, HelloNegotiationInvalidBlockSize) {
    fast::HelloMessage msg;
    msg.block_size = 512;
    msg.sq_size    = 128;
    msg.rq_size    = 128;
    EXPECT_FALSE(fast::HelloNegotiationValid(msg));
}

TEST(FastRdmaEndpoint, HelloNegotiationInvalidQpSize) {
    fast::HelloMessage msg;
    msg.block_size = 8192;
    msg.sq_size    = 10;
    msg.rq_size    = 128;
    EXPECT_FALSE(fast::HelloNegotiationValid(msg));
}

// ============================================================
// Flow control
// ============================================================

TEST(FastRdmaEndpoint, FlowControlInit) {
    fast::FastRdmaEndpoint ep;
    ep.SetNegotiatedParams(32, 32, 32, 32, 8192);

    EXPECT_TRUE(ep.IsWritable());
    EXPECT_EQ(ep.sq_window_size(), 29);
    EXPECT_EQ(ep.remote_rq_window_size(), 29);
}

TEST(FastRdmaEndpoint, WaitForWritableBlocksAndWakes) {
    fast::FastRdmaEndpoint ep;
    ep.SetNegotiatedParams(32, 32, 32, 32, 8192);

    ep.SimulateSendN(29);
    EXPECT_FALSE(ep.IsWritable());

    std::atomic<bool> woken{false};
    std::thread waiter([&] {
        ep.WaitForWritable();
        woken = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(woken);

    ibv_wc wc{};
    wc.opcode = IBV_WC_SEND;
    wc.wr_id  = 1;
    ep.HandleCompletion(wc);

    waiter.join();
    EXPECT_TRUE(woken);
    EXPECT_EQ(ep.sq_window_size(), 1);
}

TEST(FastRdmaEndpoint, MultipleWaiters) {
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

TEST(FastRdmaEndpoint, WaitConsumeBehavior) {
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
// SendAck (pure threshold logic, SendImm is a stub)
// ============================================================

TEST(FastRdmaEndpoint, SendAckBelowThreshold) {
    fast::FastRdmaEndpoint ep;
    ep.SetNegotiatedParams(32, 32, 32, 32, 8192);

    EXPECT_EQ(ep.TestSendAck(10), 0);
    EXPECT_EQ(ep.new_rq_wrs(), 10);
}

// ============================================================
// HandleCompletion (SEND WC only — pure window logic)
// ============================================================

TEST(FastRdmaEndpoint, HandleSendCompletion) {
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

TEST(FastRdmaEndpoint, HandleImmSendCompletion) {
    fast::FastRdmaEndpoint ep;
    ep.SetNegotiatedParams(32, 32, 32, 32, 8192);

    ibv_wc wc{};
    wc.opcode = IBV_WC_SEND;
    wc.wr_id  = 0;
    ssize_t ret = ep.HandleCompletion(wc);

    EXPECT_EQ(ret, 0);
    EXPECT_EQ(ep.sq_window_size(), 29);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
