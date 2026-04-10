#include <sstream>
#include "fast_iobuf.h"
#include "fast_block_pool.h"

namespace fast
{
    IOBuf::IOBuf(const IOBuf &rhs)
    {
        if (rhs._small())
        {
            _sv = rhs._sv;
            if (_sv.refs[0].block)
            {
                _sv.refs[0].block->inc_ref();
            }
            if (_sv.refs[1].block)
            {
                _sv.refs[1].block->inc_ref();
            }
        }
        else
        {
            _bv.magic = -1;
            _bv.start = 0;
            _bv.nref = rhs._bv.nref;
            _bv.cap_mask = rhs._bv.cap_mask;
            _bv.nbytes = rhs._bv.nbytes;
            _bv.refs = new IOBuf::BlockRef[_bv.capacity()];
            for (size_t i = 0; i < _bv.nref; ++i)
            {
                _bv.refs[i] = rhs._bv.ref_at(i);
                _bv.refs[i].block->inc_ref();
            }
        }
    }

    void IOBuf::operator=(const IOBuf &rhs)
    {
        if (this == &rhs)
        {
            return;
        }
        if (!rhs._small() && !_small() && _bv.cap_mask == rhs._bv.cap_mask) // 如果内存等大，直接复用，减少内存分配释放开销
        {
            // Reuse array of refs
            // Remove references to previous blocks.
            for (size_t i = 0; i < _bv.nref; ++i)
            {
                _bv.ref_at(i).block->dec_ref();
            }
            // References blocks in rhs.
            _bv.start = 0;
            _bv.nref = rhs._bv.nref;
            _bv.nbytes = rhs._bv.nbytes;
            for (size_t i = 0; i < _bv.nref; ++i)
            {
                _bv.refs[i] = rhs._bv.ref_at(i);
                _bv.refs[i].block->inc_ref();
            }
        }
        else
        {
            this->~IOBuf();
            new (this) IOBuf(rhs);
        }
    }

    void IOBuf::clear()
    {
        if (_small())
        {
            if (_sv.refs[0].block != NULL)
            {
                _sv.refs[0].block->dec_ref();
                reset_block_ref(_sv.refs[0]);

                if (_sv.refs[1].block != NULL)
                {
                    _sv.refs[1].block->dec_ref();
                    reset_block_ref(_sv.refs[1]);
                }
            }
        }
        else
        {
            for (uint32_t i = 0; i < _bv.nref; ++i)
            {
                _bv.ref_at(i).block->dec_ref();
            }
            fast::release_blockref_array(_bv.refs, _bv.capacity());
            new (this) IOBuf;
        }
    }

    size_t IOBuf::pop_front(size_t n)
    {
        const size_t len = length();
        if (n >= len)
        {
            clear();
            return len;
        }
        const size_t saved_n = n;
        while (n)
        { // length() == 0 does not enter
            IOBuf::BlockRef &r = _front_ref();
            if (r.length > n)
            {
                r.offset += n;
                r.length -= n;
                if (!_small())
                {
                    _bv.nbytes -= n;
                }
                return saved_n;
            }
            n -= r.length;
            _pop_front_ref();
        }
        return saved_n;
    }

    size_t IOBuf::pop_back(size_t n)
    {
        const size_t len = length();
        if (n >= len)
        {
            clear();
            return len;
        }
        const size_t saved_n = n;
        while (n)
        { // length() == 0 does not enter
            IOBuf::BlockRef &r = _back_ref();
            if (r.length > n)
            {
                r.length -= n;
                if (!_small())
                {
                    _bv.nbytes -= n;
                }
                return saved_n;
            }
            n -= r.length;
            _pop_back_ref();
        }
        return saved_n;
    }

