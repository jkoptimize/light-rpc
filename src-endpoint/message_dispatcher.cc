#include <arpa/inet.h>
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

bool MessageDispatcher::CutInputMessage(IOBuf& read_buf, IOBuf& frame) {
    if (read_buf.length() < 4) return false;

    const void* start = read_buf.fetch1();
    uint32_t total_len = ntohl(*static_cast<const uint32_t*>(start));

    if (total_len < kFrameHeaderBytes || read_buf.length() < total_len) return false;

    read_buf.cutn(&frame, total_len);
    return true;
}

int MessageDispatcher::ProcessNewMessage(IOBuf& read_buf, FastRdmaEndpoint* endpoint) {
    if (!_handler) return 0;

    int count = 0;
    while (true) {
        IOBuf frame;
        if (!CutInputMessage(read_buf, frame)) break;

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
