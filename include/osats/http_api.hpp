#pragma once

#include "osats/net/socket.hpp"
#include "osats/world.hpp"

#include <poll.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace osats {

// Minimal HTTP/1.1 status + live map API, multiplexed into the server poll loop.
class HttpApi {
 public:
  using SnapshotFn = std::function<std::string()>;  // JSON traffic snapshot
  using StatsFn = std::function<std::string()>;     // JSON status

  HttpApi(std::uint16_t port, SnapshotFn traffic, StatsFn status);
  int listenFd() const { return listener_.fd(); }

  void acceptNew();
  // Returns fds that should be polled.
  void collectPollFds(std::vector<pollfd>& pfds) const;
  void onReadable(int fd);
  void onWritable(int fd);
  void drop(int fd);

 private:
  struct Conn {
    net::Socket sock;
    std::string in;
    std::string out;
    std::size_t out_off = 0;
    bool closing = false;
  };

  void handleRequest(Conn& c, const std::string& req);
  static std::string httpResponse(int code, const char* content_type,
                                  const std::string& body);

  net::Socket listener_;
  SnapshotFn traffic_;
  StatsFn status_;
  std::unordered_map<int, std::unique_ptr<Conn>> conns_;
};

std::string jsonEscape(std::string_view s);
std::string buildTrafficJson(const World& world);
std::string buildStatusJson(const World& world, std::uint16_t port,
                            std::uint16_t http_port, std::size_t sessions);
std::string liveMapHtml();

}  // namespace osats