    size_t IOBuf::cutn(IOBuf *out, size_t n)
    {
        const size_t len = length();
        if (n > len)
        {
            n = len;
        }
        const size_t saved_n = n;
        while (n)
        { // length() == 0 does not enter
            IOBuf::BlockRef &r = _front_ref();
            if (r.length <= n)
            {
                n -= r.length;
                out->_move_back_ref(r);
                _moveout_front_ref();
            }
            else
            {
                const IOBuf::BlockRef cr = {r.offset, (uint32_t)n, r.block};
                out->_push_back_ref(cr);

                r.offset += n;
                r.length -= n;
                if (!_small())
                {
                    _bv.nbytes -= n;
                }
                return saved_n;
            }
        }
        return saved_n;
    }

    size_t IOBuf::cutn(void *out, size_t n)
    {
        const size_t len = length();
        if (n > len)
        {
            n = len;
        }
        const size_t saved_n = n;
        while (n)
        { // length() == 0 does not enter
            IOBuf::BlockRef &r = _front_ref();
            if (r.length <= n)
            {
                memcpy(out, r.block->data + r.offset, r.length);
                out = (char *)out + r.length;
                n -= r.length;
                _pop_front_ref();
            }
            else
            {
                memcpy(out, r.block->data + r.offset, n);
                out = (char *)out + n;
                r.offset += n;
                r.length -= n;
                if (!_small())
                {
                    _bv.nbytes -= n;
                }
                return saved_n;
            }
        }
        return saved_n;
    }

    size_t IOBuf::cutn(std::string *out, size_t n)
    {
        if (n == 0)
        {
            return 0;
        }
        const size_t len = length();
        if (n > len)
        {
            n = len;
        }
        const size_t old_size = out->size();
        out->resize(out->size() + n);
        return cutn(&(*out)[old_size], n);
    }

    bool IOBuf::cut1(void *c)
    {
        if (empty())
        {
            return false;
        }
        IOBuf::BlockRef &r = _front_ref();
        *(char *)c = r.block->data[r.offset];
        if (r.length > 1)
        {
            ++r.offset;
            --r.length;
            if (!_small())
            {
                --_bv.nbytes;
            }
        }
        else
        {
            _pop_front_ref();
        }
        return true;
    }

    void IOBuf::append(const IOBuf &other)
    {
        const size_t nref = other._ref_num();
        for (size_t i = 0; i < nref; ++i)
        {
            _push_back_ref(other._ref_at(i));
        }
    }

    void IOBuf::append(const Movable &movable_other)
    {
        if (empty())
        {
            swap(movable_other.value());
        }
        else
        {
            fast::IOBuf &other = movable_other.value();
            const size_t nref = other._ref_num();
            for (size_t i = 0; i < nref; ++i)
            {
                _move_back_ref(other._ref_at(i));
            }
            if (!other._small())
            {
                fast::release_blockref_array(other._bv.refs, other._bv.capacity());
            }
            new (&other) IOBuf;
        }
    }

    int IOBuf::push_back(char c)
    {
        IOBuf::Block *b = fast::share_tls_block();
        if (!b)
        {
            return -1;
        }
        b->data[b->size] = c;
        const IOBuf::BlockRef r = {b->size, 1, b};
        ++b->size;
        _push_back_ref(r);
        return 0;
    }

    int IOBuf::append(char const *s)
    {
        if (s != NULL)
        {
            return append(s, strlen(s));
        }
        return -1;
    }

    int IOBuf::append(void const *data, size_t count)
    {
        if (!data)
        {
            return -1;
        }
        if (count == 1)
        {
            return push_back(*((char const *)data));
        }
        size_t total_nc = 0;
        while (total_nc < count)
        { // excluded count == 0
            IOBuf::Block *b = fast::share_tls_block();
            if (!b)
            {
                return -1;
            }
            const size_t nc = std::min(count - total_nc, b->left_space());
            memcpy(b->data + b->size, (char *)data + total_nc, nc);

            const IOBuf::BlockRef r = {(uint32_t)b->size, (uint32_t)nc, b};
            _push_back_ref(r);
            b->size += nc;
            total_nc += nc;
        }
        return 0;
    }

