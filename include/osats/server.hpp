#pragma once

#include "osats/net/framed_connection.hpp"
#include "osats/world.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace osats {

struct ServerConfig {
  std::uint16_t port = kDefaultListenPort;
  std::uint16_t tick_hz = 20;
  std::size_t max_outbound_bytes = 1 << 20;  // per session soft limit
  double default_visibility_nm = 50.0;
  bool verbose = false;
};

class Server {
 public:
  explicit Server(ServerConfig config);
  ~Server();

  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  void run();
  void requestStop();

  WorldStats stats() const { return world_.stats(); }

 private:
  struct Session {
    EntityId entity_id = kInvalidEntityId;
    bool authed = false;
    bool pending_disconnect = false;
    std::string callsign;
    Role role = Role::Pilot;
    std::uint64_t last_heartbeat_ms = 0;
    std::unique_ptr<net::FramedConnection> conn;
  };

  void acceptNew();
  void pollOnce(int timeout_ms);
  void onFrame(Session& session, const DecodedFrame& frame);
  void handleHello(Session& session, const DecodedFrame& frame);
  void handlePosition(Session& session, const DecodedFrame& frame);
  void handleHeartbeat(Session& session, const DecodedFrame& frame);
  void disconnect(Session& session, const std::string& reason);
  void broadcastPeerUpdate(EntityId subject);
  void sendTo(EntityId id, std::vector<char> frame);
  static std::uint64_t nowMs();

  ServerConfig config_;
  net::Socket listener_;
  World world_;
  std::unordered_map<int, std::unique_ptr<Session>> sessions_by_fd_;
  std::unordered_map<EntityId, Session*> sessions_by_id_;
  std::atomic<bool> stop_{false};
  std::uint64_t last_stats_log_ms_ = 0;
};

}  // namespace osats
