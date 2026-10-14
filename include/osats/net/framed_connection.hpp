#pragma once

#include "osats/net/socket.hpp"
#include "osats/protocol.hpp"

#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

namespace osats::net {

class FramedConnection {
 public:
  explicit FramedConnection(Socket sock) : socket_(std::move(sock)) {}

  int fd() const { return socket_.fd(); }
  bool valid() const { return socket_.valid(); }

  bool wantsWrite() const {
    std::lock_guard lock(mu_);
    return !outbox_.empty();
  }

  void enqueue(std::vector<char> frame) {
    std::lock_guard lock(mu_);
    outbox_.push_back(std::move(frame));
  }

  bool readable(const std::function<void(const DecodedFrame&)>& on_frame);
  bool writable();

  void close() {
    std::lock_guard lock(mu_);
    socket_.close();
    outbox_.clear();
    inbox_.clear();
    out_offset_ = 0;
  }

  std::size_t outboundBytes() const {
    std::lock_guard lock(mu_);
    std::size_t n = 0;
    for (const auto& f : outbox_) n += f.size();
    return n;
  }

 private:
  mutable std::mutex mu_;
  Socket socket_;
  std::vector<char> inbox_;
  std::deque<std::vector<char>> outbox_;
  std::size_t out_offset_ = 0;
};

}  // namespace osats::net