    int IOBuf::resize(size_t n, char c)
    {
        const size_t saved_len = length();
        if (n < saved_len)
        {
            pop_back(saved_len - n);
            return 0;
        }
        const size_t count = n - saved_len;
        size_t total_nc = 0;
        while (total_nc < count)
        { // excluded count == 0
            IOBuf::Block *b = fast::share_tls_block();
            if (!b)
            {
                return -1;
            }
            const size_t nc = std::min(count - total_nc, b->left_space());
            memset(b->data + b->size, c, nc);

            const IOBuf::BlockRef r = {(uint32_t)b->size, (uint32_t)nc, b};
            _push_back_ref(r);
            b->size += nc;
            total_nc += nc;
        }
        return 0;
    }

    bool IOBuf::equals(const fast::IOBuf &other) const
    {
        const size_t sz1 = size();
        if (sz1 != other.size())
        {
            return false;
        }
        if (!sz1)
        {
            return true;
        }
        const BlockRef &r1 = _ref_at(0);
        const char *d1 = r1.block->data + r1.offset;
        size_t len1 = r1.length;
        const BlockRef &r2 = other._ref_at(0);
        const char *d2 = r2.block->data + r2.offset;
        size_t len2 = r2.length;
        const size_t nref1 = _ref_num();
        const size_t nref2 = other._ref_num();
        size_t i = 1;
        size_t j = 1;
        do
        {
            const size_t cmplen = std::min(len1, len2);
            if (memcmp(d1, d2, cmplen) != 0)
            {
                return false;
            }
            len1 -= cmplen;
            if (!len1)
            {
                if (i >= nref1)
                {
                    return true;
                }
                const BlockRef &r = _ref_at(i++);
                d1 = r.block->data + r.offset;
                len1 = r.length;
            }
            else
            {
                d1 += cmplen;
            }
            len2 -= cmplen;
            if (!len2)
            {
                if (j >= nref2)
                {
                    return true;
                }
                const BlockRef &r = other._ref_at(j++);
                d2 = r.block->data + r.offset;
                len2 = r.length;
            }
            else
            {
                d2 += cmplen;
            }
        } while (true);
        return true;
    }

    template <bool MOVE>
    void IOBuf::_push_or_move_back_ref_to_smallview(const BlockRef &r)
    {
        BlockRef *const refs = _sv.refs;
        if (NULL == refs[0].block)
        {
            refs[0] = r;
            if (!MOVE)
            {
                r.block->inc_ref();
            }
            return;
        }
        if (NULL == refs[1].block)
        {
            if (refs[0].block == r.block &&
                refs[0].offset + refs[0].length == r.offset)
            { // Merge ref
                refs[0].length += r.length;
                if (MOVE)
                {
                    r.block->dec_ref();
                }
                return;
            }
            refs[1] = r;
            if (!MOVE)
            {
                r.block->inc_ref();
            }
            return;
        }
        if (refs[1].block == r.block &&
            refs[1].offset + refs[1].length == r.offset)
        { // Merge ref
            refs[1].length += r.length;
            if (MOVE)
            {
                r.block->dec_ref();
            }
            return;
        }
        // Convert to BigView
        BlockRef *new_refs = fast::acquire_blockref_array();
        new_refs[0] = refs[0];
        new_refs[1] = refs[1];
        new_refs[2] = r;
        const size_t new_nbytes = refs[0].length + refs[1].length + r.length;
        if (!MOVE)
        {
            r.block->inc_ref();
        }
        _bv.magic = -1;
        _bv.start = 0;
        _bv.refs = new_refs;
        _bv.nref = 3;
        _bv.cap_mask = INITIAL_CAP - 1;
        _bv.nbytes = new_nbytes;
    }
    // Explicitly initialize templates.
    template void IOBuf::_push_or_move_back_ref_to_smallview<true>(const BlockRef &);
    template void IOBuf::_push_or_move_back_ref_to_smallview<false>(const BlockRef &);

