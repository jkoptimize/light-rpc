// fast_iobuf_inl.h has NO include guard. fast_iobuf.cc explicitly includes
// this file (in addition to fast_iobuf.h) to ensure both are always processed.
// fast_iobuf.h also includes this file at the end, but the guard on fast_iobuf.h
// prevents double-processing. With this file having no guard, fast_iobuf.h can
// safely include it without triggering the fast_iobuf_inl_h -> fast_iobuf.h -> fast_iobuf_inl_h
// circular include issue.
#include <atomic>
#include "fast_log.h"

namespace fast
{

    void blockmem_deallocate(void *mem);

    using UserDataDeleter = std::function<void(void *)>;

    struct UserDataExtension
    {
        UserDataDeleter deleter;
    };

    inline void reset_block_ref(fast::IOBuf::BlockRef &ref)
    {
        ref = {};
    }

    inline IOBuf::IOBuf()
    {
        reset_block_ref(_sv.refs[0]);
        reset_block_ref(_sv.refs[1]);
    }

    inline IOBuf::IOBuf(const Movable &rhs)
    {
        _sv = rhs.value()._sv;
        new (&rhs.value()) IOBuf;
    }

    inline void IOBuf::operator=(const Movable &rhs)
    {
        clear();
        _sv = rhs.value()._sv;
        new (&rhs.value()) IOBuf;
    }

    inline void IOBuf::operator=(const char *s)
    {
        clear();
        append(s);
    }

    inline void IOBuf::operator=(const std::string &s)
    {
        clear();
        append(s);
    }

    inline void IOBuf::swap(IOBuf &other)
    {
        const SmallView tmp = other._sv;
        other._sv = _sv;
        _sv = tmp;
    }

    inline bool IOBuf::empty() const
    {
        return _small() ? !_sv.refs[0].block : !_bv.nbytes;
    }

    inline size_t IOBuf::length() const
    {
        return _small() ? (_sv.refs[0].length + _sv.refs[1].length) : _bv.nbytes;
    }

    inline bool IOBuf::_small() const
    {
        return _bv.magic >= 0;
    }

    inline int IOBuf::append(const std::string &s)
    {
        return append(s.data(), s.length());
    }

    inline void IOBuf::_push_back_ref(const BlockRef &r)
    {
        if (_small())
            return _push_or_move_back_ref_to_smallview<false>(r);
        return _push_or_move_back_ref_to_bigview<false>(r);
    }

    inline void IOBuf::_move_back_ref(const BlockRef &r)
    {
        if (_small())
            return _push_or_move_back_ref_to_smallview<true>(r);
        return _push_or_move_back_ref_to_bigview<true>(r);
    }

    inline size_t IOBuf::_ref_num() const
    {
        return _small() ? (!!_sv.refs[0].block + !!_sv.refs[1].block) : _bv.nref;
    }

    inline IOBuf::BlockRef &IOBuf::_front_ref()
    {
        return _small() ? _sv.refs[0] : _bv.refs[_bv.start];
    }

    inline const IOBuf::BlockRef &IOBuf::_front_ref() const
    {
        return _small() ? _sv.refs[0] : _bv.refs[_bv.start];
    }

    inline IOBuf::BlockRef &IOBuf::_back_ref()
    {
        return _small() ? _sv.refs[!!_sv.refs[1].block] : _bv.ref_at(_bv.nref - 1);
    }

    inline const IOBuf::BlockRef &IOBuf::_back_ref() const
    {
        return _small() ? _sv.refs[!!_sv.refs[1].block] : _bv.ref_at(_bv.nref - 1);
    }

    inline IOBuf::BlockRef &IOBuf::_ref_at(size_t i)
    {
        return _small() ? _sv.refs[i] : _bv.ref_at(i);
    }

    inline const IOBuf::BlockRef &IOBuf::_ref_at(size_t i) const
    {
        return _small() ? _sv.refs[i] : _bv.ref_at(i);
    }

    inline const IOBuf::BlockRef *IOBuf::_pref_at(size_t i) const
    {
        if (_small())
            return i < (size_t)(!!_sv.refs[0].block + !!_sv.refs[1].block) ? &_sv.refs[i] : NULL;
        return i < _bv.nref ? &_bv.ref_at(i) : NULL;
    }

    struct IOBuf::Block
    {
        std::atomic<int> nshared;
        uint16_t flags;
        uint16_t abi_check;
        uint32_t size;
        uint32_t cap;
        union
        {
            Block *portal_next;
            uint64_t data_meta;
        } u;
        char *data;

        Block(char *data_in, uint32_t data_size)
            : nshared(1), flags(0), abi_check(0), size(0), cap(data_size), u({NULL}), data(data_in)
        {
        }

        UserDataExtension *get_user_data_extension()
        {
            char *p = (char *)this;
            return (UserDataExtension *)(p + sizeof(Block));
        }

        inline void check_abi()
        {
#ifndef NDEBUG
            if (abi_check != 0)
            {
                LOG(FATAL) << "ABI check failed";
            }
#endif
        }

        void inc_ref()
        {
            check_abi();
            nshared.fetch_add(1, std::memory_order_relaxed);
        }

        void dec_ref()
        {
            check_abi();
            if (nshared.fetch_sub(1, std::memory_order_release) == 1)
            {
                std::atomic_thread_fence(std::memory_order_acquire);
                this->~Block();
                fast::blockmem_deallocate(this);
            }
        }

        int ref_count() const { return nshared.load(std::memory_order_relaxed); }
        bool full() const { return size >= cap; }
        size_t left_space() const { return cap - size; }
    };

}; // namespace fast