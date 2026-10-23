#pragma once

#include "osats/http_api.hpp"
#include "osats/net/framed_connection.hpp"
#include "osats/weather.hpp"
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
  std::uint16_t http_port = kDefaultHttpPort;
  std::uint16_t tick_hz = 20;
  std::size_t max_outbound_bytes = 1 << 20;
  double default_visibility_nm = 50.0;
  std::uint64_t idle_timeout_ms = 60000;
  bool verbose = false;
  bool enable_http = true;
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
    std::string real_name;
    Role role = Role::Pilot;
    std::uint64_t last_heartbeat_ms = 0;
    std::unique_ptr<net::FramedConnection> conn;
  };

  void acceptNew();
  void pollOnce(int timeout_ms);
  void shutdownSessions();
  void sweepIdle(std::uint64_t now_ms);
  void onFrame(Session& session, const DecodedFrame& frame);
  void handleHello(Session& session, const DecodedFrame& frame);
  void handlePosition(Session& session, const DecodedFrame& frame);
  void handleHeartbeat(Session& session, const DecodedFrame& frame);
  void handleFlightPlan(Session& session, const DecodedFrame& frame);
  void handleText(Session& session, const DecodedFrame& frame);
  void handleRadioTune(Session& session, const DecodedFrame& frame);
  void handlePing(Session& session, const DecodedFrame& frame);
  void handleQuery(Session& session, const DecodedFrame& frame);
  void handleMetar(Session& session, const DecodedFrame& frame);
  void disconnect(Session& session, const std::string& reason);
  void broadcastPeerUpdate(EntityId subject, bool force = false);
  void publishContact(EntityId subject, EntityId other);
  void retractContact(EntityId subject, EntityId other);
  void sendTo(EntityId id, std::vector<char> frame);
  void requireAuth(Session& session);
  static std::uint64_t nowMs();

  ServerConfig config_;
  net::Socket listener_;
  World world_;
  WeatherService weather_;
  std::unique_ptr<HttpApi> http_;
  std::unordered_map<int, std::unique_ptr<Session>> sessions_by_fd_;
  std::unordered_map<EntityId, Session*> sessions_by_id_;
  std::atomic<bool> stop_{false};
  std::uint64_t last_stats_log_ms_ = 0;
};

}  // namespace osats