    template <bool MOVE>
    void IOBuf::_push_or_move_back_ref_to_bigview(const BlockRef &r)
    {
        BlockRef &back = _bv.ref_at(_bv.nref - 1);
        if (back.block == r.block && back.offset + back.length == r.offset)
        {
            // Merge ref
            back.length += r.length;
            _bv.nbytes += r.length;
            if (MOVE)
            {
                r.block->dec_ref();
            }
            return;
        }

        if (_bv.nref != _bv.capacity())
        {
            _bv.ref_at(_bv.nref++) = r;
            _bv.nbytes += r.length;
            if (!MOVE)
            {
                r.block->inc_ref();
            }
            return;
        }
        // resize, don't modify bv until new_refs is fully assigned
        const uint32_t new_cap = _bv.capacity() * 2;
        BlockRef *new_refs = fast::acquire_blockref_array(new_cap);
        for (uint32_t i = 0; i < _bv.nref; ++i)
        {
            new_refs[i] = _bv.ref_at(i);
        }
        new_refs[_bv.nref++] = r;

        // Change other variables
        _bv.start = 0;
        fast::release_blockref_array(_bv.refs, _bv.capacity());
        _bv.refs = new_refs;
        _bv.cap_mask = new_cap - 1;
        _bv.nbytes += r.length;
        if (!MOVE)
        {
            r.block->inc_ref();
        }
    }
    // Explicitly initialize templates.
    template void IOBuf::_push_or_move_back_ref_to_bigview<true>(const BlockRef &);
    template void IOBuf::_push_or_move_back_ref_to_bigview<false>(const BlockRef &);

    template <bool MOVEOUT>
    int IOBuf::_pop_or_moveout_front_ref()
    {
        if (_small())
        {
            if (_sv.refs[0].block != NULL)
            {
                if (!MOVEOUT)
                {
                    _sv.refs[0].block->dec_ref();
                }
                _sv.refs[0] = _sv.refs[1];
                reset_block_ref(_sv.refs[1]);
                return 0;
            }
            return -1;
        }
        else
        {
            // _bv.nref must be greater than 2
            const uint32_t start = _bv.start;
            if (!MOVEOUT)
            {
                _bv.refs[start].block->dec_ref();
            }
            if (--_bv.nref > 2)
            {
                _bv.start = (start + 1) & _bv.cap_mask;
                _bv.nbytes -= _bv.refs[start].length;
            }
            else
            { // count==2, fall back to SmallView
                BlockRef *const saved_refs = _bv.refs;
                const uint32_t saved_cap_mask = _bv.cap_mask;
                _sv.refs[0] = saved_refs[(start + 1) & saved_cap_mask];
                _sv.refs[1] = saved_refs[(start + 2) & saved_cap_mask];
                fast::release_blockref_array(saved_refs, saved_cap_mask + 1);
            }
            return 0;
        }
    }
    // Explicitly initialize templates.
    template int IOBuf::_pop_or_moveout_front_ref<true>();
    template int IOBuf::_pop_or_moveout_front_ref<false>();

    int IOBuf::_pop_back_ref()
    {
        if (_small())
        {
            if (_sv.refs[1].block != NULL)
            {
                _sv.refs[1].block->dec_ref();
                reset_block_ref(_sv.refs[1]);
                return 0;
            }
            else if (_sv.refs[0].block != NULL)
            {
                _sv.refs[0].block->dec_ref();
                reset_block_ref(_sv.refs[0]);
                return 0;
            }
            return -1;
        }
        else
        {
            // _bv.nref must be greater than 2
            const uint32_t start = _bv.start;
            IOBuf::BlockRef &back = _bv.refs[(start + _bv.nref - 1) & _bv.cap_mask];
            back.block->dec_ref();
            if (--_bv.nref > 2)
            {
                _bv.nbytes -= back.length;
            }
            else
            { // count==2, fall back to SmallView
                BlockRef *const saved_refs = _bv.refs;
                const uint32_t saved_cap_mask = _bv.cap_mask;
                _sv.refs[0] = saved_refs[start];
                _sv.refs[1] = saved_refs[(start + 1) & saved_cap_mask];
                fast::release_blockref_array(saved_refs, saved_cap_mask + 1);
            }
            return 0;
        }
    }

