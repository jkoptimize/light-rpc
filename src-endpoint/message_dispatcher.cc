#include <arpa/inet.h>
#include <thread>
#include "message_dispatcher.h"

namespace fast {

void MessageDispatcher::SetHandler(MessageHandler handler, void* arg) {
    _handler = std::move(handler);
    _arg     = arg;
}

bool MessageDispatcher::CutInputMessage(IOBuf& read_buf, IOBuf& frame) {
    if (read_buf.length() < 4) return false;

    const void* start = read_buf.fetch1();
    uint32_t total_len = ntohl(*static_cast<const uint32_t*>(start));

    if (total_len < 4 || read_buf.length() < total_len) return false;

    read_buf.cutn(&frame, total_len);
    return true;
}

int MessageDispatcher::ProcessNewMessage(IOBuf& read_buf) {
    if (!_handler) return 0;

    int count = 0;
    while (true) {
        IOBuf frame;
        if (!CutInputMessage(read_buf, frame)) break;

        auto handler = _handler;
        auto arg     = _arg;
        std::thread([handler, arg](IOBuf f) mutable {
            handler(f, arg);
        }, std::move(frame)).detach();

        ++count;
    }
    return count;
}

}  // namespace fast
