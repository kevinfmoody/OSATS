#pragma once

#include "osats/net/framed_connection.hpp"
#include "osats/types.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace osats {

struct Peer {
  EntityId id = kInvalidEntityId;
  std::string callsign;
  Role role = Role::Pilot;
  Position position;
};

class Client {
 public:
  using PeerCallback = std::function<void(const Peer&)>;
  using RemoveCallback = std::function<void(EntityId)>;
  using ErrorCallback = std::function<void(ErrorCode, const std::string&)>;

  Client() = default;
  ~Client();

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  void connect(const std::string& host, std::uint16_t port,
               const std::string& callsign, Role role = Role::Pilot);
  void disconnect();
  bool connected() const { return connected_.load(); }

  EntityId entityId() const { return entity_id_.load(); }

  void sendPosition(const Position& pos, double visibility_nm = 50.0);
  void sendHeartbeat();

  void onPeerUpdate(PeerCallback cb) { peer_cb_ = std::move(cb); }
  void onPeerRemove(RemoveCallback cb) { remove_cb_ = std::move(cb); }
  void onError(ErrorCallback cb) { error_cb_ = std::move(cb); }

  std::unordered_map<EntityId, Peer> peers() const;

 private:
  void ioLoop();
  void handleFrame(const DecodedFrame& frame);

  std::unique_ptr<net::FramedConnection> conn_;
  std::thread io_thread_;
  std::atomic<bool> connected_{false};
  std::atomic<bool> stop_{false};
  std::atomic<EntityId> entity_id_{kInvalidEntityId};
  std::string callsign_;

  mutable std::mutex peers_mu_;
  std::unordered_map<EntityId, Peer> peers_;

  PeerCallback peer_cb_;
  RemoveCallback remove_cb_;
  ErrorCallback error_cb_;
};

}  // namespace osats