    inline IOBuf::BlockRef *acquire_blockref_array(size_t cap)
    {
        return new IOBuf::BlockRef[cap];
    }

    inline IOBuf::BlockRef *acquire_blockref_array()
    {
        return acquire_blockref_array(IOBuf::INITIAL_CAP);
    }

    inline void release_blockref_array(IOBuf::BlockRef *refs, size_t cap)
    {
        delete[] refs;
    }

    // ZeroCopyInputStream 用于作数据输入（eg:收到的网络IO数据->应用层数据），避免了中间拷贝
    // 构造函数入参表示输入源，应用层反序列化时通过Next\backup\Skip等接口访问和控制输入源数据
    IOBufAsZeroCopyInputStream::IOBufAsZeroCopyInputStream(const IOBuf &buf)
        : _ref_index(0), _add_offset(0), _byte_count(0), _buf(&buf)
    {
    }

    bool IOBufAsZeroCopyInputStream::Next(const void **data, int *size)
    {
        const IOBuf::BlockRef *cur_ref = _buf->_pref_at(_ref_index);
        if (cur_ref == NULL)
        {
            return false;
        }
        *data = cur_ref->block->data + cur_ref->offset + _add_offset;
        // Impl. of Backup/Skip guarantees that _add_offset < cur_ref->length.
        *size = cur_ref->length - _add_offset;
        _byte_count += cur_ref->length - _add_offset;
        _add_offset = 0;
        ++_ref_index;
        return true;
    }

    void IOBufAsZeroCopyInputStream::BackUp(int count)
    {
        if (_ref_index > 0)
        {
            const IOBuf::BlockRef *cur_ref = _buf->_pref_at(--_ref_index);
            LOG_IF(FATAL, _add_offset == 0 && cur_ref->length >= (uint32_t)count)
                << "BackUp() is not after a Next()";
            _add_offset = cur_ref->length - count;
            _byte_count -= count;
        }
        else
        {
            LOG(FATAL) << "BackUp an empty ZeroCopyInputStream";
        }
    }

    // Skips a number of bytes.  Returns false if the end of the stream is
    // reached or some input error occurred.  In the end-of-stream case, the
    // stream is advanced to the end of the stream (so ByteCount() will return
    // the total size of the stream).
    bool IOBufAsZeroCopyInputStream::Skip(int count)
    {
        const IOBuf::BlockRef *cur_ref = _buf->_pref_at(_ref_index);
        while (cur_ref)
        {
            const int left_bytes = cur_ref->length - _add_offset;
            if (count < left_bytes)
            {
                _add_offset += count;
                _byte_count += count;
                return true;
            }
            count -= left_bytes;
            _add_offset = 0;
            _byte_count += left_bytes;
            cur_ref = _buf->_pref_at(++_ref_index);
        }
        return false;
    }

    int64_t IOBufAsZeroCopyInputStream::ByteCount() const
    {
        return _byte_count;
    }

    IOBufAsZeroCopyOutputStream::IOBufAsZeroCopyOutputStream(IOBuf *buf)
        : _buf(buf), _block_size(0), _cur_block(NULL), _byte_count(0)
    {
    }

    IOBufAsZeroCopyOutputStream::IOBufAsZeroCopyOutputStream(
        IOBuf *buf, uint32_t block_size)
        : _buf(buf), _block_size(block_size), _cur_block(NULL), _byte_count(0)
    {
        if (_block_size <= offsetof(IOBuf::Block, data))
        {
            throw std::invalid_argument("block_size is too small");
        }
    }

