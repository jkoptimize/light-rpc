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

}  // namespace fast
