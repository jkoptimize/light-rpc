#pragma once

#include <functional>
#include "fast_iobuf.h"

namespace fast {

enum class DispatcherMode { kServer, kClient };

// Cuts complete frames from read_buf and dispatches each to a handler.
// Frame format: [ total_len: fixed32, 4B, big-endian ] [ payload... ]
// In kClient mode, total_len is at frame offset 0 (response frame).
// Handler runs in a detached thread for each complete frame.

class MessageDispatcher {
public:
    using MessageHandler = std::function<int(IOBuf& frame, void* arg)>;

    void SetHandler(MessageHandler handler, void* arg);
    void SetMode(DispatcherMode mode) { _mode = mode; }

    // Called when new data arrives in read_buf.
    // Returns number of frames dispatched, -1 on error.
    int ProcessNewMessage(IOBuf& read_buf);

    // Cut one complete frame from read_buf.  Returns true on success.
    bool CutInputMessage(IOBuf& read_buf, IOBuf& frame);

    MessageHandler  _handler = nullptr;
    void*           _arg     = nullptr;
    DispatcherMode  _mode    = DispatcherMode::kServer;
};

}  // namespace fast