    IOBufAsZeroCopyOutputStream::~IOBufAsZeroCopyOutputStream()
    {
        _release_block();
    }

    int64_t IOBufAsZeroCopyOutputStream::ByteCount() const
    {
        return _byte_count;
    }

    void IOBufAsZeroCopyOutputStream::_release_block()
    {
        if (_block_size > 0)
        {
            if (_cur_block)
            {
                _cur_block->dec_ref();
            }
        }
        else
        {
            fast::release_tls_block(_cur_block);
        }
        _cur_block = NULL;
    }

    bool IOBufAsZeroCopyOutputStream::Next(void **data, int *size)
    {
        if (_cur_block == NULL || _cur_block->full())
        {
            _release_block();
            if (_block_size > 0)
            {
                _cur_block = fast::create_block(_block_size);
            }
            else
            {
                _cur_block = fast::acquire_tls_block();
            }
            if (_cur_block == NULL)
            {
                return false;
            }
        }
        const IOBuf::BlockRef r = {_cur_block->size,
                                   (uint32_t)_cur_block->left_space(),
                                   _cur_block};
        *data = _cur_block->data + r.offset;
        *size = r.length;
        _cur_block->size = _cur_block->cap;
        _buf->_push_back_ref(r);
        _byte_count += r.length;
        return true;
    }

    void IOBufAsZeroCopyOutputStream::BackUp(int count)
    {
        while (!_buf->empty())
        {
            IOBuf::BlockRef &r = _buf->_back_ref();
            if (_cur_block)
            {
                // A ordinary BackUp that should be supported by all ZeroCopyOutputStream
                // _cur_block must match end of the IOBuf
                if (r.block != _cur_block)
                {
                    LOG(FATAL) << "r.block=" << r.block
                               << " does not match _cur_block=" << _cur_block;
                    return;
                }
                if (r.offset + r.length != _cur_block->size)
                {
                    LOG(FATAL) << "r.offset(" << r.offset << ") + r.length("
                               << r.length << ") != _cur_block->size("
                               << _cur_block->size << ")";
                    return;
                }
            }
            else
            {
                // An extended BackUp which is undefined in regular
                // ZeroCopyOutputStream. The `count' given by user is larger than
                // size of last _cur_block (already released in last iteration).
                if (r.block->ref_count() == 1)
                {
                    // A special case: the block is only referenced by last
                    // BlockRef of _buf. Safe to allocate more on the block.
                    if (r.offset + r.length != r.block->size)
                    {
                        LOG(FATAL) << "r.offset(" << r.offset << ") + r.length("
                                   << r.length << ") != r.block->size("
                                   << r.block->size << ")";
                        return;
                    }
                }
                else if (r.offset + r.length != r.block->size)
                {
                    // Last BlockRef does not match end of the block (which is
                    // used by other IOBuf already). Unsafe to re-reference
                    // the block and allocate more, just pop the bytes.
                    _byte_count -= _buf->pop_back(count);
                    return;
                } // else Last BlockRef matches end of the block. Even if the
                // block is shared by other IOBuf, it's safe to allocate bytes
                // after block->size.
                _cur_block = r.block;
                _cur_block->inc_ref();
            }
            if (r.length > (uint32_t)count)
            {
                r.length -= count;
                if (!_buf->_small())
                {
                    _buf->_bv.nbytes -= count;
                }
                _cur_block->size -= count;
                _byte_count -= count;
                // Release block for TLS before quiting BackUp() for other
                // code to reuse the block even if this wrapper object is
                // not destructed. Example:
                //    IOBufAsZeroCopyOutputStream wrapper(...);
                //    ParseFromZeroCopyStream(&wrapper, ...); // Calls BackUp
                //    IOBuf buf;
                //    buf.append("foobar");  // can reuse the TLS block.
                if (_block_size == 0)
                {
                    fast::release_tls_block(_cur_block);
                    _cur_block = NULL;
                }
                return;
            }
            _cur_block->size -= r.length;
            _byte_count -= r.length;
            count -= r.length;
            _buf->_pop_back_ref();
            _release_block();
            if (count == 0)
            {
                return;
            }
        }
        LOG_IF(FATAL, count != 0) << "BackUp an empty IOBuf";
    }

