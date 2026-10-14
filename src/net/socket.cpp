#include "osats/net/socket.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

namespace osats::net {
namespace {

[[noreturn]] void throwErrno(const std::string& what) {
  throw std::runtime_error(what + ": " + std::strerror(errno));
}

}  // namespace

Socket Socket::listenTcp(std::uint16_t port, int backlog) {
  Socket s(::socket(AF_INET, SOCK_STREAM, 0));
  if (!s.valid()) throwErrno("socket");
  s.setReuseAddr();
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  if (::bind(s.fd(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    throwErrno("bind");
  }
  if (::listen(s.fd(), backlog) < 0) throwErrno("listen");
  s.setNonBlocking();
  return s;
}

Socket Socket::connectTcp(const std::string& host, std::uint16_t port) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* result = nullptr;
  const auto port_str = std::to_string(port);
  const int rc = ::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result);
  if (rc != 0) {
    throw std::runtime_error(std::string("getaddrinfo: ") + gai_strerror(rc));
  }

  Socket s;
  for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
    const int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;
    if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
      s = Socket(fd);
      break;
    }
    ::close(fd);
  }
  ::freeaddrinfo(result);
  if (!s.valid()) {
    throw std::runtime_error("connect failed to " + host + ":" + port_str);
  }
  s.setNoDelay();
  return s;
}

Socket Socket::accept() {
  sockaddr_storage addr{};
  socklen_t len = sizeof(addr);
  const int fd = ::accept(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
  if (fd < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) return Socket{};
    throwErrno("accept");
  }
  Socket client(fd);
  client.setNonBlocking();
  client.setNoDelay();
  return client;
}

void Socket::setNonBlocking() {
  const int flags = ::fcntl(fd_, F_GETFL, 0);
  if (flags < 0) throwErrno("fcntl get");
  if (::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) throwErrno("fcntl set");
}

void Socket::setNoDelay() {
  int one = 1;
  if (::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) < 0) {
    throwErrno("TCP_NODELAY");
  }
}

void Socket::setReuseAddr() {
  int one = 1;
  if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
    throwErrno("SO_REUSEADDR");
  }
}

int Socket::recvSome(char* buf, std::size_t n) {
  const ssize_t got = ::recv(fd_, buf, n, 0);
  if (got > 0) return static_cast<int>(got);
  if (got == 0) return 0;
  if (errno == EAGAIN || errno == EWOULDBLOCK) return -1;
  if (errno == EINTR) return -1;
  throwErrno("recv");
}

int Socket::sendSome(const char* buf, std::size_t n) {
  const ssize_t sent = ::send(fd_, buf, n, MSG_NOSIGNAL);
  if (sent >= 0) return static_cast<int>(sent);
  if (errno == EAGAIN || errno == EWOULDBLOCK) return -1;
  if (errno == EINTR) return -1;
  throwErrno("send");
}

void Socket::close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

}  // namespace osats::net
