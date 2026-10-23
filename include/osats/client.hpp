#pragma once

#include "osats/net/framed_connection.hpp"
#include "osats/types.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace osats {

struct Peer {
  EntityId id = kInvalidEntityId;
  std::string callsign;
  std::string real_name;
  Role role = Role::Pilot;
  Position position;
  std::uint32_t radio_hz = 122800;
  std::optional<FlightPlan> flight_plan;
  // Server steady-clock ms when this position was received. Zero if the
  // sender did not stamp the frame.
  std::uint64_t server_time_ms = 0;
  // Local steady-clock ms when this client parsed the frame. peers() coasts
  // from here; the stored sample itself is not moved.
  std::uint64_t received_ms = 0;
};

struct TextEvent {
  TextScope scope{};
  std::string from;
  std::string to;
  std::string text;
};

class Client {
 public:
  using PeerCallback = std::function<void(const Peer&)>;
  using RemoveCallback = std::function<void(EntityId)>;
  using ErrorCallback = std::function<void(ErrorCode, const std::string&)>;
  using TextCallback = std::function<void(const TextEvent&)>;
  using MetarCallback = std::function<void(const std::string& icao, const std::string& metar)>;
  using ReplyCallback = std::function<void(QueryType, const std::string& target,
                                           const std::string& body)>;
  using AnnounceCallback = std::function<void(const std::string& name,
                                              const std::string& version)>;

  Client() = default;
  ~Client();

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  void connect(const std::string& host, std::uint16_t port,
               const std::string& callsign, Role role = Role::Pilot,
               const std::string& real_name = "");
  void disconnect();
  bool connected() const { return connected_.load(); }

  EntityId entityId() const { return entity_id_.load(); }
  std::string serverName() const;

  void sendPosition(const Position& pos, double visibility_nm = 50.0);
  void sendHeartbeat();
  void sendFlightPlan(const FlightPlan& fp);
  void sendText(TextScope scope, const std::string& to_or_freq, const std::string& text);
  void tuneRadio(std::uint32_t radio_hz);
  void ping(std::uint32_t nonce = 1);
  void query(QueryType type, const std::string& target);
  void requestMetar(const std::string& icao);

  void onPeerUpdate(PeerCallback cb) { peer_cb_ = std::move(cb); }
  void onPeerRemove(RemoveCallback cb) { remove_cb_ = std::move(cb); }
  void onError(ErrorCallback cb) { error_cb_ = std::move(cb); }
  void onText(TextCallback cb) { text_cb_ = std::move(cb); }
  void onMetar(MetarCallback cb) { metar_cb_ = std::move(cb); }
  void onReply(ReplyCallback cb) { reply_cb_ = std::move(cb); }
  void onAnnounce(AnnounceCallback cb) { announce_cb_ = std::move(cb); }

  std::unordered_map<EntityId, Peer> peers() const;
  std::vector<TextEvent> drainTexts();

 private:
  void ioLoop();
  void handleFrame(const DecodedFrame& frame);
  void enqueue(std::vector<char> frame);

  std::unique_ptr<net::FramedConnection> conn_;
  std::thread io_thread_;
  std::atomic<bool> connected_{false};
  std::atomic<bool> stop_{false};
  std::atomic<bool> announced_{false};
  std::atomic<EntityId> entity_id_{kInvalidEntityId};
  std::string callsign_;
  mutable std::mutex mu_;
  std::string server_name_;
  std::unordered_map<EntityId, Peer> peers_;
  std::vector<TextEvent> texts_;

  PeerCallback peer_cb_;
  RemoveCallback remove_cb_;
  ErrorCallback error_cb_;
  TextCallback text_cb_;
  MetarCallback metar_cb_;
  ReplyCallback reply_cb_;
  AnnounceCallback announce_cb_;
};

}  // namespace osats
