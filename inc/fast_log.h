#pragma once

#include <error.h>
#include <stdio.h>
#include <stdlib.h>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <memory>

namespace fast
{

#define LOG_INFO(M, ...) \
  fprintf(stderr, "[INFO] (%s:%d) " M "\n", __FILE__, __LINE__, ##__VA_ARGS__)

#define LOG_ERR(M, ...)                          \
  fprintf(stderr,                                \
          "[ERROR] (%s:%d: errno: %s) " M "\n",  \
          __FILE__,                              \
          __LINE__,                              \
          errno == 0 ? "None" : strerror(errno), \
          ##__VA_ARGS__)

#define CHECK(COND)                        \
  do                                       \
  {                                        \
    if (!(COND))                           \
    {                                      \
      LOG_ERR("Check failure: %s", #COND); \
      exit(EXIT_FAILURE);                  \
    }                                      \
  } while (0);

  enum LogLevel
  {
    INFO,
    ERROR,
    FATAL
  };

  // 日志流类，负责收集日志内容并在析构时输出
  class LogMessage
  {
  public:
    explicit LogMessage(LogLevel level) : level_(level) {}
    ~LogMessage()
    {
      std::cerr << stream_.str() << std::endl;
    }

    // 模板化的 operator<<，支持任意类型，实现流式拼接
    template <typename T>
    LogMessage &operator<<(const T &value)
    {
      stream_ << value;
      return *this;
    }

    // 针对 std::ostream 操纵符（如 std::endl）的重载
    LogMessage &operator<<(std::ostream &(*manip)(std::ostream &))
    {
      manip(stream_);
      return *this;
    }

  private:
    LogLevel level_;
    std::ostringstream stream_;

    // 禁止拷贝，防止临时对象析构问题
    LogMessage(const LogMessage &) = delete;
    LogMessage &operator=(const LogMessage &) = delete;
  };

  // 代理类，用于 LOG_IF
  class LogMessageProxy
  {
  public:
    LogMessageProxy(LogLevel level, bool condition)
    {
      if (condition)
      {
        msg_ = std::make_unique<LogMessage>(level);
      }
    }

    // 转发所有 << 操作到内部的 LogMessage（如果存在）
    template <typename T>
    LogMessageProxy &operator<<(const T &val)
    {
      if (msg_)
        *msg_ << val;
      return *this;
    }

    LogMessageProxy &operator<<(std::ostream &(*manip)(std::ostream &))
    {
      if (msg_)
        *msg_ << manip;
      return *this;
    }

  private:
    std::unique_ptr<LogMessage> msg_;
    LogMessageProxy(const LogMessageProxy &) = delete;
    LogMessageProxy &operator=(const LogMessageProxy &) = delete;
  };

// 定义 LOG 宏，展开为创建 LogMessage 临时对象
#define LOG(level) LogMessage(level)
#define LOG_IF(level, condition) LogMessageProxy(level, condition)

} // namespace fast