#include "osats/net/framed_connection.hpp"

#include <stdexcept>

namespace osats::net {

bool FramedConnection::readable(
    const std::function<void(const DecodedFrame&)>& on_frame) {
  char buf[8192];
  while (true) {
    int n = 0;
    try {
      n = socket_.recvSome(buf, sizeof(buf));
    } catch (const ConnectionClosed&) {
      return false;
    }
    if (n == 0) return false;
    if (n < 0) break;
    inbox_.insert(inbox_.end(), buf, buf + n);

    while (true) {
      DecodedFrame frame;
      std::size_t consumed = 0;
      try {
        if (!tryDecodeFrame(inbox_.data(), inbox_.size(), frame, consumed)) {
          break;
        }
      } catch (const std::runtime_error&) {
        return false;
      }
      on_frame(frame);
      inbox_.erase(inbox_.begin(),
                   inbox_.begin() + static_cast<std::ptrdiff_t>(consumed));
    }

    if (inbox_.size() > (1u << 20)) return false;
  }
  return true;
}

bool FramedConnection::writable() {
  std::lock_guard lock(mu_);
  while (!outbox_.empty()) {
    auto& front = outbox_.front();
    int n = 0;
    try {
      n = socket_.sendSome(front.data() + out_offset_, front.size() - out_offset_);
    } catch (const ConnectionClosed&) {
      return false;
    }
    if (n < 0) return true;
    out_offset_ += static_cast<std::size_t>(n);
    if (out_offset_ < front.size()) return true;
    outbox_.pop_front();
    out_offset_ = 0;
  }
  return true;
}

}  // namespace osats::net
