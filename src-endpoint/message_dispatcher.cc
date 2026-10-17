#include <arpa/inet.h>
#include <algorithm>
#include <cstring>
#include <thread>
#include <cerrno>
#include <system_error>
#include "fast_define.h"
#include "message_dispatcher.h"
#include "fast_rdma_endpoint.h"

namespace fast {

void MessageDispatcher::SetHandler(MessageHandler handler, void* arg) {
    _handler = std::move(handler);
    _arg     = arg;
}

int MessageDispatcher::CutInputMessage(IOBuf& read_buf, IOBuf& frame) {
    if (read_buf.length() < 4) return 0;

    // The fixed header can straddle IOBuf blocks. Read without consuming it.
    IOBufAsZeroCopyInputStream input(read_buf);
    uint32_t be = 0;
    size_t copied = 0;
    const void* data;
    int size;
    while (copied < sizeof(be) && input.Next(&data, &size)) {
        const size_t n = std::min(sizeof(be) - copied, static_cast<size_t>(size));
        memcpy(reinterpret_cast<char*>(&be) + copied, data, n);
        copied += n;
    }
    const uint32_t total_len = ntohl(be);

    if (total_len < kFrameHeaderBytes) { errno = EPROTO; return -1; }
    if (read_buf.length() < total_len) return 0;

    read_buf.cutn(&frame, total_len);
    return 1;
}

int MessageDispatcher::ProcessNewMessage(IOBuf& read_buf, FastRdmaEndpoint* endpoint) {
    if (!_handler) return 0;

    int count = 0;
    while (true) {
        IOBuf frame;
        const int rc = CutInputMessage(read_buf, frame);
        if (rc < 0) return -1;
        if (rc == 0) break;

        auto handler = _handler;
        auto arg     = _arg;
        if (endpoint->Failed()) { errno = endpoint->error(); return -1; }
        EndpointUniquePtr reference;
        endpoint->ReAddress(&reference);
        try {
            std::thread([handler, arg, reference = std::move(reference)](IOBuf f) mutable {
                handler(f, arg);
            }, std::move(frame)).detach();
        } catch (const std::system_error& e) {
            errno = e.code().value();
            return -1;
        }

        ++count;
    }
    return count;
}

}  // namespace fast
