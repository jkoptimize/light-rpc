#pragma once

#include "inc/fast_log.h"

#include <unordered_map>
#include <mutex>
#include <vector>
#include <functional>

namespace fast
{

  // vType should be pointer type or value type.
  template <typename vType>
  class SafeHashMap
  {
  public:
    SafeHashMap() = default;
    ~SafeHashMap() = default;
    void SafeInsert(uint32_t key, vType val)
    {
      std::lock_guard<std::mutex> lk(mtx_);
      hashmap_.emplace(key, val);
    }

    vType SafeGet(uint32_t key)
    {
      std::lock_guard<std::mutex> lk(mtx_);
      auto it = hashmap_.find(key);
      CHECK(it != hashmap_.end());
      return it->second;
    }

    void SafeErase(uint32_t key)
    {
      std::lock_guard<std::mutex> lk(mtx_);
      CHECK(hashmap_.count(key));
      hashmap_.erase(key);
    }

    vType SafeGetAndErase(uint32_t key)
    {
      std::lock_guard<std::mutex> lk(mtx_);
      auto it = hashmap_.find(key);
      CHECK(it != hashmap_.end());
      vType val = it->second;
      hashmap_.erase(it);
      return val;
    }

  private:
    std::mutex mtx_;
    std::unordered_map<uint32_t, vType> hashmap_;
  };

  // Forward declarations for ScopeGuard and MakeScopeGuard
  template <typename Callback>
  class ScopeGuard;

  template <typename Callback>
  ScopeGuard<Callback> MakeScopeGuard(Callback &&callback) noexcept;

  // ScopeGuard is a simple implementation to guarantee that
  // a function is executed upon leaving the current scope.
  template <typename Callback>
  class ScopeGuard
  {
  public:
    ScopeGuard(ScopeGuard &&other) noexcept
        : _callback(std::move(other._callback)), _dismiss(other._dismiss)
    {
      other.dismiss();
    }

    ~ScopeGuard() noexcept
    {
      if (!_dismiss)
      {
        _callback();
      }
    }

    void dismiss() noexcept
    {
      _dismiss = true;
    }

    ScopeGuard() = delete;
    ScopeGuard(const ScopeGuard &) = delete;
    ScopeGuard &operator=(const ScopeGuard &) = delete;
    ScopeGuard &operator=(ScopeGuard &&) = delete;

  private:
    // Only MakeScopeGuard and move constructor can create ScopeGuard.
    friend ScopeGuard<Callback> MakeScopeGuard<Callback>(Callback &&callback) noexcept;

    explicit ScopeGuard(Callback &&callback) noexcept
        : _callback(std::forward<Callback>(callback)), _dismiss(false) {}

  private:
    Callback _callback;
    bool _dismiss;
  };

  // The MakeScopeGuard() function is used to create a new ScopeGuard object.
  // It can be instantiated with a lambda function, a std::function<void()>,
  // a functor, or a void(*)() function pointer.
  template <typename Callback>
  ScopeGuard<Callback> MakeScopeGuard(Callback &&callback) noexcept
  {
    return ScopeGuard<Callback>{std::forward<Callback>(callback)};
  }

  class ThreadExitHelper
  {
  public:
    using Callback = std::function<void()>;

    // 注册一个回调，在当前线程退出时执行
    static void add_callback(Callback cb)
    {
      instance().callbacks.push_back(std::move(cb));
    }

  private:
    ThreadExitHelper() = default;
    ~ThreadExitHelper()
    {
      // 逆序调用注册的回调
      for (auto it = callbacks.rbegin(); it != callbacks.rend(); ++it)
      {
        (*it)();
      }
    }

    // 获取当前线程的唯一实例
    static ThreadExitHelper &instance()
    {
      thread_local ThreadExitHelper helper;
      return helper;
    }

    std::vector<Callback> callbacks;
  };

} // namespace fast