    // === TLS block management ===

    namespace
    {
        const int MAX_BLOCKS_PER_THREAD = 8;

        struct TLSData
        {
            IOBuf::Block *block_head;
            size_t num_blocks;
            bool registered;
        };

        inline TLSData &tls_data()
        {
            static thread_local TLSData data = {NULL, 0, false};
            return data;
        }

        void clear_tls_block_chain()
        {
            TLSData &tls = tls_data();
            IOBuf::Block *b = tls.block_head;
            tls.block_head = NULL;
            while (b)
            {
                IOBuf::Block *next = b->u.portal_next;
                b->dec_ref();
                b = next;
            }
            tls.num_blocks = 0;
        }
    } // anonymous namespace

    void blockmem_deallocate(void *mem)
    {
        BlockDeallocate(mem);
    }

    inline IOBuf::Block *create_block(const size_t block_size)
    {
        if (block_size > 0xFFFFFFFFULL)
        {
            LOG(FATAL) << "block_size=" << block_size << " is too large";
            return NULL;
        }
        char *mem = (char *)fast::BlockAllocate(block_size);
        if (mem == NULL)
        {
            return NULL;
        }
        return new (mem) IOBuf::Block(mem + sizeof(IOBuf::Block),
                                      block_size - sizeof(IOBuf::Block));
    }

    inline IOBuf::Block *create_block()
    {
        return create_block(IOBuf::DEFAULT_BLOCK_SIZE);
    }

    IOBuf::Block *share_tls_block()
    {
        TLSData &tls = tls_data();
        IOBuf::Block *b = tls.block_head;
        if (b != NULL && !b->full())
        {
            return b;
        }
        IOBuf::Block *new_block = NULL;
        if (b)
        {
            new_block = b;
            while (new_block && new_block->full())
            {
                IOBuf::Block *saved_next = new_block->u.portal_next;
                new_block->dec_ref();
                --tls.num_blocks;
                new_block = saved_next;
            }
        }
        else if (!tls.registered)
        {
            tls.registered = true;
            ThreadExitHelper::add_callback([]()
                                           { clear_tls_block_chain(); });
        }
        if (!new_block)
        {
            new_block = create_block();
            if (new_block)
            {
                ++tls.num_blocks;
            }
        }
        tls.block_head = new_block;
        return new_block;
    }

    void release_tls_block(IOBuf::Block *b)
    {
        if (!b)
        {
            return;
        }
        TLSData &tls = tls_data();
        if (b->full())
        {
            b->dec_ref();
        }
        else if (tls.num_blocks >= MAX_BLOCKS_PER_THREAD)
        {
            b->dec_ref();
        }
        else
        {
            b->u.portal_next = tls.block_head;
            tls.block_head = b;
            ++tls.num_blocks;
            if (!tls.registered)
            {
                tls.registered = true;
                ThreadExitHelper::add_callback([]()
                                               { clear_tls_block_chain(); });
            }
        }
    }

    IOBuf::Block *acquire_tls_block()
    {
        TLSData &tls = tls_data();
        IOBuf::Block *b = tls.block_head;
        if (!b)
        {
            return create_block();
        }
        while (b->full())
        {
            IOBuf::Block *saved_next = b->u.portal_next;
            b->dec_ref();
            tls.block_head = saved_next;
            --tls.num_blocks;
            b = saved_next;
        }
        if (b)
        {
            tls.block_head = b->u.portal_next;
            b->u.portal_next = NULL;
            --tls.num_blocks;
        }
        return b;
    }

} // namespace fast
