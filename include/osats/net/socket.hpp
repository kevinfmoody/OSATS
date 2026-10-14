#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace osats::net {

class Socket {
 public:
  Socket() = default;
  explicit Socket(int fd) : fd_(fd) {}
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  Socket(Socket&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
  Socket& operator=(Socket&& other) noexcept {
    if (this != &other) {
      close();
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }
  ~Socket() { close(); }

  static Socket listenTcp(std::uint16_t port, int backlog = 512);
  static Socket connectTcp(const std::string& host, std::uint16_t port);

  Socket accept();
  void setNonBlocking();
  void setNoDelay();
  void setReuseAddr();

  // Returns bytes read, 0 on EOF, -1 on would-block, throws on hard error.
  int recvSome(char* buf, std::size_t n);
  // Returns bytes written, -1 on would-block, throws on hard error.
  int sendSome(const char* buf, std::size_t n);

  void close();
  int fd() const { return fd_; }
  bool valid() const { return fd_ >= 0; }

 private:
  int fd_ = -1;
};

}  // namespace osats::net
