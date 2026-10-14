#pragma once

#include "osats/net/socket.hpp"
#include "osats/protocol.hpp"

#include <cstddef>
#include <deque>
#include <functional>
#include <vector>

namespace osats::net {

class FramedConnection {
 public:
  explicit FramedConnection(Socket sock) : socket_(std::move(sock)) {}

  int fd() const { return socket_.fd(); }
  bool valid() const { return socket_.valid(); }
  bool wantsWrite() const { return !outbox_.empty(); }

  void enqueue(std::vector<char> frame) {
    outbox_.push_back(std::move(frame));
  }

  // Read available bytes; invoke on_frame for each complete frame.
  // Returns false if peer closed.
  bool readable(const std::function<void(const DecodedFrame&)>& on_frame);

  // Flush outbound queue; returns false on hard failure.
  bool writable();

  void close() { socket_.close(); outbox_.clear(); inbox_.clear(); }

  std::size_t outboundBytes() const {
    std::size_t n = 0;
    for (const auto& f : outbox_) n += f.size();
    return n;
  }

 private:
  Socket socket_;
  std::vector<char> inbox_;
  std::deque<std::vector<char>> outbox_;
  std::size_t out_offset_ = 0;
};

}  // namespace osats::net
