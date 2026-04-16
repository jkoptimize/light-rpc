// Note: no include guard here. fast_iobuf.h is designed to be included from
// fast_iobuf_inl.h and vice versa. The mutual include works because
// fast_iobuf.h closes namespace fast BEFORE including fast_iobuf_inl.h.
#ifndef FAST_IOBUF_H
#define FAST_IOBUF_H

#include <stdint.h>
#include <string>
#include <functional>
#include <google/protobuf/io/zero_copy_stream.h>
#include "fast_utils.h"

namespace fast
{
    class IOBuf
    {
        friend class IOBufAsZeroCopyInputStream;
        friend class IOBufAsZeroCopyOutputStream;
        friend class FastChannel;
        friend class FastServer;

    public:
        static const size_t DEFAULT_BLOCK_SIZE = 8192;
        static const size_t INITIAL_CAP = 32; // must be power of 2

        struct Block;
        struct BlockRef
        {
            uint32_t offset;
            uint32_t length;
            Block *block;
        };

        /// @brief Number of block references in this IOBuf.
        size_t ref_num() const { return _ref_num(); }

        /// @brief Get the i-th BlockRef.
        const BlockRef& ref_at(size_t i) const { return _ref_at(i); }

        struct SmallView
        {
            BlockRef refs[2];
        };

        struct BigView
        {
            int32_t magic;
            uint32_t start;
            BlockRef *refs;
            uint32_t nref;
            uint32_t cap_mask;
            size_t nbytes;

            const BlockRef &ref_at(uint32_t i) const
            {
                return refs[(start + i) & cap_mask];
            }

            BlockRef &ref_at(uint32_t i)
            {
                return refs[(start + i) & cap_mask];
            }

            uint32_t capacity() const { return cap_mask + 1; }
        };

        struct Movable
        {
            explicit Movable(IOBuf &v) : _v(&v) {}
            IOBuf &value() const { return *_v; }

        private:
            IOBuf *_v;
        };

        IOBuf();
        IOBuf(const IOBuf &);
        IOBuf(const Movable &);
        ~IOBuf() { clear(); };
        void operator=(const IOBuf &);
        void operator=(const Movable &);
        void operator=(const char *);
        void operator=(const std::string &);

        void swap(IOBuf &);
        size_t pop_front(size_t n);
        size_t pop_back(size_t n);
        size_t cutn(IOBuf *out, size_t n);
        size_t cutn(void *out, size_t n);
        size_t cutn(std::string *out, size_t n);
        bool cut1(void *c);
        void append(const IOBuf &other);
        void append(const Movable &other);
        int push_back(char c);
        int append(void const *data, size_t count);
        int append(char const *s);
        int append(const std::string &s);
        int resize(size_t n) { return resize(n, '\0'); }
        int resize(size_t n, char c);
        void clear();
        bool empty() const;
        size_t length() const;
        size_t size() const { return length(); }
        bool equals(const IOBuf &other) const;

    protected:
        bool _small() const;

        template <bool MOVE>
        void _push_or_move_back_ref_to_smallview(const BlockRef &);
        template <bool MOVE>
        void _push_or_move_back_ref_to_bigview(const BlockRef &);

        void _push_back_ref(const BlockRef &);
        void _move_back_ref(const BlockRef &);
        int _pop_front_ref() { return _pop_or_moveout_front_ref<false>(); }
        int _moveout_front_ref() { return _pop_or_moveout_front_ref<true>(); }

        template <bool MOVEOUT>
        int _pop_or_moveout_front_ref();
        int _pop_back_ref();
        size_t _ref_num() const;
        BlockRef &_front_ref();
        const BlockRef &_front_ref() const;
        BlockRef &_back_ref();
        const BlockRef &_back_ref() const;
        BlockRef &_ref_at(size_t i);
        const BlockRef &_ref_at(size_t i) const;
        const BlockRef *_pref_at(size_t i) const;

    private:
        union
        {
            BigView _bv;
            SmallView _sv;
        };
    };

    std::ostream &operator<<(std::ostream &, const IOBuf &buf);

    inline bool operator==(const fast::IOBuf &b1, const fast::IOBuf &b2)
    {
        return b1.equals(b2);
    }
    inline bool operator!=(const fast::IOBuf &b1, const fast::IOBuf &b2)
    {
        return !b1.equals(b2);
    }

    class IOBufAsZeroCopyInputStream
        : public google::protobuf::io::ZeroCopyInputStream
    {
    public:
        explicit IOBufAsZeroCopyInputStream(const IOBuf &);
        bool Next(const void **data, int *size) override;
        void BackUp(int count) override;
        bool Skip(int count) override;
        int64_t ByteCount() const override;

    private:
        int _ref_index;
        int _add_offset;
        int64_t _byte_count;
        const IOBuf *_buf;
    };

    class IOBufAsZeroCopyOutputStream
        : public google::protobuf::io::ZeroCopyOutputStream
    {
    public:
        explicit IOBufAsZeroCopyOutputStream(IOBuf *);
        IOBufAsZeroCopyOutputStream(IOBuf *, uint32_t block_size);
        ~IOBufAsZeroCopyOutputStream();
        bool Next(void **data, int *size) override;
        void BackUp(int count) override;
        int64_t ByteCount() const override;

    private:
        void _release_block();
        IOBuf *_buf;
        uint32_t _block_size;
        IOBuf::Block *_cur_block;
        int64_t _byte_count;
    };

    // blockref_array helpers (body in fast_iobuf.cc)
    IOBuf::BlockRef *acquire_blockref_array(size_t cap);
    IOBuf::BlockRef *acquire_blockref_array();
    void release_blockref_array(IOBuf::BlockRef *refs, size_t cap);

    // TLS block management
    IOBuf::Block *share_tls_block();
    void release_tls_block(IOBuf::Block *b);
    IOBuf::Block *acquire_tls_block();
    IOBuf::Block *create_block();
    IOBuf::Block *create_block(const size_t block_size);

} // namespace fast — CLOSED before fast_iobuf_inl.h continues below

// fast_iobuf_inl.h closes its own namespace fast at the end
#include "fast_iobuf_inl.h"

// Specialize std::swap for IOBuf
#if __cplusplus < 201103L
#include <algorithm>
#else
#include <utility>
#endif
namespace std
{
    template <>
    inline void swap(fast::IOBuf &a, fast::IOBuf &b)
    {
        return a.swap(b);
    }
} // namespace std

#endif
