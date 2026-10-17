#include <gtest/gtest.h>
#include "fast_iobuf.h"

namespace fast {

TEST(IOBufUserDataTest, AppendAndDestroyCallsDeleter) {
    bool deleted = false;
    char* data = new char[1024];
    memset(data, 'A', 1024);

    {
        IOBuf buf;
        buf.append_user_data_with_meta(data, 1024,
            [&deleted](void* p) { deleted = true; delete[] (char*)p; },
            42);
        EXPECT_EQ(buf.length(), 1024u);
        EXPECT_EQ(static_cast<const char*>(buf.fetch1())[0], 'A');
        // meta 可通过 get_first_data_meta 查询
    }
    // buf 析构 → dec_ref → deleter 调用
    EXPECT_TRUE(deleted);
}

TEST(IOBufUserDataTest, CutnPreservesDeleter) {
    bool deleted = false;
    char* data = new char[1024];
    memset(data, 'X', 1024);

    IOBuf buf;
    buf.append_user_data_with_meta(data, 1024,
        [&deleted](void* p) { deleted = true; delete[] (char*)p; },
        0);

    IOBuf cut;
    buf.cutn(&cut, 100);
    buf.clear();  // buf 放弃全部引用

    EXPECT_FALSE(deleted);  // cut 还持有引用
    EXPECT_EQ(cut.length(), 100u);
    cut.clear();  // 最后一个引用释放
    EXPECT_TRUE(deleted);
}

// Each fragment is ordinary user memory; these tests never call verbs.
TEST(IOBufUserDataTest, CopyToOffsetsAcrossBlocksWithoutConsuming) {
    IOBuf buf;
    int releases = 0;
    for (const std::string part : {"ab", "cde", "fghi"}) {
        auto* data = new char[part.size()];
        memcpy(data, part.data(), part.size());
        buf.append_user_data_with_meta(data, part.size(), [&](void* p) {
            ++releases;
            delete[] static_cast<char*>(p);
        }, 0);
    }
    // Exercise a nonzero BlockRef offset as well as the copy_to position.
    buf.pop_front(1);
    const IOBuf& input = buf;
    for (size_t pos = 0; pos <= 10; ++pos) {
        for (size_t count : {size_t(0), size_t(1), size_t(4), size_t(20), size_t(-1)}) {
            char out[32];
            memset(out, '?', sizeof(out));
            const std::string expected = pos >= 8 ? "" : std::string("bcdefghi").substr(pos, count);
            EXPECT_EQ(expected.size(), input.copy_to(out + 1, count, pos));
            EXPECT_EQ(expected, std::string(out + 1, expected.size()));
            EXPECT_EQ('?', out[0]);
            EXPECT_EQ('?', out[expected.size() + 1]);
            EXPECT_EQ(8u, input.length());
            EXPECT_EQ(0, releases);
        }
    }
    char out[8];
    EXPECT_EQ(8u, input.copy_to(out));
    EXPECT_EQ("bcdefghi", std::string(out, 8));
    EXPECT_EQ(0u, input.copy_to(out, size_t(-1), size_t(-1)));
    buf.clear();
    EXPECT_EQ(3, releases);
}

TEST(IOBufUserDataTest, CopyToEmptyAndSmallView) {
    IOBuf buf;
    char out[4] = {};
    EXPECT_EQ(0u, buf.copy_to(out));
    auto* data = new char[3]{'a', 'b', 'c'};
    buf.append_user_data_with_meta(data, 3,
        [](void* p) { delete[] static_cast<char*>(p); }, 0);
    EXPECT_EQ(3u, buf.copy_to(out, sizeof(out)));
    EXPECT_EQ("abc", std::string(out));
    EXPECT_EQ(3u, buf.length());
}

}  // namespace fast
